#include "UI/AppearanceDialog.h"

#include "Core/Appearance.h"
#include "Core/DpiHelper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace CodexQuotaBar {
namespace {

    constexpr UINT WM_CQB_APPEARANCE_CONFIRM = WM_APP + 22;
    constexpr int kEditControlBase = 100;
    constexpr size_t kColorCount = 19;

    struct ColorSpec {
        const wchar_t* key;
        const wchar_t* label;
    };

    constexpr std::array<ColorSpec, kColorCount> kColorSpecs = {{
        { L"Surface", L"主背景" },
        { L"StatsCardBackground", L"子卡片背景" },
        { L"StatsCardBorder", L"子卡片边框" },
        { L"Text", L"主文字" },
        { L"MutedText", L"辅助文字" },
        { L"TrackBackground", L"进度槽背景" },
        { L"Unavailable", L"未返回状态" },
        { L"ProgressHigh", L"额度充足" },
        { L"ProgressMedium", L"额度中等" },
        { L"ProgressLow", L"额度偏低" },
        { L"ProgressCritical", L"额度告急" },
        { L"OuterBorder", L"外边框" },
        { L"Divider", L"分隔线" },
        { L"Chevron", L"展开箭头" },
        { L"SyncSuccess", L"同步成功" },
        { L"SyncIdle", L"同步等待" },
        { L"SyncBusy", L"同步进行" },
        { L"MenuHover", L"菜单悬停" },
        { L"MenuDivider", L"菜单分隔线" },
    }};

    struct ColorField {
        HWND edit = nullptr;
        RECT outline = {};
        RECT swatch = {};
        bool focused = false;
        bool invalid = false;
    };

    struct AppearanceDialogState {
        std::array<ColorField, kColorCount> fields;
        std::map<std::wstring, std::wstring> selectedColors;
        int hoverButton = -1;
        int invalidField = -1;
        bool accepted = false;
        bool done = false;
        float dpiScale = 1.0f;
        ThemePalette palette;
        std::wstring validationMessage;
        RECT confirmRect = {};
        RECT cancelRect = {};
        HFONT editFont = nullptr;
        HBRUSH editBrush = nullptr;

        ComPtr<ID2D1Factory> d2dFactory;
        ComPtr<IDWriteFactory> writeFactory;
        ComPtr<IDWriteTextFormat> titleFont;
        ComPtr<IDWriteTextFormat> bodyFont;
        ComPtr<IDWriteTextFormat> hintFont;
        ComPtr<ID2D1DCRenderTarget> renderTarget;
        ComPtr<ID2D1SolidColorBrush> brush;
    };

