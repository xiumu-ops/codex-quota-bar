#include "UI/MainWindow.h"
#include "Core/Logger.h"
#include "UI/AppearanceDialog.h"
#include "UI/CustomMenu.h"
#include "UI/TransparencyDialog.h"
#include "Services/CodexClient.h"
#include "Services/CompanionMode.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/ConfigStore.h"
#include "resources/resource.h"

#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <system_error>
#include <limits>
#include <thread>

namespace CodexQuotaBar {

    constexpr UINT_PTR TIMER_REFRESH_ID = 1001;
    constexpr UINT_PTR TIMER_COMPANION_ID = 1002;

    constexpr int IDM_TOGGLE_EXPAND = 2001;
    constexpr int IDM_REFRESH = 2002;
    constexpr int IDM_EXIT = 2003;
    constexpr int IDM_COMPANION_MODE = 2004;
    constexpr int IDM_BACKGROUND_TRANSPARENCY = 2005;
    constexpr int IDM_SCALE_SUB = 2100;        // 一级菜单：进入缩放子菜单
    constexpr int IDM_SCALE_LEVEL_BASE = 2101; // 二级菜单：档位 id 基址（+0..4）
    constexpr int IDM_REFRESH_INTERVAL_SUB = 2200;
    constexpr int IDM_REFRESH_INTERVAL_BASE = 2201; // 二级菜单：刷新档位 id 基址（+0..4）
    constexpr int IDM_APPEARANCE_SUB = 2300;
    constexpr int IDM_APPEARANCE_DEFAULT = 2301;
    constexpr int IDM_APPEARANCE_CUSTOM = 2302;
    constexpr int IDM_APPEARANCE_OPEN_CONFIG = 2303;
    constexpr int IDM_APPEARANCE_RELOAD = 2304;
    constexpr int IDM_APPEARANCE_OPEN_DEFAULT_CONFIG = 2305;
    constexpr int IDM_APPEARANCE_EDIT_COLORS = 2306;

    namespace {
        constexpr UINT kCompanionPollMs = 2000;
        constexpr int kCompanionMissingPollThreshold = 1; // 首次轮询未发现即隐藏（最长约 2 秒）
        // 用户缩放档位：75% / 87.5% / 100% / 112.5% / 125%
        constexpr float kUserScaleLevels[] = { 0.75f, 0.875f, 1.0f, 1.125f, 1.25f };
        constexpr int kUserScaleCount = 5;
        constexpr int kRefreshIntervalMinutes[] = { 1, 5, 10, 30, 60 };
        constexpr int kRefreshIntervalCount = 5;

        // 数值 → 最接近档位索引（配置读回时归一）
        int ClosestScaleLevel(float value) {
            int best = 2; // 默认 100%
            float bestDiff = 1e9f;
            for (int i = 0; i < kUserScaleCount; ++i) {
                const float diff = std::fabs(kUserScaleLevels[i] - value);
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = i;
                }
            }
            return best;
        }

        // 档位 → 百分比文案："75%" / "87.5%" / "100%" / "112.5%" / "125%"
        std::wstring ScaleLevelText(int level) {
            wchar_t buf[16] = {};
            swprintf_s(buf, L"%g%%", kUserScaleLevels[level] * 100.0);
            return buf;
        }

