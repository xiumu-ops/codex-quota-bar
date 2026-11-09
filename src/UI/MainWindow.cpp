#include "UI/MainWindow.h"
#include "Core/Logger.h"
#include "UI/AppearanceDialog.h"
#include "UI/CustomMenu.h"
#include "Services/CodexClient.h"
#include "Services/CompanionMode.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Layout.h"
#include "Core/Settings.h"
#include "Core/ConfigStore.h"
#include "UI/WindowEffects.h"
#include "UI/TaskbarPlacement.h"
#include "resources/resource.h"

#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cwchar>
#include <system_error>
#include <limits>
#include <thread>

namespace CodexQuotaBar {

    constexpr UINT_PTR TIMER_REFRESH_ID = 1001;
    constexpr UINT_PTR TIMER_COMPANION_ID = 1002;
    constexpr UINT_PTR TIMER_RENDER_RETRY_ID = 1003;
    constexpr UINT_PTR TIMER_WEATHER_LAYOUT_ID = 1004;

    constexpr int IDM_TOGGLE_EXPAND = 2001;
    constexpr int IDM_REFRESH = 2002;
    constexpr int IDM_EXIT = 2003;
    constexpr int IDM_COMPANION_MODE = 2004;
    constexpr int IDM_ALWAYS_ON_TOP = 2005;
    constexpr int IDM_MINI_MODE = 2006;
    constexpr int IDM_COMPONENT_MODE = 2007;
    constexpr int IDM_REFRESH_INTERVAL_SUB = 2200;
    constexpr int IDM_REFRESH_INTERVAL_BASE = 2201; // 二级菜单：刷新档位 id 基址（+0..4）
    constexpr int IDM_APPEARANCE_SUB = 2300;
    constexpr int IDM_APPEARANCE_DEFAULT = 2301;
    constexpr int IDM_APPEARANCE_CUSTOM = 2302;
    constexpr int IDM_APPEARANCE_EDIT = 2303;
    constexpr int IDM_FONT_SUB = 2400;
    constexpr int IDM_FONT_YAHEI_UI = 2401;
    constexpr int IDM_FONT_CHOOSE_SYSTEM = 2498;
    constexpr int IDM_FONT_CUSTOM_DIALOG = 2499;

    namespace {
        constexpr UINT kCompanionPollMs = 2000;
        constexpr int kRenderRetryLimit = 3;
        // 数值 → 最接近档位索引（配置读回时归一）
        int ClosestScaleLevel(float value) {
            int best = DEFAULT_USER_SCALE_LEVEL;
            float bestDiff = 1e9f;
            for (std::size_t i = 0; i < USER_SCALE_LEVELS.size(); ++i) {
                const float diff = std::fabs(USER_SCALE_LEVELS[i] - value);
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = static_cast<int>(i);
                }
            }
            return best;
        }