    BYTE ColorByte(float value) {
        return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    COLORREF ToColorRef(const D2D1_COLOR_F& color) {
        return RGB(ColorByte(color.r), ColorByte(color.g), ColorByte(color.b));
    }

    bool EnsureRenderTarget(AppearanceDialogState* state) {
        if (state->renderTarget) return true;
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (!state->d2dFactory ||
            FAILED(state->d2dFactory->CreateDCRenderTarget(
                &properties, &state->renderTarget)) ||
            FAILED(state->renderTarget->CreateSolidColorBrush(
                state->palette.text, &state->brush))) {
            state->brush.Reset();
            state->renderTarget.Reset();
            return false;
        }
        state->renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        return true;
    }

    void DrawText(
        AppearanceDialogState* state,
        const wchar_t* text,
        IDWriteTextFormat* format,
        const D2D1_RECT_F& rect,
        const D2D1_COLOR_F& color)
    {
        if (!format || !text) return;
        state->brush->SetColor(color);
        state->renderTarget->DrawTextW(
            text,
            static_cast<UINT32>(std::wcslen(text)),
            format,
            rect,
            state->brush.Get());
    }

    void DrawButton(
        AppearanceDialogState* state,
        const RECT& buttonRect,
        const wchar_t* text,
        bool hovered)
    {
        const D2D1_RECT_F rect = D2D1::RectF(
            static_cast<float>(buttonRect.left),
            static_cast<float>(buttonRect.top),
            static_cast<float>(buttonRect.right),
            static_cast<float>(buttonRect.bottom));
        const float radius = ScaleF(6.0f, state->dpiScale);
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        state->brush->SetColor(hovered ? state->palette.menuHover : state->palette.statsCardBg);
        state->renderTarget->FillRoundedRectangle(rounded, state->brush.Get());
        state->brush->SetColor(state->palette.statsCardBorder);
        state->renderTarget->DrawRoundedRectangle(rounded, state->brush.Get(), 1.0f);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawText(state, text, state->bodyFont.Get(), rect, state->palette.text);
    }

    std::wstring ReadFieldText(HWND edit) {
        wchar_t value[16] = {};
        GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        return value;
    }

    bool RenderDialog(HDC hdc, const RECT& clientRect, AppearanceDialogState* state) {
        if (!hdc || !EnsureRenderTarget(state)) return false;
        RECT bound = clientRect;
        if (FAILED(state->renderTarget->BindDC(hdc, &bound))) return false;

        const float width = static_cast<float>(clientRect.right);
        const float height = static_cast<float>(clientRect.bottom);
        const float scale = state->dpiScale;
        state->renderTarget->BeginDraw();
        state->brush->SetColor(state->palette.surface);
        state->renderTarget->FillRectangle(D2D1::RectF(0, 0, width, height), state->brush.Get());

        state->titleFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        state->hintFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->hintFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        DrawText(
            state, L"编辑个性配色", state->titleFont.Get(),
            D2D1::RectF(ScaleF(16, scale), ScaleF(8, scale), width, ScaleF(42, scale)),
            state->palette.text);
        state->brush->SetColor(state->palette.menuDivider);
        state->renderTarget->DrawLine(
            D2D1::Point2F(ScaleF(12, scale), ScaleF(44, scale)),
            D2D1::Point2F(width - ScaleF(12, scale), ScaleF(44, scale)),
            state->brush.Get(), 1.0f);

        for (size_t index = 0; index < kColorCount; ++index) {
            ColorField& field = state->fields[index];
            const D2D1_RECT_F outline = D2D1::RectF(
                static_cast<float>(field.outline.left),
                static_cast<float>(field.outline.top),
                static_cast<float>(field.outline.right),
                static_cast<float>(field.outline.bottom));
            const int column = index < 10 ? 0 : 1;
            const float labelLeft = ScaleF(column == 0 ? 18.0f : 386.0f, scale);
            DrawText(
                state, kColorSpecs[index].label, state->bodyFont.Get(),
                D2D1::RectF(
                    labelLeft,
                    static_cast<float>(field.outline.top),
                    labelLeft + ScaleF(106.0f, scale),
                    static_cast<float>(field.outline.bottom)),
                state->palette.text);

            const float radius = ScaleF(5.0f, scale);
            state->brush->SetColor(state->palette.trackBg);
            state->renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(outline, radius, radius), state->brush.Get());
            state->brush->SetColor(
                field.invalid
                    ? state->palette.progressRed
                    : (field.focused
                        ? state->palette.progressGreen
                        : state->palette.statsCardBorder));
            state->renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(outline, radius, radius),
                state->brush.Get(),
                field.invalid || field.focused ? 2.0f : 1.0f);

            uint32_t rgb = 0;
            const std::wstring value = ReadFieldText(field.edit);
            const D2D1_RECT_F swatch = D2D1::RectF(
                static_cast<float>(field.swatch.left),
                static_cast<float>(field.swatch.top),
                static_cast<float>(field.swatch.right),
                static_cast<float>(field.swatch.bottom));
            state->brush->SetColor(
                TryParseAppearanceColor(value, rgb)
                    ? D2D1::ColorF(rgb)
                    : state->palette.trackBg);
            state->renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(swatch, radius, radius), state->brush.Get());
            state->brush->SetColor(state->palette.statsCardBorder);
            state->renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(swatch, radius, radius), state->brush.Get(), 1.0f);
        }

        const wchar_t* hint = state->invalidField >= 0
            ? state->validationMessage.c_str()
            : L"确认后将保存全部配色，并自动启用个性外观";
        DrawText(
            state, hint, state->hintFont.Get(),
            D2D1::RectF(
                ScaleF(18, scale), ScaleF(410, scale),
                ScaleF(500, scale), ScaleF(444, scale)),
            state->invalidField >= 0 ? state->palette.progressRed : state->palette.muted);

        DrawButton(state, state->confirmRect, L"确定", state->hoverButton == 0);
        DrawButton(state, state->cancelRect, L"取消", state->hoverButton == 1);

        state->brush->SetColor(state->palette.statsCardBorder);
        state->renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f),
                ScaleF(8, scale), ScaleF(8, scale)),
            state->brush.Get(), 1.0f);

        const HRESULT result = state->renderTarget->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            state->brush.Reset();
            state->renderTarget.Reset();
            return false;
        }
        return SUCCEEDED(result);
    }

    void CloseDialog(HWND hwnd, AppearanceDialogState* state, bool accepted) {
        state->accepted = accepted;
        state->done = true;
        DestroyWindow(hwnd);
    }

    void ConfirmDialog(HWND hwnd, AppearanceDialogState* state) {
        std::map<std::wstring, std::wstring> colors;
        state->invalidField = -1;
        for (size_t index = 0; index < kColorCount; ++index) {
            ColorField& field = state->fields[index];
            const std::wstring value = ReadFieldText(field.edit);
            uint32_t rgb = 0;
            field.invalid = !TryParseAppearanceColor(value, rgb);
            if (field.invalid && state->invalidField < 0) {
                state->invalidField = static_cast<int>(index);
            }
            colors[kColorSpecs[index].key] = value;
        }
        if (state->invalidField >= 0) {
            state->validationMessage = L"“";
            state->validationMessage += kColorSpecs[state->invalidField].label;
            state->validationMessage += L"”必须使用 #RRGGBB 格式";
            HWND edit = state->fields[state->invalidField].edit;
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        state->selectedColors = std::move(colors);
        CloseDialog(hwnd, state, true);
    }

    LRESULT CALLBACK AppearanceWndProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        auto* state = reinterpret_cast<AppearanceDialogState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT client = {};
            GetClientRect(hwnd, &client);
            RenderDialog(hdc, client, state);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetTextColor(editDc, ToColorRef(state->palette.text));
            SetBkColor(editDc, ToColorRef(state->palette.trackBg));
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            if (controlId >= kEditControlBase &&
                controlId < kEditControlBase + static_cast<int>(kColorCount)) {
                ColorField& field = state->fields[controlId - kEditControlBase];
                if (HIWORD(wParam) == EN_SETFOCUS) field.focused = true;
                if (HIWORD(wParam) == EN_KILLFOCUS) field.focused = false;
                if (HIWORD(wParam) == EN_CHANGE) {
                    field.invalid = false;
                    state->invalidField = -1;
                    for (size_t index = 0; index < kColorCount; ++index) {
                        if (state->fields[index].invalid) {
                            state->invalidField = static_cast<int>(index);
                            state->validationMessage = L"“";
                            state->validationMessage += kColorSpecs[index].label;
                            state->validationMessage += L"”必须使用 #RRGGBB 格式";
                            break;
                        }
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tracking);
            const POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int hover = -1;
            if (PtInRect(&state->confirmRect, point)) hover = 0;
            else if (PtInRect(&state->cancelRect, point)) hover = 1;
            if (hover != state->hoverButton) {
                state->hoverButton = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            state->hoverButton = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (point.y < Scale(44.0f, state->dpiScale)) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            const POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&state->confirmRect, point)) ConfirmDialog(hwnd, state);
            else if (PtInRect(&state->cancelRect, point)) CloseDialog(hwnd, state, false);
            return 0;
        }
        case WM_CQB_APPEARANCE_CONFIRM:
            ConfirmDialog(hwnd, state);
            return 0;
        case WM_CLOSE:
            CloseDialog(hwnd, state, false);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool InitializeDrawing(AppearanceDialogState& state, const std::wstring& fontFamily) {
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory), &options, &state.d2dFactory)) ||
            FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), &state.writeFactory))) {
            return false;
        }
        const float scale = state.dpiScale;
        return SUCCEEDED(state.writeFactory->CreateTextFormat(
                   fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_BOLD,
                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                   16.0f * scale, L"zh-cn", &state.titleFont)) &&
               SUCCEEDED(state.writeFactory->CreateTextFormat(
                   fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                   14.0f * scale, L"zh-cn", &state.bodyFont)) &&
               SUCCEEDED(state.writeFactory->CreateTextFormat(
                   fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                   13.0f * scale, L"zh-cn", &state.hintFont));
    }

    float FitScaleToMonitor(HWND owner, float requestedScale) {
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info)) return requestedScale;
        const float workWidth = static_cast<float>(info.rcWork.right - info.rcWork.left);
        const float workHeight = static_cast<float>(info.rcWork.bottom - info.rcWork.top);
        return (std::min)({ requestedScale, workWidth / 752.0f, workHeight / 474.0f });
    }

    POINT CenterAndClamp(HWND owner, int width, int height) {
        RECT ownerRect = {};
        GetWindowRect(owner, &ownerRect);
        POINT position = {
            ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
            ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2
        };
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(monitor, &info)) {
            position.x = std::clamp(position.x, info.rcWork.left, info.rcWork.right - width);
            position.y = std::clamp(position.y, info.rcWork.top, info.rcWork.bottom - height);
        }
        return position;
    }

} // namespace

    bool ShowAppearanceColorsDialog(
        HWND owner,
        float dpiScale,
        const std::map<std::wstring, std::wstring>& currentColors,
        std::map<std::wstring, std::wstring>& selectedColors,
        const ThemePalette& palette,
        const std::wstring& fontFamily)
    {
        AppearanceDialogState state;
        state.dpiScale = FitScaleToMonitor(owner, dpiScale);
        state.palette = palette;
        if (!InitializeDrawing(state, fontFamily)) return false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(WNDCLASSEXW);
            windowClass.style = CS_DROPSHADOW;
            windowClass.lpfnWndProc = &AppearanceWndProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = L"Codex-Quota-Bar_Appearance";
            if (!RegisterClassExW(&windowClass) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }
            registered = true;
        }

        const float scale = state.dpiScale;
        const int width = Scale(740.0f, scale);
        const int height = Scale(462.0f, scale);
        const POINT position = CenterAndClamp(owner, width, height);
        state.confirmRect = {
            Scale(530, scale), Scale(414, scale),
            Scale(622, scale), Scale(448, scale)
        };
        state.cancelRect = {
            Scale(632, scale), Scale(414, scale),
            Scale(724, scale), Scale(448, scale)
        };

        HWND dialog = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
            L"Codex-Quota-Bar_Appearance", L"编辑个性配色", WS_POPUP,
            position.x, position.y, width, height,
            owner, nullptr, GetModuleHandleW(nullptr), &state);
        if (!dialog) return false;

        const int cornerPreference = DWMWCP_ROUND;
        DwmSetWindowAttribute(
            dialog, DWMWA_WINDOW_CORNER_PREFERENCE,
            &cornerPreference, sizeof(cornerPreference));
        state.editBrush = CreateSolidBrush(ToColorRef(state.palette.trackBg));
        state.editFont = CreateFontW(
            -Scale(14.0f, scale), 0, 0, 0, FW_REGULAR,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, fontFamily.c_str());
        if (!state.editBrush || !state.editFont) {
            DestroyWindow(dialog);
            if (state.editFont) DeleteObject(state.editFont);
            if (state.editBrush) DeleteObject(state.editBrush);
            return false;
        }

        const auto defaults = DefaultAppearanceColors();
        for (size_t index = 0; index < kColorCount; ++index) {
            const int column = index < 10 ? 0 : 1;
            const int row = column == 0 ? static_cast<int>(index) : static_cast<int>(index) - 10;
            const float columnX = column == 0 ? 18.0f : 386.0f;
            const float rowY = 58.0f + row * 34.0f;
            ColorField& field = state.fields[index];
            field.outline = {
                Scale(columnX + 112.0f, scale), Scale(rowY, scale),
                Scale(columnX + 264.0f, scale), Scale(rowY + 28.0f, scale)
            };
            field.swatch = {
                Scale(columnX + 274.0f, scale), Scale(rowY + 2.0f, scale),
                Scale(columnX + 310.0f, scale), Scale(rowY + 26.0f, scale)
            };
            auto found = currentColors.find(kColorSpecs[index].key);
            const std::wstring& value = found != currentColors.end()
                ? found->second
                : defaults.at(kColorSpecs[index].key);
            field.edit = CreateWindowExW(
                0, L"EDIT", value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_UPPERCASE | ES_AUTOHSCROLL,
                field.outline.left + Scale(6, scale),
                field.outline.top + Scale(3, scale),
                field.outline.right - field.outline.left - Scale(12, scale),
                field.outline.bottom - field.outline.top - Scale(6, scale),
                dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditControlBase + index)),
                GetModuleHandleW(nullptr), nullptr);
            if (!field.edit) {
                DestroyWindow(dialog);
                DeleteObject(state.editFont);
                DeleteObject(state.editBrush);
                return false;
            }
            SendMessageW(field.edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.editFont), TRUE);
            SendMessageW(field.edit, EM_SETLIMITTEXT, 7, 0);
        }

        EnableWindow(owner, FALSE);
        ShowWindow(dialog, SW_SHOWNORMAL);
        UpdateWindow(dialog);
        SetFocus(state.fields[0].edit);
        SendMessageW(state.fields[0].edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));

        MSG message = {};
        while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN) {
                if (message.wParam == VK_RETURN) {
                    SendMessageW(dialog, WM_CQB_APPEARANCE_CONFIRM, 0, 0);
                    continue;
                }
                if (message.wParam == VK_ESCAPE) {
                    SendMessageW(dialog, WM_CLOSE, 0, 0);
                    continue;
                }
            }
            if (!IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (IsWindow(dialog)) DestroyWindow(dialog);
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        DeleteObject(state.editFont);
        DeleteObject(state.editBrush);
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
        }
        if (!state.accepted) return false;
        selectedColors = std::move(state.selectedColors);
        return true;
    }

} // namespace CodexQuotaBar