        int ClosestRefreshIntervalLevel(int minutes) {
            int best = 0;
            int bestDiff = (std::numeric_limits<int>::max)();
            for (int i = 0; i < kRefreshIntervalCount; ++i) {
                const int diff = std::abs(kRefreshIntervalMinutes[i] - minutes);
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = i;
                }
            }
            return best;
        }

        std::wstring RefreshIntervalText(int level) {
            return std::to_wstring(kRefreshIntervalMinutes[level]) + L" 分钟";
        }
    }

    MainWindow::MainWindow()
        : m_renderer(std::make_unique<Direct2DRenderer>())
    {
    }

    MainWindow::~MainWindow() {
        BeginShutdown();
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

    bool MainWindow::Create(bool forceInitialRefresh) {
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

        int initW = Scale(BAR_WIDTH, 1.0f);
        int initH = Scale(COLLAPSED_HEIGHT, 1.0f);

        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            wc.lpszClassName,
            L"Codex-Quota-Bar",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT, initW, initH,
            NULL, NULL, hInstance, this);

        if (!m_hwnd) return false;

        // Windows 11 DWM 硬件级圆角
        int cornerPref = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

        // 初始化 Direct2D 渲染器
        HRESULT rendererHr = m_renderer->Initialize(m_hwnd);
        if (FAILED(rendererHr)) {
            DestroyWindow(m_hwnd);
            m_hwnd = NULL;
            return false;
        }

        // 读取用户设置
        std::wstring settingsErrors;
        m_settings = ConfigStore::LoadSettings(&settingsErrors);
        m_userScaleLevel = ClosestScaleLevel(m_settings.userScale);
        m_refreshIntervalLevel = ClosestRefreshIntervalLevel(m_settings.refreshIntervalMinutes);
        m_companionMode = m_settings.companionMode;
        m_codexDesktopRunning = false;
        if (m_companionMode) {
            CompanionMode::ConfigureAutoStart(true);
        }
        ApplyAppearance(settingsErrors);

        // 获取当前屏幕 DPI 并适配
        UINT dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0) dpi = BASE_DPI;
        ApplyDpiScale(dpi);

        // 读取持久化窗口位置
        auto stored = ConfigStore::LoadState();
        if (stored.hasPosition) {
            SIZE sz = { Scale(BAR_WIDTH, m_uiScale), Scale(COLLAPSED_HEIGHT, m_uiScale) };
            POINT clamped = ClampToScreens(stored.position, sz);
            SetWindowPos(m_hwnd, NULL, clamped.x, clamped.y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
            m_collapsedLocation = clamped;
        } else {
            RECT rcWork;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
            int w = Scale(BAR_WIDTH, m_uiScale);
            int h = Scale(COLLAPSED_HEIGHT, m_uiScale);
            int x = rcWork.left + (rcWork.right - rcWork.left - w) / 2;
            int y = rcWork.top + 34;
            SetWindowPos(m_hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            m_collapsedLocation = { x, y };
        }
        m_hasCollapsedLocation = true;

        // 自动刷新定时器
        SetTimer(m_hwnd, TIMER_REFRESH_ID,
                 static_cast<UINT>(kRefreshIntervalMinutes[m_refreshIntervalLevel] * 60 * 1000), NULL);
        if (m_companionMode) {
            SetTimer(m_hwnd, TIMER_COMPANION_ID, kCompanionPollMs, NULL);
            PollCompanionMode();
        }

        if (forceInitialRefresh || !m_companionMode || m_codexDesktopRunning) {
            RefreshQuota();
        }

        return true;
    }

    void MainWindow::Show() {
        if (m_hwnd) {
            if (m_companionMode && !m_codexDesktopRunning) return;
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(m_hwnd);
        }
    }

    void MainWindow::Hide() {
        if (m_hwnd) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
    }

    void MainWindow::ToggleVisible() {
        if (m_hwnd) {
            if (IsWindowVisible(m_hwnd)) Hide();
            else Show();
        }
    }

    void MainWindow::ApplyDpiScale(int dpi) {
        m_currentDpi = (dpi <= 0) ? BASE_DPI : dpi;
        m_dpiScale = m_currentDpi / static_cast<float>(BASE_DPI);
        ApplyUiScale();
    }

    void MainWindow::ApplyUiScale() {
        m_uiScale = m_dpiScale * kUserScaleLevels[m_userScaleLevel];
        m_renderer->SetDpiScale(m_uiScale);

        int targetW = Scale(BAR_WIDTH, m_uiScale);
        float logicalH = m_expanded ? static_cast<float>(EXPANDED_HEIGHT)
                                    : static_cast<float>(COLLAPSED_HEIGHT);
        int targetH = Scale(logicalH, m_uiScale);

        SetWindowPos(m_hwnd, NULL, 0, 0, targetW, targetH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        m_renderer->Resize(targetW, targetH);
        InvalidateRect(m_hwnd, NULL, FALSE);
    }

    void MainWindow::ApplyUserScale(int level) {
        if (level < 0 || level >= kUserScaleCount) return;
        m_userScaleLevel = level;
        m_settings.userScale = kUserScaleLevels[level];
        SaveSettingsWithFeedback();
        ApplyUiScale();
    }

    void MainWindow::ApplyRefreshInterval(int level) {
        if (level < 0 || level >= kRefreshIntervalCount || !m_hwnd) return;
        m_refreshIntervalLevel = level;
        SetTimer(m_hwnd, TIMER_REFRESH_ID,
                 static_cast<UINT>(kRefreshIntervalMinutes[level] * 60 * 1000), NULL);
        m_settings.refreshIntervalMinutes = kRefreshIntervalMinutes[level];
        SaveSettingsWithFeedback();
    }

    bool MainWindow::SaveSettingsWithFeedback(bool preserveDiskAppearance) {
        AppSettings settingsToSave = m_settings;
        if (preserveDiskAppearance) {
            std::wstring loadErrors;
            bool readable = false;
            const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
            if (!readable || !loadErrors.empty()) {
                MessageBoxW(
                    m_hwnd,
                    (L"配置未保存，请先修正配置文件：\n\n" + loadErrors).c_str(),
                    L"Codex-Quota-Bar 配置错误",
                    MB_OK | MB_ICONWARNING);
                return false;
            }
            settingsToSave.appearance = loaded.appearance;
        }

        std::wstring errors;
        if (ConfigStore::SaveSettings(settingsToSave, &errors)) return true;
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
            m_settings.appearance.backgroundTransparency,
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
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    }

    void MainWindow::SetAppearanceMode(AppearanceMode mode) {
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable || !loadErrors.empty()) {
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

    void MainWindow::ConfigureBackgroundTransparency() {
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable || !loadErrors.empty()) {
            MessageBoxW(
                m_hwnd,
                (L"无法修改透明背景，请先修正配置文件：\n\n" + loadErrors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }

        int selectedValue = loaded.appearance.backgroundTransparency;
        if (!ShowTransparencyDialog(
                m_hwnd,
                m_uiScale,
                loaded.appearance.backgroundTransparency,
                selectedValue,
                m_renderer->Palette(),
                m_renderer->FontFamily())) {
            return;
        }
        if (selectedValue == loaded.appearance.backgroundTransparency) return;

        const AppearanceSettings previous = m_settings.appearance;
        const AppearanceSettings previousDefault = m_settings.defaultAppearance;
        m_settings.appearance = loaded.appearance;
        m_settings.defaultAppearance = loaded.defaultAppearance;
        m_settings.appearance.backgroundTransparency = selectedValue;
        if (!SaveSettingsWithFeedback(false)) {
            m_settings.appearance = previous;
            m_settings.defaultAppearance = previousDefault;
            return;
        }
        ApplyAppearance();
    }

    void MainWindow::ConfigureCustomColors() {
        std::wstring loadErrors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&loadErrors, &readable);
        if (!readable || !loadErrors.empty()) {
            MessageBoxW(
                m_hwnd,
                (L"无法修改个性配色，请先修正配置文件：\n\n" + loadErrors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }

        std::map<std::wstring, std::wstring> selectedColors;
        if (!ShowAppearanceColorsDialog(
                m_hwnd,
                m_uiScale,
                loaded.appearance.colors,
                selectedColors,
                m_renderer->Palette(),
                m_renderer->FontFamily())) {
            return;
        }

        const AppearanceSettings previous = m_settings.appearance;
        const AppearanceSettings previousDefault = m_settings.defaultAppearance;
        m_settings.appearance = loaded.appearance;
        m_settings.defaultAppearance = loaded.defaultAppearance;
        m_settings.appearance.colors = std::move(selectedColors);
        m_settings.appearance.mode = AppearanceMode::Custom;
        if (!SaveSettingsWithFeedback(false)) {
            m_settings.appearance = previous;
            m_settings.defaultAppearance = previousDefault;
            return;
        }
        ApplyAppearance();
    }

    void MainWindow::ReloadAppearance() {
        std::wstring errors;
        bool readable = false;
        const AppSettings loaded = ConfigStore::LoadSettings(&errors, &readable);
        if (!readable) {
            MessageBoxW(
                m_hwnd,
                (L"无法重新加载配置：\n\n" + errors).c_str(),
                L"Codex-Quota-Bar 配置错误",
                MB_OK | MB_ICONWARNING);
            return;
        }
        m_settings.appearance = loaded.appearance;
        m_settings.defaultAppearance = loaded.defaultAppearance;
        ApplyAppearance(errors);
    }

    void MainWindow::OpenConfigFile() {
        const std::wstring path = ConfigStore::ConfigFilePath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            !SaveSettingsWithFeedback()) {
            return;
        }
        const HINSTANCE result = ShellExecuteW(
            m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            MessageBoxW(
                m_hwnd, L"无法使用默认编辑器打开用户配置文件。",
                L"Codex-Quota-Bar", MB_OK | MB_ICONWARNING);
        }
    }

    void MainWindow::OpenDefaultConfigFile() {
        const std::wstring path = ConfigStore::DefaultConfigFilePath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(
                m_hwnd, L"找不到 config-default.json。请重新安装或重新构建程序。",
                L"Codex-Quota-Bar", MB_OK | MB_ICONWARNING);
            return;
        }
        const HINSTANCE result = ShellExecuteW(
            m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            MessageBoxW(
                m_hwnd, L"无法使用默认编辑器打开 config-default.json。",
                L"Codex-Quota-Bar", MB_OK | MB_ICONWARNING);
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

        if (m_companionMode) {
            m_codexDesktopRunning = false;
            SetTimer(m_hwnd, TIMER_COMPANION_ID, kCompanionPollMs, NULL);
            Hide();
            PollCompanionMode();
        } else {
            KillTimer(m_hwnd, TIMER_COMPANION_ID);
            m_codexDesktopRunning = false;
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(m_hwnd);
        }
    }

    void MainWindow::PollCompanionMode() {
        if (!m_companionMode || !m_hwnd) return;

        const auto state = m_companionPollState;
        if (state->canceled.load() || state->inFlight.exchange(true)) return;
        const HWND hwnd = m_hwnd.load();
        try {
            std::thread([state, hwnd]() {
                const bool running = CompanionMode::IsDesktopRunning();
                if (state->canceled.load() ||
                    !PostMessageW(hwnd, WM_CQB_COMPANION_RESULT, running ? 1 : 0, 0)) {
                    state->inFlight = false;
                }
            }).detach();
        } catch (const std::system_error&) {
            state->inFlight = false;
            WriteLog(LogLevel::Warning, L"伴随模式检测线程创建失败。");
        }
    }

    void MainWindow::HandleCompanionResult(bool running) {
        m_companionPollState->inFlight = false;
        if (!m_companionMode || m_shuttingDown.load()) return;

        if (running) {
            const bool wasRunning = m_codexDesktopRunning;
            m_codexDesktopRunning = true;
            m_codexMissingPolls = 0;
            if (!IsWindowVisible(m_hwnd)) {
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(m_hwnd);
                if (!wasRunning) RefreshQuota();
            }
            return;
        }

        if (m_codexMissingPolls < kCompanionMissingPollThreshold) {
            ++m_codexMissingPolls;
        }
        if (m_codexMissingPolls >= kCompanionMissingPollThreshold) {
            m_codexDesktopRunning = false;
            if (IsWindowVisible(m_hwnd)) Hide();
        }
    }

    void MainWindow::OnDpiChanged(int newDpi, const RECT* suggestedRect) {
        if (suggestedRect) {
            SetWindowPos(m_hwnd, NULL, suggestedRect->left, suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (m_dragging) {
                m_dragWindowPos = { suggestedRect->left, suggestedRect->top };
            }
        }
        ApplyDpiScale(newDpi);
    }

    POINT MainWindow::ClampToScreens(POINT pt, SIZE sz) const {
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);
        RECT area = mi.rcWork;

        POINT clamped = pt;
        if (clamped.x < area.left) clamped.x = area.left;
        if (clamped.x + sz.cx > area.right) clamped.x = area.right - sz.cx;
        if (clamped.y < area.top) clamped.y = area.top;
        if (clamped.y + sz.cy > area.bottom) clamped.y = area.bottom - sz.cy;

        return clamped;
    }

    void MainWindow::ToggleExpanded() {
        int collapsedH = Scale(COLLAPSED_HEIGHT, m_uiScale);
        int expandedH = Scale(EXPANDED_HEIGHT, m_uiScale);

        HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);
        RECT area = mi.rcWork;

        RECT rc;
        GetWindowRect(m_hwnd, &rc);

        int targetLeft = rc.left;
        int targetTop = rc.top;
        int targetH;

        if (!m_expanded) {
            m_collapsedLocation = { rc.left, rc.top };
            m_hasCollapsedLocation = true;
            m_expanded = true;

            if (rc.top + expandedH > area.bottom) {
                targetTop = std::max<int>(area.top, area.bottom - expandedH);
            }
            targetH = expandedH;
        } else {
            POINT col = m_hasCollapsedLocation ? m_collapsedLocation : POINT{ rc.left, rc.top };
            SIZE sz = { rc.right - rc.left, collapsedH };
            col = ClampToScreens(col, sz);
            m_collapsedLocation = col;
            targetLeft = col.x;
            targetTop = col.y;
            targetH = collapsedH;
            m_expanded = false;
        }

        int targetW = Scale(BAR_WIDTH, m_uiScale);

        SetWindowPos(
            m_hwnd, NULL,
            targetLeft, targetTop,
            targetW, targetH,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);

        m_renderer->Resize(targetW, targetH);
        RedrawWindow(m_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void MainWindow::OnWindowMoved() {
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        POINT pt = { rc.left, rc.top };
        SIZE sz = { rc.right - rc.left, rc.bottom - rc.top };
        POINT clamped = ClampToScreens(pt, sz);
        SetWindowPos(m_hwnd, NULL, clamped.x, clamped.y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);

        m_collapsedLocation = clamped;
        m_hasCollapsedLocation = true;
        ConfigStore::SavePosition(m_collapsedLocation);
    }

    void MainWindow::SetStats(const TokenStats& stats) {
        m_snapshot.stats = stats;
        if (m_hwnd) {
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
    }

    bool MainWindow::PostStats(TokenStats stats) {
        if (m_shuttingDown.load()) return false;
        const HWND hwnd = m_hwnd.load();
        if (!hwnd) return false;
        {
            const std::lock_guard<std::mutex> lock(m_statsMutex);
            if (m_shuttingDown.load()) return false;
            m_pendingStats = std::move(stats);
        }
        if (PostMessageW(hwnd, WM_CQB_STATS, 0, 0)) return true;
        const std::lock_guard<std::mutex> lock(m_statsMutex);
        m_pendingStats.reset();
        return false;
    }

    void MainWindow::BeginShutdown() {
        m_shuttingDown = true;
        if (m_fetchState) {
            m_fetchState->canceled = true;
            const std::lock_guard<std::mutex> lock(m_fetchState->mutex);
            m_fetchState->pending.reset();
        }
        if (m_companionPollState) {
            m_companionPollState->canceled = true;
        }
        {
            const std::lock_guard<std::mutex> lock(m_statsMutex);
            m_pendingStats.reset();
        }
        if (m_fetchThread.joinable()) {
            m_fetchThread.detach();
        }
    }

    void MainWindow::RefreshQuota() {
        if (!m_hwnd || m_shuttingDown.load()) return;
        if (m_fetchState->inFlight.exchange(true)) {
            m_refreshQueued = true;
            return;
        }

        m_syncState = SyncState::Syncing;
        InvalidateRect(m_hwnd, NULL, FALSE);

        if (m_fetchThread.joinable()) {
            m_fetchThread.join();
        }

        HWND hwnd = m_hwnd;
        auto fetchState = m_fetchState;

        try {
            m_fetchThread = std::thread([hwnd, fetchState]() {
                QuotaSnapshot snap = CodexClient::FetchSnapshot();
                {
                    const std::lock_guard<std::mutex> lock(fetchState->mutex);
                    if (fetchState->canceled.load()) return;
                    fetchState->pending = std::move(snap);
                }
                if (!PostMessageW(hwnd, WM_CQB_QUOTA_RESULT, 0, 0)) {
                    const std::lock_guard<std::mutex> lock(fetchState->mutex);
                    fetchState->pending.reset();
                    fetchState->inFlight = false;
                }
            });
        } catch (const std::system_error&) {
            m_fetchState->inFlight = false;
            m_syncState = SyncState::Failed;
            WriteLog(LogLevel::Error, L"额度同步线程创建失败。");
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
    }

    bool MainWindow::IsHeaderArea(POINT pt) const {
        int logicalY = static_cast<int>(std::round(pt.y / m_uiScale));
        return logicalY < COLLAPSED_HEIGHT;
    }

    bool MainWindow::IsExpandButtonArea(POINT pt) const {
        int logicalX = static_cast<int>(std::round(pt.x / m_uiScale));
        int logicalY = static_cast<int>(std::round(pt.y / m_uiScale));

        if (logicalX < 354 || logicalX >= BAR_WIDTH) return false;

        return logicalY >= 66 && logicalY < COLLAPSED_HEIGHT;
    }

    void MainWindow::HandleContextMenu(int screenX, int screenY) {
        const std::vector<MenuItem> items = {
            { IDM_TOGGLE_EXPAND, m_expanded ? L"收起详情" : L"展开详情" },
            { IDM_REFRESH, L"立即刷新" },
            { IDM_REFRESH_INTERVAL_SUB, L"刷新间隔" },
            { IDM_SCALE_SUB, L"缩放大小" },
            { IDM_APPEARANCE_SUB, L"外观配置" },
            { IDM_BACKGROUND_TRANSPARENCY,
              L"透明背景：" +
                  std::to_wstring(m_settings.appearance.backgroundTransparency) + L"%" },
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
        } else if (cmd == IDM_REFRESH) {
            RefreshQuota();
        } else if (cmd == IDM_REFRESH_INTERVAL_SUB) {
            std::vector<MenuItem> intervalItems;
            for (int i = 0; i < kRefreshIntervalCount; ++i) {
                std::wstring text = RefreshIntervalText(i);
                if (i == m_refreshIntervalLevel) text += L" (当前)";
                intervalItems.push_back({ IDM_REFRESH_INTERVAL_BASE + i, text });
            }
            const int interval = CustomMenu::Show(
                m_hwnd, m_uiScale, screenX, screenY, intervalItems,
                m_renderer->Palette(), m_renderer->FontFamily());
            if (interval >= IDM_REFRESH_INTERVAL_BASE &&
                interval < IDM_REFRESH_INTERVAL_BASE + kRefreshIntervalCount) {
                ApplyRefreshInterval(interval - IDM_REFRESH_INTERVAL_BASE);
            }
        } else if (cmd == IDM_SCALE_SUB) {
            std::vector<MenuItem> scaleItems;
            for (int i = 0; i < kUserScaleCount; ++i) {
                std::wstring text = ScaleLevelText(i);
                if (i == m_userScaleLevel) text += L" (当前)";
                scaleItems.push_back({ IDM_SCALE_LEVEL_BASE + i, text });
            }
            const int level = CustomMenu::Show(
                m_hwnd, m_uiScale, screenX, screenY, scaleItems,
                m_renderer->Palette(), m_renderer->FontFamily());
            if (level >= IDM_SCALE_LEVEL_BASE &&
                level < IDM_SCALE_LEVEL_BASE + kUserScaleCount) {
                ApplyUserScale(level - IDM_SCALE_LEVEL_BASE);
            }
        } else if (cmd == IDM_APPEARANCE_SUB) {
            const bool custom =
                m_settings.appearance.mode == AppearanceMode::Custom;
            const std::vector<MenuItem> appearanceItems = {
                { IDM_APPEARANCE_DEFAULT, custom ? L"使用默认外观" : L"使用默认外观 (当前)" },
                { IDM_APPEARANCE_CUSTOM, custom ? L"使用个性外观 (当前)" : L"使用个性外观" },
                { IDM_APPEARANCE_EDIT_COLORS, L"编辑个性配色" },
                { 0, L"" },
                { IDM_APPEARANCE_OPEN_CONFIG, L"高级配置：用户文件" },
                { IDM_APPEARANCE_OPEN_DEFAULT_CONFIG, L"高级配置：默认文件" },
                { IDM_APPEARANCE_RELOAD, L"重新加载外观" },
            };
            const int appearance = CustomMenu::Show(
                m_hwnd, m_uiScale, screenX, screenY, appearanceItems,
                m_renderer->Palette(), m_renderer->FontFamily());
            if (appearance == IDM_APPEARANCE_DEFAULT) {
                SetAppearanceMode(AppearanceMode::Default);
            } else if (appearance == IDM_APPEARANCE_CUSTOM) {
                SetAppearanceMode(AppearanceMode::Custom);
            } else if (appearance == IDM_APPEARANCE_EDIT_COLORS) {
                ConfigureCustomColors();
            } else if (appearance == IDM_APPEARANCE_OPEN_CONFIG) {
                OpenConfigFile();
            } else if (appearance == IDM_APPEARANCE_OPEN_DEFAULT_CONFIG) {
                OpenDefaultConfigFile();
            } else if (appearance == IDM_APPEARANCE_RELOAD) {
                ReloadAppearance();
            }
        } else if (cmd == IDM_BACKGROUND_TRANSPARENCY) {
            ConfigureBackgroundTransparency();
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
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (!m_dragging && IsHeaderArea(pt) && !IsExpandButtonArea(pt)) {
                m_dragging = true;
                SetCapture(m_hwnd);
                m_dragClickPoint = pt;
                GetCursorPos(&m_dragOrigin);
                RECT rc;
                GetWindowRect(m_hwnd, &rc);
                m_dragWindowPos = { rc.left, rc.top };
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (m_dragging) {
                POINT cur;
                int deltaX = 0;
                int deltaY = 0;
                if (GetCursorPos(&cur) && (cur.x != m_dragOrigin.x || cur.y != m_dragOrigin.y)) {
                    deltaX = cur.x - m_dragOrigin.x;
                    deltaY = cur.y - m_dragOrigin.y;
                } else {
                    POINT clientPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    deltaX = clientPt.x - m_dragClickPoint.x;
                    deltaY = clientPt.y - m_dragClickPoint.y;
                }
                SetWindowPos(m_hwnd, NULL,
                    m_dragWindowPos.x + deltaX,
                    m_dragWindowPos.y + deltaY,
                    0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (m_dragging) {
                m_dragging = false;
                ReleaseCapture();
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
            m_dragging = false;
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
            POINT pt;
            GetCursorPos(&pt);
            HandleContextMenu(pt.x, pt.y);
            return 0;
        }

        case WM_CQB_REFRESH:
            RefreshQuota();
            return 0;

        case WM_CQB_STATS: {
            std::optional<TokenStats> stats;
            {
                const std::lock_guard<std::mutex> lock(m_statsMutex);
                stats.swap(m_pendingStats);
            }
            if (stats) SetStats(*stats);
            return 0;
        }

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
                if (!snap->statsSynchronized) snap->stats = m_snapshot.stats;
                m_snapshot = std::move(*snap);
                m_syncState = m_snapshot.success ? SyncState::Synced : SyncState::Failed;
                InvalidateRect(m_hwnd, NULL, FALSE);
            }
            m_fetchState->inFlight = false;
            if (m_refreshQueued.exchange(false)) {
                RefreshQuota();
            }
            return 0;
        }

        case WM_CQB_COMPANION_RESULT:
            HandleCompanionResult(wParam != 0);
            return 0;

        case 0x02E0: // WM_DPICHANGED
            OnDpiChanged(LOWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_REFRESH_ID) {
                if (!m_companionMode || m_codexDesktopRunning) {
                    RefreshQuota();
                }
            } else if (wParam == TIMER_COMPANION_ID) {
                PollCompanionMode();
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            if (m_hasCollapsedLocation) {
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
        }

        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }

    void MainWindow::OnPaint() {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);

        HRESULT hr = m_renderer->Render(m_expanded, m_snapshot, m_syncState);
        if (hr == D2DERR_RECREATE_TARGET) {
            InvalidateRect(m_hwnd, NULL, FALSE);
        }

        EndPaint(m_hwnd, &ps);
    }

} // namespace CodexQuotaBar