        int ClosestRefreshIntervalLevel(int minutes) {
            int best = 0;
            int bestDiff = (std::numeric_limits<int>::max)();
            for (std::size_t i = 0; i < REFRESH_INTERVAL_MINUTES.size(); ++i) {
                const int diff = std::abs(REFRESH_INTERVAL_MINUTES[i] - minutes);
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = static_cast<int>(i);
                }
            }
            return best;
        }

        std::wstring RefreshIntervalText(int level) {
            return std::to_wstring(
                REFRESH_INTERVAL_MINUTES[static_cast<std::size_t>(level)]) + L" 分钟";
        }
    }

    MainWindow::MainWindow()
        : m_renderer(std::make_unique<Direct2DRenderer>())
    {
    }

    MainWindow::~MainWindow() {
        BeginShutdown();
        m_fetchWorker.Join();
        m_companionWorker.Join();
        m_weatherWorker.Join();
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = NULL;
        }
    }

    LRESULT CALLBACK MainWindow::WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
            MainWindow* pThis = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
            pThis->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWindow::WndProcThunk));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MainWindow* pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (pThis) {
            return pThis->HandleMessage(msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool MainWindow::Create() {
        HINSTANCE hInstance = GetModuleHandleW(NULL);

        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &MainWindow::WndProcSetup;
        wc.hInstance = hInstance;
        wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"Codex-Quota-Bar_Window";

        if (RegisterClassExW(&wc) == 0) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }
        }

        // 读取用户设置（提前读取以精确确定初始窗口几何尺寸与显示模式）
        std::wstring settingsErrors;
        m_settings = ConfigStore::LoadSettings(&settingsErrors);
        m_userScaleLevel = ClosestScaleLevel(m_settings.userScale);
        m_refreshIntervalLevel = ClosestRefreshIntervalLevel(m_settings.refreshIntervalMinutes);
        m_companionMode = m_settings.companionMode;
        m_codexDesktopRunning = false;

        const SIZE initialLogical = LogicalWindowSize();
        int initW = Scale(static_cast<float>(initialLogical.cx), 1.0f);
        int initH = Scale(static_cast<float>(initialLogical.cy), 1.0f);

        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            wc.lpszClassName,
            L"Codex-Quota-Bar",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, initW, initH,
            NULL, NULL, hInstance, this);

        if (!m_hwnd) return false;

        // 与右键菜单统一使用 DWM 原生投影；低缩放迷你栏使用小圆角档位。
        ApplyCurrentWindowEffects();

        // 初始化 Direct2D 渲染器
        HRESULT rendererHr = m_renderer->Initialize(m_hwnd);
        if (FAILED(rendererHr)) {
            DestroyWindow(m_hwnd);
            m_hwnd = NULL;
            return false;
        }

        ApplyWindowZOrder();
        if (m_companionMode) {
            CompanionMode::ConfigureAutoStart(true);
        }
        ApplyAppearance(settingsErrors);

        m_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        // 获取当前屏幕 DPI 并确定初始窗口坐标，再统一完成缩放渲染与分层提交
        const UINT dpi = GetDpiForWindow(m_hwnd);
        m_dpiScale = dpi == 0 ? 1.0f : static_cast<float>(dpi) / BASE_DPI;
        m_uiScale = m_dpiScale * USER_SCALE_LEVELS[
            static_cast<std::size_t>(m_userScaleLevel)];

        const SIZE initialPhysical = PhysicalWindowSize();
        POINT initialPosition = {};
        auto stored = ConfigStore::LoadState();
        if (stored.hasPosition) {
            initialPosition = ClampToScreens(stored.position, initialPhysical);
        } else {
            RECT rcWork = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
            const int x = rcWork.left +
                (rcWork.right - rcWork.left - initialPhysical.cx) / 2;
            const int y = rcWork.top + 34;
            initialPosition = ClampToScreens({ x, y }, initialPhysical);
        }
        m_collapsedLocation = initialPosition;
        m_hasCollapsedLocation = true;
        SetWindowPos(
            m_hwnd, NULL,
            initialPosition.x, initialPosition.y,
            initialPhysical.cx, initialPhysical.cy,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);

        // Moving to the saved monitor can synchronously deliver WM_DPICHANGED.
        // Query again so the initial render uses the window's destination DPI.
        const UINT destinationDpi = GetDpiForWindow(m_hwnd);
        if (destinationDpi != 0) {
            m_dpiScale = static_cast<float>(destinationDpi) / BASE_DPI;
        }
        ApplyUiScale();

        // 自动刷新定时器
        SetTimer(m_hwnd, TIMER_REFRESH_ID,
                 static_cast<UINT>(REFRESH_INTERVAL_MINUTES[
                     static_cast<std::size_t>(m_refreshIntervalLevel)] * 60 * 1000), NULL);
        if (m_companionMode) {
            SetTimer(m_hwnd, TIMER_COMPANION_ID, kCompanionPollMs, NULL);
            PollCompanionMode();
        }

        if (!m_companionMode || m_codexDesktopRunning) {
            RefreshQuota();
        }

        return true;
    }

    void MainWindow::Show() {
        if (m_hwnd) {
            if (m_companionMode && !m_codexDesktopRunning) return;
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            if (IsWeatherDockActive()) ApplyWindowZOrder();
            RenderLayeredWindow();
        }
    }

    void MainWindow::Hide() {
        if (m_hwnd) {
            CustomMenu::Dismiss(m_hwnd);
            ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    void MainWindow::ToggleVisible() {
        if (m_hwnd) {
            if (IsWindowVisible(m_hwnd)) Hide();
            else Show();
        }
    }

    void MainWindow::ApplyUiScale(const POINT* requestedPosition) {
        const float userScale = USER_SCALE_LEVELS[
            static_cast<std::size_t>(m_userScaleLevel)];
        m_uiScale = m_dpiScale * userScale;
        m_renderer->SetScales(m_dpiScale, userScale);

        const SIZE physical = PhysicalWindowSize();
        const int targetW = physical.cx;
        const int targetH = physical.cy;
        // 必须先更新渲染器目标尺寸并丢弃旧位图，防止 SetWindowPos 期间重入绘制把旧尺寸写回分层窗口。
        m_renderer->Resize(
            static_cast<UINT>(targetW),
            static_cast<UINT>(targetH));

        if (IsWeatherDockActive() && !m_dragging && !requestedPosition) {
            DockToWeatherWidget();
            return;
        }

        RECT rc = {};
        GetWindowRect(m_hwnd, &rc);
        const POINT anchor = requestedPosition ? *requestedPosition : POINT{ rc.left, rc.top };
        POINT clamped = {};
        if (m_dragging) {
            clamped = ClampToScreens(anchor, physical, m_settings.miniMode && !m_expanded);
        } else if (m_expanded && m_hasCollapsedLocation) {
            m_collapsedLocation = ClampToScreens(
                m_collapsedLocation, CollapsedPhysicalWindowSize());
            clamped = ClampToScreens(requestedPosition ? anchor : m_collapsedLocation, physical);
        } else {
            clamped = ClampToScreens(anchor, physical);
            m_collapsedLocation = clamped;
            m_hasCollapsedLocation = true;
        }

        SetWindowPos(
            m_hwnd, NULL, clamped.x, clamped.y, targetW, targetH,
            SWP_NOZORDER | SWP_NOACTIVATE |
            SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_FRAMECHANGED);
        ApplyCurrentWindowEffects();
        RenderLayeredWindow();
        DwmFlush();
        if (IsWeatherDockActive() && !m_dragging) ScheduleWeatherLayout();
    }

    void MainWindow::ApplyUserScale(int level) {
        if (level < 0 || level >= static_cast<int>(USER_SCALE_LEVELS.size())) return;
        m_userScaleLevel = level;
        m_settings.userScale = USER_SCALE_LEVELS[static_cast<std::size_t>(level)];
        SaveSettingsWithFeedback();
        ApplyUiScale();
        if (m_hasCollapsedLocation) {
            ConfigStore::SavePosition(m_collapsedLocation);
        }
    }

    void MainWindow::ApplyRefreshInterval(int level) {
        if (level < 0 ||
            level >= static_cast<int>(REFRESH_INTERVAL_MINUTES.size()) ||
            !m_hwnd) return;
        m_refreshIntervalLevel = level;
        SetTimer(m_hwnd, TIMER_REFRESH_ID,
                 static_cast<UINT>(REFRESH_INTERVAL_MINUTES[
                     static_cast<std::size_t>(level)] * 60 * 1000), NULL);
        m_settings.refreshIntervalMinutes =
            REFRESH_INTERVAL_MINUTES[static_cast<std::size_t>(level)];
        SaveSettingsWithFeedback();
    }

    bool MainWindow::SaveSettingsWithFeedback(bool preserveDiskAppearance) {
        AppSettings settingsToSave = m_settings;
        if (preserveDiskAppearance) {
            std::wstring loadErrors;
            bool readable = false;
            const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
            if (!readable) {
                MessageBoxW(
                    m_hwnd,
                    (L"配置未保存，请先修正配置文件：\n\n" + loadErrors).c_str(),
                    L"Codex-Quota-Bar 配置错误",
                    MB_OK | MB_ICONWARNING);
                return false;
            }
            settingsToSave.appearance = loaded.appearance;
            settingsToSave.defaultAppearance = loaded.defaultAppearance;
        }

        std::wstring errors;
        if (ConfigStore::SaveSettings(settingsToSave, &errors)) {
            if (preserveDiskAppearance) {
                const auto& previous = m_settings.appearance;
                const auto& saved = settingsToSave.appearance;
                const auto& oldDefault = m_settings.defaultAppearance;
                const auto& newDefault = settingsToSave.defaultAppearance;
                const bool changed = previous.mode != saved.mode ||
                    previous.fontFamily != saved.fontFamily ||
                    previous.backgroundTransparency != saved.backgroundTransparency ||
                    previous.colors != saved.colors ||
                    oldDefault.fontFamily != newDefault.fontFamily ||
                    oldDefault.backgroundTransparency != newDefault.backgroundTransparency ||
                    oldDefault.colors != newDefault.colors;
                m_settings.appearance = saved;
                m_settings.defaultAppearance = newDefault;
                if (changed) ApplyAppearance();
            }
            return true;
        }
        MessageBoxW(
            m_hwnd,
            (L"配置未保存，请修正配置文件：\n\n" + errors).c_str(),
            L"Codex-Quota-Bar 配置错误",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    void MainWindow::ApplyAppearance(const std::wstring& validationErrors) {
        const bool custom = m_settings.appearance.mode == AppearanceMode::Custom;
        const AppearanceSettings& visualAppearance = custom
            ? m_settings.appearance
            : m_settings.defaultAppearance;
        const ThemePalette palette = ThemePalette::Custom(visualAppearance);
        const std::wstring& fontFamily = visualAppearance.fontFamily;

        std::wstring fontError;
        m_renderer->SetAppearance(
            palette,
            fontFamily,
            visualAppearance.backgroundTransparency,
            &fontError);
        std::wstring errors = validationErrors;
        if (!fontError.empty()) {
            if (!errors.empty()) errors += L"\n";
            errors += fontError;
        }
        if (!errors.empty()) {
            MessageBoxW(
                m_hwnd,
                (L"部分外观配置未应用：\n\n" + errors).c_str(),
                L"Codex-Quota-Bar 配置校验",
                MB_OK | MB_ICONWARNING);
        }
        if (m_hwnd && IsWindowVisible(m_hwnd)) RenderLayeredWindow();
    }

    void MainWindow::SetAppearanceMode(AppearanceMode mode) {
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable) {
            MessageBoxW(
                m_hwnd,
                (L"无法切换外观，请先修正配置文件：\n\n" + loadErrors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }

        const AppearanceSettings previous = m_settings.appearance;
        const AppearanceSettings previousDefault = m_settings.defaultAppearance;
        m_settings.appearance = loaded.appearance;
        m_settings.defaultAppearance = loaded.defaultAppearance;
        m_settings.appearance.mode = mode;
        if (!SaveSettingsWithFeedback(false)) {
            m_settings.appearance = previous;
            m_settings.defaultAppearance = previousDefault;
            return;
        }
        ApplyAppearance();
    }

    void MainWindow::ConfigureCustomAppearance() {
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable) {
            MessageBoxW(
                m_hwnd,
                (L"无法修改个性外观，请先修正配置文件：\n\n" + loadErrors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }

        const auto applyAppearance = [this, loaded](
            const std::map<std::wstring, std::wstring>& colors,
            int backgroundTransparency,
            const std::wstring& fontFamily,
            int userScalePercent)
        {
            const AppearanceSettings previous = m_settings.appearance;
            const AppearanceSettings previousDefault = m_settings.defaultAppearance;
            const float previousScale = m_settings.userScale;
            const int previousScaleLevel = m_userScaleLevel;

            m_settings.appearance = loaded.appearance;
            m_settings.defaultAppearance = loaded.defaultAppearance;
            m_settings.appearance.colors = colors;
            m_settings.appearance.backgroundTransparency = backgroundTransparency;
            m_settings.appearance.fontFamily = fontFamily;
            m_settings.appearance.mode = AppearanceMode::Custom;

            const float newScale = static_cast<float>(userScalePercent) / 100.0f;
            m_settings.userScale = newScale;
            m_userScaleLevel = ClosestScaleLevel(newScale);

            if (!SaveSettingsWithFeedback(false)) {
                m_settings.appearance = previous;
                m_settings.defaultAppearance = previousDefault;
                m_settings.userScale = previousScale;
                m_userScaleLevel = previousScaleLevel;
                return false;
            }
            ApplyAppearance();
            ApplyUiScale();
            return true;
        };

        const std::wstring initialFont = !loaded.appearance.fontFamily.empty()
            ? loaded.appearance.fontFamily
            : m_renderer->FontFamily();

        const int initialScalePercent = std::clamp(
            static_cast<int>(std::round(m_settings.userScale * 100.0f)),
            MIN_USER_SCALE_PERCENT, MAX_USER_SCALE_PERCENT);

        ShowAppearanceDialog(
            m_hwnd,
            m_dpiScale,
            loaded.appearance.colors,
            loaded.appearance.backgroundTransparency,
            initialFont,
            initialScalePercent,
            m_renderer->Palette(),
            applyAppearance);
    }

    void MainWindow::ApplyFontFamily(const std::wstring& font) {
        if (font.empty()) return;
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable) {
            MessageBoxW(
                m_hwnd,
                (L"无法切换字体，请先修正配置文件：\n\n" + loadErrors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }

        const AppearanceSettings previous = m_settings.appearance;
        const AppearanceSettings previousDefault = m_settings.defaultAppearance;
        m_settings.appearance = loaded.appearance;
        m_settings.defaultAppearance = loaded.defaultAppearance;
        m_settings.appearance.fontFamily = font;
        m_settings.appearance.mode = AppearanceMode::Custom;
        if (!SaveSettingsWithFeedback(false)) {
            m_settings.appearance = previous;
            m_settings.defaultAppearance = previousDefault;
            return;
        }
        ApplyAppearance();
    }

    void MainWindow::ShowFontMenu(int screenX, int screenY) {
        constexpr wchar_t yaHeiFamily[] = L"Microsoft YaHei UI";
        const std::wstring currentFont = m_renderer->FontFamily();
        const bool usingYaHei = _wcsicmp(currentFont.c_str(), yaHeiFamily) == 0 ||
                               _wcsicmp(currentFont.c_str(), L"微软雅黑 UI") == 0;
        const std::vector<MenuItem> fontItems = {
            { IDM_FONT_YAHEI_UI, usingYaHei ? L"微软雅黑 UI (当前)" : L"微软雅黑 UI" },
            { 0, L"" },
            { IDM_FONT_CHOOSE_SYSTEM, L"浏览全部系统字体..." },
            { IDM_FONT_CUSTOM_DIALOG, L"自定义外观设置..." },
        };

        const int selected = CustomMenu::Show(
            m_hwnd, m_uiScale, screenX, screenY, fontItems,
            m_renderer->Palette(), currentFont);

        if (selected == IDM_FONT_YAHEI_UI) {
            ApplyFontFamily(yaHeiFamily);
        } else if (selected == IDM_FONT_CHOOSE_SYSTEM) {
            LOGFONTW lf = {};
            wcsncpy_s(lf.lfFaceName, currentFont.c_str(), _TRUNCATE);
            lf.lfCharSet = DEFAULT_CHARSET;
            CHOOSEFONTW cf = { sizeof(CHOOSEFONTW) };
            cf.hwndOwner = m_hwnd;
            cf.lpLogFont = &lf;
            cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
            if (ChooseFontW(&cf)) {
                ApplyFontFamily(lf.lfFaceName);
            }
        } else if (selected == IDM_FONT_CUSTOM_DIALOG) {
            ConfigureCustomAppearance();
        }
    }

    void MainWindow::ToggleAlwaysOnTop() {
        if (!m_hwnd || IsWeatherDockActive()) return;

        const bool previous = m_settings.alwaysOnTop;
        m_settings.alwaysOnTop = !previous;
        if (!SaveSettingsWithFeedback()) {
            m_settings.alwaysOnTop = previous;
            return;
        }

        ApplyWindowZOrder();
        SyncWindowPosition(false);
    }

    void MainWindow::ToggleMiniMode() {
        if (IsWeatherDockActive()) return;

        const bool previous = m_settings.miniMode;
        m_settings.miniMode = !previous;
        if (!SaveSettingsWithFeedback()) {
            m_settings.miniMode = previous;
            return;
        }

        if (m_settings.miniMode && m_expanded) {
            ToggleExpanded();
            return;
        }
        if (!m_expanded) {
            ApplyWindowZOrder();
            ApplyUiScale();
            if (m_hasCollapsedLocation) {
                ConfigStore::SavePosition(m_collapsedLocation);
            }
        }
    }

    bool MainWindow::SetComponentMode(bool enabled) {
        const bool previousDock = m_settings.dockToWeather;
        const bool previousMini = m_settings.miniMode;
        const bool previousTopmost = m_settings.alwaysOnTop;
        const bool previousExpanded = m_expanded;
        m_settings.SetComponentMode(enabled);
        if (enabled) m_expanded = false;

        if (!SaveSettingsWithFeedback()) {
            m_settings.dockToWeather = previousDock;
            m_settings.miniMode = previousMini;
            m_settings.alwaysOnTop = previousTopmost;
            m_expanded = previousExpanded;
            return false;
        }

        ApplyWindowZOrder();
        if (enabled) {
            ApplyUiScale();
        } else {
            SyncWindowPosition(true);
        }
        return true;
    }

    bool MainWindow::IsWeatherDockActive() const {
        return m_settings.dockToWeather;
    }

    void MainWindow::ApplyWindowZOrder() {
        const bool componentMode = IsWeatherDockActive();
        const bool topmost = m_settings.alwaysOnTop || componentMode;
        const bool currentlyTopmost = (GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (!componentMode && topmost == currentlyTopmost) return;
        SetWindowPos(m_hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void MainWindow::DockToWeatherWidget(bool waitForProbe) {
        if (!m_hwnd || !IsWeatherDockActive() || m_dragging) return;

        const SIZE sz = PhysicalWindowSize();
        const WeatherWidgetPlacement placement =
            GetWeatherPlacement(sz);
        if (waitForProbe && m_weatherMetrics.hTaskbar && m_weatherProbeState->inFlight) return;

        const POINT pos = placement.dockPosition;
        m_collapsedLocation = pos;
        m_hasCollapsedLocation = true;

        SetWindowPos(
            m_hwnd, HWND_TOPMOST,
            pos.x, pos.y,
            sz.cx, sz.cy,
            SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);

        ApplyCurrentWindowEffects();
        RenderLayeredWindow();
        DwmFlush();

        if (!m_weatherLayoutPending && !m_weatherProbeState->inFlight) {
            ConfigStore::SavePosition(m_collapsedLocation);
        }
    }

    void MainWindow::ScheduleWeatherLayout() {
        InvalidateWeatherProbe();
        if (!IsWeatherDockActive()) return;
        m_weatherLayoutPending = SetTimer(m_hwnd, TIMER_WEATHER_LAYOUT_ID, 120, nullptr) != 0;
        if (!m_weatherLayoutPending) DockToWeatherWidget(true);
    }

    void MainWindow::InvalidateWeatherProbe() {
        ++m_weatherGeneration;
        m_weatherProbeNeeded = true;
        m_weatherWidgetRect.reset();
    }

    WeatherWidgetPlacement MainWindow::GetWeatherPlacement(SIZE size) {
        RECT window = {};
        GetWindowRect(m_hwnd, &window);
        const POINT center = { window.left + (window.right - window.left) / 2,
                               window.top + (window.bottom - window.top) / 2 };
        const TaskbarMetrics metrics = TaskbarPlacement::GetTaskbarMetrics(center, m_dpiScale);
        if (metrics.hTaskbar != m_weatherMetrics.hTaskbar ||
            metrics.dpiScale != m_weatherMetrics.dpiScale ||
            !EqualRect(&metrics.taskbarRect, &m_weatherMetrics.taskbarRect) ||
            !EqualRect(&metrics.monitorRect, &m_weatherMetrics.monitorRect)) {
            InvalidateWeatherProbe();
        }
        m_weatherMetrics = metrics;
        auto state = m_weatherProbeState;
        if (metrics.hTaskbar && m_weatherProbeNeeded && !state->canceled &&
            !state->inFlight.exchange(true)) {
            try {
                const HWND hwnd = m_hwnd;
                const auto generation = m_weatherGeneration;
                m_weatherWorker.Submit([state, hwnd, metrics, generation](std::stop_token stop) {
                    const auto rect = TaskbarPlacement::ProbeWeatherWidget(metrics, stop);
                    const std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->canceled) return;
                    state->generation = generation;
                    state->result = rect;
                    if (!PostMessageW(hwnd, WM_CQB_WEATHER_RESULT, 0, 0)) {
                        state->inFlight = false;
                    }
                });
                m_weatherProbeNeeded = false;
            } catch (const std::system_error&) {
                state->inFlight = false;
            }
        }
        return TaskbarPlacement::FindWeatherWidgetPlacement(size, metrics, m_weatherWidgetRect);
    }

    void MainWindow::HandleWeatherProbeResult() {
        {
            const std::lock_guard<std::mutex> lock(m_weatherProbeState->mutex);
            if (m_weatherProbeState->generation == m_weatherGeneration) {
                m_weatherWidgetRect = m_weatherProbeState->result;
            }
            m_weatherProbeState->inFlight = false;
        }
        if (IsWeatherDockActive() && !m_dragging && !m_weatherLayoutPending) {
            DockToWeatherWidget(true);
        }
    }

    void MainWindow::ToggleCompanionMode() {
        const bool enabled = !m_companionMode;
        if (!CompanionMode::ConfigureAutoStart(enabled)) {
            MessageBoxW(
                m_hwnd,
                enabled ? L"无法创建当前用户开机启动项，伴随模式未启用。"
                        : L"无法移除当前用户开机启动项，伴随模式未关闭。",
                L"Codex-Quota-Bar",
                MB_OK | MB_ICONWARNING);
            return;
        }

        m_companionMode = enabled;
        m_settings.companionMode = m_companionMode;
        SaveSettingsWithFeedback();
        m_codexMissingPolls = 0;
        m_companionProbeFailures = 0;

        if (m_companionMode) {
            m_codexDesktopRunning = false;
            SetTimer(m_hwnd, TIMER_COMPANION_ID, kCompanionPollMs, NULL);
            Hide();
            PollCompanionMode();
        } else {
            KillTimer(m_hwnd, TIMER_COMPANION_ID);
            m_codexDesktopRunning = false;
            Show();
        }
    }

    void MainWindow::PollCompanionMode() {
        if (!m_companionMode || !m_hwnd) return;

        const auto state = m_companionPollState;
        if (state->canceled.load() || state->inFlight.exchange(true)) return;
        const HWND hwnd = m_hwnd.load();
        try {
            m_companionWorker.Submit([state, hwnd](std::stop_token stop) {
                auto processState = DesktopProcessState::QueryFailed;
                try {
                    processState = CompanionMode::ProbeDesktopState();
                } catch (...) {
                    WriteLog(LogLevel::Warning, L"伴随模式探测失败。");
                }
                const std::lock_guard<std::mutex> lock(state->mutex);
                if (stop.stop_requested() || state->canceled.load() ||
                    !PostMessageW(
                        hwnd,
                        WM_CQB_COMPANION_RESULT,
                        static_cast<WPARAM>(processState),
                        0)) {
                    state->inFlight = false;
                }
            });
        } catch (const std::system_error&) {
            state->inFlight = false;
            WriteLog(LogLevel::Warning, L"伴随模式检测线程创建失败。");
        }
    }

    void MainWindow::HandleCompanionResult(DesktopProcessState state) {
        m_companionPollState->inFlight = false;
        if (!m_companionMode || m_shuttingDown.load()) return;

        if (state == DesktopProcessState::QueryFailed) {
            if (m_companionProbeFailures++ == 0) {
                WriteLog(
                    LogLevel::Warning,
                    L"伴随模式暂时无法查询 Codex 桌面进程；保持当前显示状态。");
            }
            return;
        }
        m_companionProbeFailures = 0;

        if (state == DesktopProcessState::Running) {
            const bool wasRunning = m_codexDesktopRunning;
            m_codexDesktopRunning = true;
            m_codexMissingPolls = 0;
            if (!IsWindowVisible(m_hwnd)) {
                Show();
                if (!wasRunning) RefreshQuota();
            }
            return;
        }

        if (m_codexMissingPolls < COMPANION_MISSING_POLL_THRESHOLD) {
            ++m_codexMissingPolls;
        }
        if (m_codexMissingPolls >= COMPANION_MISSING_POLL_THRESHOLD) {
            m_codexDesktopRunning = false;
            if (IsWindowVisible(m_hwnd)) {
                WriteLog(
                    LogLevel::Info,
                    L"伴随模式连续三次未检测到 Codex 桌面端，隐藏额度栏。");
                Hide();
            }
        }
    }

    void MainWindow::OnDpiChanged(int newDpi, const RECT* suggestedRect) {
        m_dpiScale = newDpi > 0 ? static_cast<float>(newDpi) / BASE_DPI : 1.0f;
        const float uiScale = m_dpiScale * USER_SCALE_LEVELS[
            static_cast<std::size_t>(m_userScaleLevel)];
        POINT anchor = {};
        const POINT* requestedPosition = nullptr;
        if (suggestedRect) {
            anchor = { suggestedRect->left, suggestedRect->top };
            requestedPosition = &anchor;
        }
        POINT cursor = {};
        if (m_dragging && GetCursorPos(&cursor)) {
            m_lastDragCursor = cursor;
            const POINT dragAnchor = DragWindowPosition(cursor, uiScale);
            const SIZE size = PhysicalWindowSize(uiScale);
            const RECT dragRect = { dragAnchor.x, dragAnchor.y,
                                   dragAnchor.x + size.cx, dragAnchor.y + size.cy };
            // Keep Windows' chosen destination at a monitor seam. A correction
            // back onto the old monitor would reenter WM_DPICHANGED.
            if (!suggestedRect ||
                MonitorFromRect(&dragRect, MONITOR_DEFAULTTONEAREST) ==
                    MonitorFromRect(suggestedRect, MONITOR_DEFAULTTONEAREST)) {
                anchor = dragAnchor;
            }
            requestedPosition = &anchor;
        }
        ApplyUiScale(requestedPosition);
        if (!m_dragging && !IsWeatherDockActive() && m_hasCollapsedLocation) {
            ConfigStore::SavePosition(m_collapsedLocation);
        }
    }

    POINT MainWindow::ClampToScreens(POINT pt, SIZE sz, bool allowTaskbarArea) const {
        std::vector<RECT> workAreas;
        const bool useMonitorBounds = allowTaskbarArea || IsWeatherDockActive();

        struct EnumContext {
            std::vector<RECT>* pAreas;
            bool useMonitor;
        } ctx = { &workAreas, useMonitorBounds };

        EnumDisplayMonitors(
            NULL, NULL,
            [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                auto* pCtx = reinterpret_cast<EnumContext*>(lParam);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                if (GetMonitorInfoW(hMon, &mi)) {
                    pCtx->pAreas->push_back(pCtx->useMonitor ? mi.rcMonitor : mi.rcWork);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

        if (workAreas.empty()) {
            RECT rcDefault = {};
            if (useMonitorBounds) {
                rcDefault = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            } else {
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcDefault, 0);
            }
            workAreas.push_back(rcDefault);
        }

        // 1. 计算所有显示器工作区构成的虚拟屏幕外包围矩形
        RECT virtualWork = workAreas[0];
        for (size_t i = 1; i < workAreas.size(); ++i) {
            virtualWork.left = (std::min)(virtualWork.left, workAreas[i].left);
            virtualWork.top = (std::min)(virtualWork.top, workAreas[i].top);
            virtualWork.right = (std::max)(virtualWork.right, workAreas[i].right);
            virtualWork.bottom = (std::max)(virtualWork.bottom, workAreas[i].bottom);
        }

        POINT clamped = pt;

        // 2. 找到与目标窗口矩形相交面积最大或最近的基准显示器
        const auto farEdge = [](LONG origin, LONG extent) {
            const auto edge = static_cast<long long>(origin) + extent;
            return static_cast<LONG>((std::min)(
                edge, static_cast<long long>((std::numeric_limits<LONG>::max)())));
        };
        RECT targetRect = { pt.x, pt.y, farEdge(pt.x, sz.cx), farEdge(pt.y, sz.cy) };
        HMONITOR hMon = MonitorFromRect(&targetRect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (!hMon || !GetMonitorInfoW(hMon, &mi)) {
            mi.rcWork = virtualWork;
            mi.rcMonitor = virtualWork;
        }
        const RECT curWork = useMonitorBounds ? mi.rcMonitor : mi.rcWork;

        // Once a drag ends, keep the whole window on one work area. The
        // bounding rectangle of offset monitors contains invisible gaps.
        if (!m_dragging || workAreas.size() == 1) {
            return TaskbarPlacement::ClampToMonitorBounds(pt, sz, curWork);
        }

        // 3. 跨显示器边界检查：
        // 只有当窗口向某一方向超出当前显示器工作区，且该方向没有相邻显示器工作区可接纳时，
        // 才钳制到当前显示器边界。存在相邻显示器时，允许平滑越过边界，仅受全局虚拟屏幕限制。
        // 多显示器：检查右侧
        if (clamped.x + sz.cx > curWork.right) {
            bool hasRightNeighbor = false;
            for (const RECT& area : workAreas) {
                if (area.right > curWork.right &&
                    area.left <= curWork.right &&
                    (clamped.y < area.bottom && clamped.y + sz.cy > area.top)) {
                    hasRightNeighbor = true;
                    break;
                }
            }
            if (!hasRightNeighbor) {
                clamped.x = curWork.right - sz.cx;
            }
        }

        // 检查左侧
        if (clamped.x < curWork.left) {
            bool hasLeftNeighbor = false;
            for (const RECT& area : workAreas) {
                if (area.left < curWork.left &&
                    area.right >= curWork.left &&
                    (clamped.y < area.bottom && clamped.y + sz.cy > area.top)) {
                    hasLeftNeighbor = true;
                    break;
                }
            }
            if (!hasLeftNeighbor) {
                clamped.x = curWork.left;
            }
        }

        // 检查下方
        if (clamped.y + sz.cy > curWork.bottom) {
            bool hasBottomNeighbor = false;
            for (const RECT& area : workAreas) {
                if (area.bottom > curWork.bottom &&
                    area.top <= curWork.bottom &&
                    (clamped.x < area.right && clamped.x + sz.cx > area.left)) {
                    hasBottomNeighbor = true;
                    break;
                }
            }
            if (!hasBottomNeighbor) {
                clamped.y = curWork.bottom - sz.cy;
            }
        }

        // 检查上方
        if (clamped.y < curWork.top) {
            bool hasTopNeighbor = false;
            for (const RECT& area : workAreas) {
                if (area.top < curWork.top &&
                    area.bottom >= curWork.top &&
                    (clamped.x < area.right && clamped.x + sz.cx > area.left)) {
                    hasTopNeighbor = true;
                    break;
                }
            }
            if (!hasTopNeighbor) {
                clamped.y = curWork.top;
            }
        }

        // 4. 最终保证窗口落在虚拟屏幕整体范围内
        if (clamped.x < virtualWork.left) clamped.x = virtualWork.left;
        if (clamped.x + sz.cx > virtualWork.right) clamped.x = virtualWork.right - sz.cx;
        if (clamped.x < virtualWork.left) clamped.x = virtualWork.left;

        if (clamped.y < virtualWork.top) clamped.y = virtualWork.top;
        if (clamped.y + sz.cy > virtualWork.bottom) clamped.y = virtualWork.bottom - sz.cy;
        if (clamped.y < virtualWork.top) clamped.y = virtualWork.top;

        return clamped;
    }

    SIZE MainWindow::LogicalWindowSize() const {
        if (m_expanded) return { BAR_WIDTH, EXPANDED_HEIGHT };
        if (m_settings.miniMode) {
            return { MiniBarLogicalWidth(m_snapshot), MINI_HEIGHT };
        }
        return { BAR_WIDTH, COLLAPSED_HEIGHT };
    }

    SIZE MainWindow::CollapsedPhysicalWindowSize(float uiScale) const {
        if (m_settings.miniMode) {
            return {
                MiniBarPhysicalWidth(m_snapshot, uiScale),
                MiniBarPhysicalHeight(uiScale)
            };
        }
        return {
            ScaleEven(static_cast<float>(BAR_WIDTH), uiScale),
            ScaleEven(static_cast<float>(COLLAPSED_HEIGHT), uiScale)
        };
    }

    SIZE MainWindow::PhysicalWindowSize(float uiScale) const {
        if (!m_expanded) {
            return CollapsedPhysicalWindowSize(uiScale);
        }
        return {
            ScaleEven(static_cast<float>(BAR_WIDTH), uiScale),
            ExpandedPhysicalHeight(uiScale)
        };
    }

    void MainWindow::ApplyCurrentWindowEffects() const {
        ApplyPopupWindowEffects(m_hwnd);
    }

    void MainWindow::ToggleExpanded() {
        if (IsWeatherDockActive()) return;

        HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);
        RECT area = mi.rcWork;

        RECT rc;
        GetWindowRect(m_hwnd, &rc);

        int targetLeft = rc.left;
        int targetTop = rc.top;
        if (!m_expanded) {
            m_collapsedLocation = { rc.left, rc.top };
            m_hasCollapsedLocation = true;
            m_expanded = true;
        } else {
            POINT col = m_hasCollapsedLocation ? m_collapsedLocation : POINT{ rc.left, rc.top };
            targetLeft = col.x;
            targetTop = col.y;
            m_expanded = false;
        }

        const SIZE physical = PhysicalWindowSize();
        const int targetW = physical.cx;
        const int targetH = physical.cy;
        m_renderer->Resize(
            static_cast<UINT>(targetW),
            static_cast<UINT>(targetH));

        if (m_expanded && rc.top + targetH > area.bottom) {
            targetTop = std::max<int>(area.top, area.bottom - targetH);
        }
        const POINT clamped = ClampToScreens(
            { targetLeft, targetTop }, { targetW, targetH });
        targetLeft = clamped.x;
        targetTop = clamped.y;
        if (!m_expanded) {
            m_collapsedLocation = clamped;
            m_hasCollapsedLocation = true;
        }

        ApplyWindowZOrder();
        SetWindowPos(
            m_hwnd, NULL,
            targetLeft, targetTop,
            targetW, targetH,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS |
            SWP_NOREDRAW | SWP_FRAMECHANGED);

        ApplyCurrentWindowEffects();
        RenderLayeredWindow();
        DwmFlush();
    }

    void MainWindow::OnWindowMoved() {
        SyncWindowPosition(true);
    }

    POINT MainWindow::DragWindowPosition(POINT cursor, float uiScale) const {
        const double ratio = static_cast<double>(uiScale) / m_dragStartScale;
        return {
            cursor.x - std::lround(m_dragClickPoint.x * ratio),
            cursor.y - std::lround(m_dragClickPoint.y * ratio)
        };
    }

    void MainWindow::SyncWindowPosition(bool updateCollapsedFromWindow) {
        if (!m_hwnd) return;
        RECT rc = {};
        if (!GetWindowRect(m_hwnd, &rc)) return;

        const SIZE sz = PhysicalWindowSize();
        POINT clamped = {};
        if (!updateCollapsedFromWindow && m_expanded && m_hasCollapsedLocation) {
            m_collapsedLocation = ClampToScreens(
                m_collapsedLocation, CollapsedPhysicalWindowSize());
            clamped = ClampToScreens(m_collapsedLocation, sz);
        } else {
            clamped = ClampToScreens({ rc.left, rc.top }, sz);
            m_collapsedLocation = clamped;
            m_hasCollapsedLocation = true;
        }

        if (clamped.x != rc.left || clamped.y != rc.top ||
            (rc.right - rc.left) != sz.cx || (rc.bottom - rc.top) != sz.cy) {
            SetWindowPos(
                m_hwnd, NULL, clamped.x, clamped.y, sz.cx, sz.cy,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
            RenderLayeredWindow();
        }

        if (m_hasCollapsedLocation) {
            ConfigStore::SavePosition(m_collapsedLocation);
        }
    }

    void MainWindow::BeginShutdown() {
        if (m_shuttingDown.exchange(true)) return;
        {
            const std::lock_guard<std::mutex> lock(m_weatherProbeState->mutex);
            m_weatherProbeState->canceled = true;
        }
        if (m_fetchState) {
            m_fetchState->canceled = true;
            const std::lock_guard<std::mutex> lock(m_fetchState->mutex);
            m_fetchState->pending.reset();
        }
        if (m_companionPollState) {
            const std::lock_guard<std::mutex> lock(m_companionPollState->mutex);
            m_companionPollState->canceled = true;
        }
        m_fetchWorker.RequestStop();
        m_companionWorker.RequestStop();
        m_weatherWorker.RequestStop();
    }

    void MainWindow::RefreshQuota() {
        if (!m_hwnd || m_shuttingDown.load()) return;
        if (m_fetchState->inFlight.exchange(true)) {
            m_refreshQueued = true;
            return;
        }

        m_syncState = SyncState::Syncing;
        if (IsWindowVisible(m_hwnd)) RenderLayeredWindow();

        HWND hwnd = m_hwnd;
        auto fetchState = m_fetchState;

        try {
            m_fetchWorker.Submit([hwnd, fetchState](std::stop_token stop) {
                QuotaSnapshot snap;
                try {
                    snap = CodexClient::FetchSnapshot(stop);
                } catch (...) {
                    WriteLog(LogLevel::Error, L"额度同步失败。");
                }
                if (stop.stop_requested()) return;
                const std::lock_guard<std::mutex> lock(fetchState->mutex);
                if (fetchState->canceled.load()) return;
                fetchState->pending = std::move(snap);
                if (!PostMessageW(hwnd, WM_CQB_QUOTA_RESULT, 0, 0)) {
                    fetchState->pending.reset();
                    fetchState->inFlight = false;
                }
            });
        } catch (const std::system_error&) {
            m_fetchState->inFlight = false;
            m_syncState = SyncState::Failed;
            WriteLog(LogLevel::Error, L"额度同步线程创建失败。");
            if (IsWindowVisible(m_hwnd)) RenderLayeredWindow();
        }
    }

    bool MainWindow::IsHeaderArea(POINT pt) const {
        const int headerHeight = m_settings.miniMode && !m_expanded
            ? MiniBarPhysicalHeight(m_uiScale)
            : ScaleEven(m_expanded ? 24.0f : static_cast<float>(COLLAPSED_HEIGHT), m_uiScale);
        return pt.x >= 0 && pt.x < PhysicalWindowSize().cx && pt.y >= 0 && pt.y < headerHeight;
    }

    bool MainWindow::IsExpandButtonArea(POINT pt) const {
        if (m_settings.miniMode && !m_expanded) return false;
        const int left = ScaleEven(345.0f, m_uiScale);
        const int right = ScaleEven(static_cast<float>(BAR_WIDTH), m_uiScale);
        const int top = ScaleEven(68.0f, m_uiScale);
        const int bottom = ScaleEven(static_cast<float>(COLLAPSED_HEIGHT), m_uiScale);
        return pt.x >= left && pt.x < right && pt.y >= top && pt.y < bottom;
    }

    void MainWindow::HandleContextMenu(int screenX, int screenY) {
        const bool componentMode = IsWeatherDockActive();
        const std::vector<MenuItem> items = {
            { componentMode ? 0 : IDM_TOGGLE_EXPAND, componentMode ? L"展开详情（已锁定）"
                : (m_expanded ? L"收起详情" : L"展开详情") },
            { componentMode ? 0 : IDM_MINI_MODE, componentMode ? L"迷你模式：开（已锁定）"
                : (m_settings.miniMode ? L"迷你模式：开" : L"迷你模式：关") },
            { IDM_COMPONENT_MODE, componentMode ? L"组件模式：开" : L"组件模式：关" },
            { IDM_REFRESH, L"立即刷新" },
            { IDM_REFRESH_INTERVAL_SUB, L"刷新间隔" },
            { IDM_FONT_SUB, L"字体选择" },
            { IDM_APPEARANCE_SUB, L"外观配置" },
            { componentMode ? 0 : IDM_ALWAYS_ON_TOP, componentMode ? L"置顶模式：开（已锁定）"
                : (m_settings.alwaysOnTop ? L"置顶模式：开" : L"置顶模式：关") },
            { IDM_COMPANION_MODE, m_companionMode ? L"伴随模式：开" : L"伴随模式：关" },
            { 0, L"" }, // 分隔线
            { IDM_EXIT, L"退出" },
        };

        SetForegroundWindow(m_hwnd);
        const int cmd = CustomMenu::Show(
            m_hwnd, m_uiScale, screenX, screenY, items,
            m_renderer->Palette(), m_renderer->FontFamily());

        if (cmd == IDM_TOGGLE_EXPAND) {
            ToggleExpanded();
        } else if (cmd == IDM_MINI_MODE) {
            ToggleMiniMode();
        } else if (cmd == IDM_COMPONENT_MODE) {
            SetComponentMode(!IsWeatherDockActive());
        } else if (cmd == IDM_REFRESH) {
            RefreshQuota();
        } else if (cmd == IDM_REFRESH_INTERVAL_SUB) {
            std::vector<MenuItem> intervalItems;
            for (std::size_t i = 0; i < REFRESH_INTERVAL_MINUTES.size(); ++i) {
                const int level = static_cast<int>(i);
                std::wstring text = RefreshIntervalText(level);
                if (level == m_refreshIntervalLevel) text += L" (当前)";
                intervalItems.push_back({ IDM_REFRESH_INTERVAL_BASE + level, text });
            }
            const int interval = CustomMenu::Show(
                m_hwnd, m_uiScale, screenX, screenY, intervalItems,
                m_renderer->Palette(), m_renderer->FontFamily());
            if (interval >= IDM_REFRESH_INTERVAL_BASE &&
                interval < IDM_REFRESH_INTERVAL_BASE +
                    static_cast<int>(REFRESH_INTERVAL_MINUTES.size())) {
                ApplyRefreshInterval(interval - IDM_REFRESH_INTERVAL_BASE);
            }
        } else if (cmd == IDM_FONT_SUB) {
            ShowFontMenu(screenX, screenY);
        } else if (cmd == IDM_APPEARANCE_SUB) {
            const bool custom =
                m_settings.appearance.mode == AppearanceMode::Custom;
            const std::vector<MenuItem> appearanceItems = {
                { IDM_APPEARANCE_DEFAULT, custom ? L"使用默认外观" : L"使用默认外观 (当前)" },
                { IDM_APPEARANCE_CUSTOM, custom ? L"使用个性外观 (当前)" : L"使用个性外观" },
                { IDM_FONT_SUB, L"选择界面字体" },
                { IDM_APPEARANCE_EDIT, L"编辑个性外观与字体" },
            };
            const int appearance = CustomMenu::Show(
                m_hwnd, m_uiScale, screenX, screenY, appearanceItems,
                m_renderer->Palette(), m_renderer->FontFamily());
            if (appearance == IDM_APPEARANCE_DEFAULT) {
                SetAppearanceMode(AppearanceMode::Default);
            } else if (appearance == IDM_APPEARANCE_CUSTOM) {
                SetAppearanceMode(AppearanceMode::Custom);
            } else if (appearance == IDM_FONT_SUB) {
                ShowFontMenu(screenX, screenY);
            } else if (appearance == IDM_APPEARANCE_EDIT) {
                ConfigureCustomAppearance();
            }
        } else if (cmd == IDM_ALWAYS_ON_TOP) {
            ToggleAlwaysOnTop();
        } else if (cmd == IDM_COMPANION_MODE) {
            ToggleCompanionMode();
        } else if (cmd == IDM_EXIT) {
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }
    }

    LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1; // 阻止背景默认抹除，杜绝白闪

        case 0x0083: // WM_NCCALCSIZE
            if (wParam != 0) {
                NCCALCSIZE_PARAMS* pnc = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                pnc->rgrc[1] = pnc->rgrc[0];
                pnc->rgrc[2] = pnc->rgrc[0];
                return WVR_REDRAW; // 0x0300 阻止 DWM 错位历史位图拷贝
            }
            return 0;

        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_LBUTTONDOWN: {
            if (IsWeatherDockActive()) return 0;
            const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            POINT screenPt = pt;
            if (!m_dragging && IsHeaderArea(pt) && !IsExpandButtonArea(pt) &&
                ClientToScreen(m_hwnd, &screenPt)) {
                m_dragClickPoint = pt;
                m_lastDragCursor = screenPt;
                m_dragStartScale = m_uiScale;
                SetCapture(m_hwnd);
                m_dragging = GetCapture() == m_hwnd;
                if (m_dragging && m_settings.miniMode && !m_expanded) {
                    GetWeatherPlacement(PhysicalWindowSize());
                }
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (m_dragging) {
                POINT cursor = {};
                if (!GetCursorPos(&cursor)) {
                    cursor = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    if (!ClientToScreen(m_hwnd, &cursor)) return 0;
                }
                if (cursor.x == m_lastDragCursor.x && cursor.y == m_lastDragCursor.y) {
                    return 0;
                }
                m_lastDragCursor = cursor;
                const SIZE sz = PhysicalWindowSize();
                const bool allowTaskbar = m_settings.miniMode && !m_expanded;
                const POINT target = DragWindowPosition(cursor, m_uiScale);
                POINT clamped = ClampToScreens(target, sz, allowTaskbar);
                const RECT proposed = { clamped.x, clamped.y, clamped.x + sz.cx, clamped.y + sz.cy };
                const HMONITOR current = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
                const HMONITOR next = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
                if (current != next) {
                    MONITORINFO oldMonitor = { sizeof(oldMonitor) };
                    MONITORINFO newMonitor = { sizeof(newMonitor) };
                    if (GetMonitorInfoW(current, &oldMonitor) && GetMonitorInfoW(next, &newMonitor)) {
                        clamped = TaskbarPlacement::HoldAtMonitorSeam(
                            clamped, sz, oldMonitor.rcMonitor, newMonitor.rcMonitor,
                            Scale(16.0f, m_dpiScale));
                    }
                }
                SetWindowPos(m_hwnd, NULL,
                    clamped.x,
                    clamped.y,
                    0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (m_dragging) {
                m_dragging = false;
                ReleaseCapture();

                RECT rc = {};
                GetWindowRect(m_hwnd, &rc);
                const SIZE sz = PhysicalWindowSize();

                if (m_settings.miniMode && !m_expanded) {
                    const WeatherWidgetPlacement placement = GetWeatherPlacement(sz);
                    const int snapThreshold = ScaleEven(24.0f, m_weatherMetrics.dpiScale);
                    const bool snapped = m_weatherMetrics.hTaskbar && TaskbarPlacement::IsNearDockPosition(
                        POINT{ rc.left, rc.top }, placement.dockPosition, snapThreshold);

                    if (snapped && SetComponentMode(true)) return 0;
                }

                OnWindowMoved();
                return 0;
            }
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (IsExpandButtonArea(pt)) {
                ToggleExpanded();
                return 0;
            }
            break;
        }

        case WM_CAPTURECHANGED:
            if (m_dragging) {
                m_dragging = false;
                OnWindowMoved();
            }
            break;

        case WM_LBUTTONDBLCLK: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (IsHeaderArea(pt) && !IsExpandButtonArea(pt)) {
                RefreshQuota();
                return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            if (m_contextMenuOpen || m_dragging) return 0;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (lParam == -1) {
                // Keyboard context-menu requests do not include a screen position.
                pt = { 0, 0 };
                if (!ClientToScreen(m_hwnd, &pt)) return 0;
            }
            m_contextMenuOpen = true;
            HandleContextMenu(pt.x, pt.y);
            m_contextMenuOpen = false;
            return 0;
        }

        case WM_CQB_REFRESH:
            RefreshQuota();
            return 0;

        case WM_CQB_SHOW:
            Show();
            return 0;

        case WM_CQB_HIDE:
            Hide();
            return 0;

        case WM_CQB_TOGGLE:
            ToggleVisible();
            return 0;

        case WM_CQB_QUOTA_RESULT: {
            std::optional<QuotaSnapshot> snap;
            {
                const std::lock_guard<std::mutex> lock(m_fetchState->mutex);
                snap.swap(m_fetchState->pending);
            }
            if (snap) {
                const int previousMiniWidth = MiniBarLogicalWidth(m_snapshot);
                if (!snap->statsSynchronized) snap->stats = m_snapshot.stats;
                m_snapshot = std::move(*snap);
                m_syncState = m_snapshot.success ? SyncState::Synced : SyncState::Failed;
                const bool miniWidthChanged = m_settings.miniMode && !m_expanded &&
                    previousMiniWidth != MiniBarLogicalWidth(m_snapshot);
                if (miniWidthChanged) {
                    ApplyUiScale();
                    if (!m_dragging && !IsWeatherDockActive() && m_hasCollapsedLocation) {
                        ConfigStore::SavePosition(m_collapsedLocation);
                    }
                } else if (IsWindowVisible(m_hwnd)) {
                    RenderLayeredWindow();
                }
            }
            m_fetchState->inFlight = false;
            if (m_refreshQueued.exchange(false)) {
                RefreshQuota();
            }
            return 0;
        }

        case WM_CQB_COMPANION_RESULT:
            HandleCompanionResult(static_cast<DesktopProcessState>(wParam));
            return 0;

        case WM_CQB_WEATHER_RESULT:
            HandleWeatherProbeResult();
            return 0;

        case WM_GETDPISCALEDSIZE: {
            if (lParam == 0 || wParam == 0) return FALSE;
            const float scale = (static_cast<float>(wParam) / BASE_DPI) *
                USER_SCALE_LEVELS[static_cast<std::size_t>(m_userScaleLevel)];
            *reinterpret_cast<SIZE*>(lParam) = PhysicalWindowSize(scale);
            return TRUE;
        }

        case WM_DPICHANGED:
            OnDpiChanged(LOWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_DISPLAYCHANGE:
            ScheduleWeatherLayout();
            if (!IsWeatherDockActive()) SyncWindowPosition(false);
            break;

        case WM_SETTINGCHANGE:
            ScheduleWeatherLayout();
            if (!IsWeatherDockActive() && wParam == SPI_SETWORKAREA) SyncWindowPosition(false);
            break;

        case WM_TIMER:
            if (wParam == TIMER_REFRESH_ID) {
                if (!m_companionMode || m_codexDesktopRunning) {
                    RefreshQuota();
                }
            } else if (wParam == TIMER_COMPANION_ID) {
                PollCompanionMode();
            } else if (wParam == TIMER_RENDER_RETRY_ID) {
                KillTimer(m_hwnd, TIMER_RENDER_RETRY_ID);
                if (IsWindowVisible(m_hwnd)) RenderLayeredWindow();
            } else if (wParam == TIMER_WEATHER_LAYOUT_ID) {
                KillTimer(m_hwnd, TIMER_WEATHER_LAYOUT_ID);
                m_weatherLayoutPending = false;
                DockToWeatherWidget(true);
            }
            return 0;

        case WM_CLOSE:
            BeginShutdown();
            DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            BeginShutdown();
            if (m_hasCollapsedLocation && (!IsWeatherDockActive() ||
                (!m_weatherLayoutPending && !m_weatherProbeState->inFlight))) {
                ConfigStore::SavePosition(m_collapsedLocation);
            }
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY: {
            const HWND hwnd = m_hwnd.load();
            const LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            m_hwnd = NULL;
            return result;
        }

        default:
            if (m_wmTaskbarCreated != 0 && msg == m_wmTaskbarCreated) {
                ScheduleWeatherLayout();
                return 0;
            }
            break;
        }

        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }

    void MainWindow::OnPaint() {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        EndPaint(m_hwnd, &ps);
        RenderLayeredWindow();
    }

    HRESULT MainWindow::RenderLayeredWindow() {
        if (!m_hwnd) return E_UNEXPECTED;
        HRESULT hr = m_renderer->Render(
            m_expanded, m_settings.miniMode, m_snapshot, m_syncState);
        if (SUCCEEDED(hr)) {
            m_renderFailureCount = 0;
            KillTimer(m_hwnd, TIMER_RENDER_RETRY_ID);
            ValidateRect(m_hwnd, NULL);
            return hr;
        }

        ++m_renderFailureCount;
        if (m_renderFailureCount <= kRenderRetryLimit) {
            wchar_t message[128] = {};
            swprintf_s(
                message,
                L"分层窗口渲染失败（HRESULT=0x%08X），正在重试（%d/%d）。",
                static_cast<unsigned>(hr),
                m_renderFailureCount,
                kRenderRetryLimit);
            WriteLog(LogLevel::Warning, message);
            if (IsWindowVisible(m_hwnd)) SetTimer(m_hwnd, TIMER_RENDER_RETRY_ID, 100, nullptr);
        } else if (m_renderFailureCount == kRenderRetryLimit + 1) {
            WriteLog(LogLevel::Error, L"分层窗口连续渲染失败，已停止自动重试。");
        }
        return hr;
    }

} // namespace CodexQuotaBar
