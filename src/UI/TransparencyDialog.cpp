#include "UI/TransparencyDialog.h"

#include "Core/Appearance.h"
#include "Core/DpiHelper.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>

namespace CodexQuotaBar {
namespace {

    constexpr UINT WM_CQB_DIALOG_CONFIRM = WM_APP + 21;
    constexpr int kEditControlId = 1;

    struct TransparencyDialogState {
        int selectedValue = 0;
        int hoverButton = -1;
        bool editFocused = true;
        bool validationError = false;
        bool accepted = false;
        bool done = false;
        float dpiScale = 1.0f;
        ThemePalette palette;
        RECT confirmRect = {};
        RECT cancelRect = {};
        HWND edit = nullptr;
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

    bool EnsureRenderTarget(TransparencyDialogState* state) {
        if (state->renderTarget) return true;
        if (!state->d2dFactory) return false;

        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(state->d2dFactory->CreateDCRenderTarget(
                &properties,
                &state->renderTarget)) ||
            FAILED(state->renderTarget->CreateSolidColorBrush(
                state->palette.text,
                &state->brush))) {
            state->brush.Reset();
            state->renderTarget.Reset();
            return false;
        }
        state->renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        return true;
    }

    void DrawText(
        TransparencyDialogState* state,
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
        TransparencyDialogState* state,
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

        if (state->bodyFont) {
            state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        DrawText(state, text, state->bodyFont.Get(), rect, state->palette.text);
    }

    bool RenderDialog(HDC hdc, const RECT& clientRect, TransparencyDialogState* state) {
        if (!hdc || !EnsureRenderTarget(state)) return false;
        RECT bound = clientRect;
        if (FAILED(state->renderTarget->BindDC(hdc, &bound))) return false;

        const float width = static_cast<float>(clientRect.right - clientRect.left);
        const float height = static_cast<float>(clientRect.bottom - clientRect.top);
        const float scale = state->dpiScale;

        state->renderTarget->BeginDraw();
        state->brush->SetColor(state->palette.surface);
        state->renderTarget->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, width, height),
            state->brush.Get());

        if (state->titleFont) {
            state->titleFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            state->titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (state->bodyFont) {
            state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (state->hintFont) {
            state->hintFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            state->hintFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        DrawText(
            state,
            L"透明背景",
            state->titleFont.Get(),
            D2D1::RectF(
                ScaleF(16.0f, scale), ScaleF(8.0f, scale),
                width - ScaleF(16.0f, scale), ScaleF(42.0f, scale)),
            state->palette.text);

        state->brush->SetColor(state->palette.menuDivider);
        state->renderTarget->DrawLine(
            D2D1::Point2F(ScaleF(12.0f, scale), ScaleF(44.0f, scale)),
            D2D1::Point2F(width - ScaleF(12.0f, scale), ScaleF(44.0f, scale)),
            state->brush.Get(),
            1.0f);

        DrawText(
            state,
            L"背景透明度",
            state->bodyFont.Get(),
            D2D1::RectF(
                ScaleF(16.0f, scale), ScaleF(54.0f, scale),
                ScaleF(205.0f, scale), ScaleF(90.0f, scale)),
            state->palette.text);

        const D2D1_RECT_F editOutline = D2D1::RectF(
            ScaleF(224.0f, scale), ScaleF(54.0f, scale),
            ScaleF(344.0f, scale), ScaleF(90.0f, scale));
        const float editRadius = ScaleF(6.0f, scale);
        state->brush->SetColor(state->palette.trackBg);
        state->renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(editOutline, editRadius, editRadius),
            state->brush.Get());
        state->brush->SetColor(
            state->validationError
                ? state->palette.progressRed
                : (state->editFocused
                    ? state->palette.progressGreen
                    : state->palette.statsCardBorder));
        state->renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(editOutline, editRadius, editRadius),
            state->brush.Get(),
            state->editFocused || state->validationError ? 2.0f : 1.0f);

        DrawText(
            state,
            L"0 = 不透明，90 = 最大透明度",
            state->hintFont.Get(),
            D2D1::RectF(
                ScaleF(16.0f, scale), ScaleF(94.0f, scale),
                width - ScaleF(16.0f, scale), ScaleF(122.0f, scale)),
            state->palette.muted);

        if (state->validationError) {
            DrawText(
                state,
                L"请输入 0 至 90 的整数",
                state->hintFont.Get(),
                D2D1::RectF(
                    ScaleF(16.0f, scale), ScaleF(120.0f, scale),
                    width - ScaleF(16.0f, scale), ScaleF(146.0f, scale)),
                state->palette.progressRed);
        }

        DrawButton(
            state,
            state->confirmRect,
            L"确定",
            state->hoverButton == 0);
        DrawButton(
            state,
            state->cancelRect,
            L"取消",
            state->hoverButton == 1);

        const float borderRadius = ScaleF(8.0f, scale);
        state->brush->SetColor(state->palette.statsCardBorder);
        state->renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f),
                borderRadius,
                borderRadius),
            state->brush.Get(),
            1.0f);

        const HRESULT result = state->renderTarget->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            state->brush.Reset();
            state->renderTarget.Reset();
            return false;
        }
        return SUCCEEDED(result);
    }

    void CloseDialog(HWND hwnd, TransparencyDialogState* state, bool accepted) {
        state->accepted = accepted;
        state->done = true;
        DestroyWindow(hwnd);
    }

    void ConfirmDialog(HWND hwnd, TransparencyDialogState* state) {
        wchar_t buffer[16] = {};
        const int length = GetWindowTextW(
            state->edit,
            buffer,
            static_cast<int>(std::size(buffer)));
        wchar_t* end = nullptr;
        errno = 0;
        const long value = length > 0 ? std::wcstol(buffer, &end, 10) : -1;
        if (errno != 0 || end == buffer || *end != L'\0' ||
            value < 0 || value > 90 ||
            !IsValidBackgroundTransparency(static_cast<int>(value))) {
            state->validationError = true;
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        state->selectedValue = static_cast<int>(value);
        CloseDialog(hwnd, state, true);
    }

    LRESULT CALLBACK TransparencyWndProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        auto* state = reinterpret_cast<TransparencyDialogState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT clientRect = {};
            GetClientRect(hwnd, &clientRect);
            RenderDialog(hdc, clientRect, state);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetTextColor(editDc, ToColorRef(state->palette.text));
            SetBkColor(editDc, ToColorRef(state->palette.trackBg));
            return reinterpret_cast<LRESULT>(state->editBrush);
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == kEditControlId) {
                if (HIWORD(wParam) == EN_SETFOCUS) state->editFocused = true;
                if (HIWORD(wParam) == EN_KILLFOCUS) state->editFocused = false;
                if (HIWORD(wParam) == EN_CHANGE) state->validationError = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking = {
                sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0
            };
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
            if (PtInRect(&state->confirmRect, point)) {
                ConfirmDialog(hwnd, state);
            } else if (PtInRect(&state->cancelRect, point)) {
                CloseDialog(hwnd, state, false);
            }
            return 0;
        }

        case WM_CQB_DIALOG_CONFIRM:
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

    bool InitializeDrawing(
        TransparencyDialogState& state,
        const std::wstring& fontFamily)
    {
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory),
                &options,
                &state.d2dFactory)) ||
            FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                &state.writeFactory))) {
            return false;
        }

        const float scale = state.dpiScale;
        if (FAILED(state.writeFactory->CreateTextFormat(
                fontFamily.c_str(), nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale,
                L"zh-cn", &state.titleFont)) ||
            FAILED(state.writeFactory->CreateTextFormat(
                fontFamily.c_str(), nullptr,
                DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale,
                L"zh-cn", &state.bodyFont)) ||
            FAILED(state.writeFactory->CreateTextFormat(
                fontFamily.c_str(), nullptr,
                DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale,
                L"zh-cn", &state.hintFont))) {
            return false;
        }
        return true;
    }

    POINT CenterAndClamp(HWND owner, int width, int height) {
        RECT ownerRect = {};
        GetWindowRect(owner, &ownerRect);
        POINT position = {
            ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
            ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2
        };

        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            position.x = std::clamp(
                position.x,
                monitorInfo.rcWork.left,
                monitorInfo.rcWork.right - width);
            position.y = std::clamp(
                position.y,
                monitorInfo.rcWork.top,
                monitorInfo.rcWork.bottom - height);
        }
        return position;
    }

} // namespace

    bool ShowTransparencyDialog(
        HWND owner,
        float dpiScale,
        int currentValue,
        int& selectedValue,
        const ThemePalette& palette,
        const std::wstring& fontFamily)
    {
        TransparencyDialogState state;
        state.selectedValue = currentValue;
        state.dpiScale = dpiScale;
        state.palette = palette;
        if (!InitializeDrawing(state, fontFamily)) return false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(WNDCLASSEXW);
            windowClass.style = CS_DROPSHADOW;
            windowClass.lpfnWndProc = &TransparencyWndProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hbrBackground = nullptr;
            windowClass.lpszClassName = L"Codex-Quota-Bar_Transparency";
            if (!RegisterClassExW(&windowClass) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }
            registered = true;
        }

        const int width = Scale(360.0f, dpiScale);
        const int height = Scale(196.0f, dpiScale);
        const POINT position = CenterAndClamp(owner, width, height);
        state.confirmRect = {
            Scale(150.0f, dpiScale), Scale(150.0f, dpiScale),
            Scale(242.0f, dpiScale), Scale(184.0f, dpiScale)
        };
        state.cancelRect = {
            Scale(252.0f, dpiScale), Scale(150.0f, dpiScale),
            Scale(344.0f, dpiScale), Scale(184.0f, dpiScale)
        };

        HWND dialog = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"Codex-Quota-Bar_Transparency",
            L"透明背景",
            WS_POPUP,
            position.x,
            position.y,
            width,
            height,
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            &state);
        if (!dialog) return false;

        const int cornerPreference = DWMWCP_ROUND;
        DwmSetWindowAttribute(
            dialog,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &cornerPreference,
            sizeof(cornerPreference));

        state.editBrush = CreateSolidBrush(ToColorRef(state.palette.trackBg));
        state.editFont = CreateFontW(
            -Scale(16.0f, dpiScale), 0, 0, 0,
            FW_REGULAR, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            fontFamily.c_str());
        state.edit = CreateWindowExW(
            0,
            L"EDIT",
            std::to_wstring(currentValue).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL,
            Scale(230.0f, dpiScale),
            Scale(59.0f, dpiScale),
            Scale(108.0f, dpiScale),
            Scale(26.0f, dpiScale),
            dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditControlId)),
            GetModuleHandleW(nullptr),
            nullptr);
        if (!state.edit || !state.editBrush || !state.editFont) {
            DestroyWindow(dialog);
            if (state.editFont) DeleteObject(state.editFont);
            if (state.editBrush) DeleteObject(state.editBrush);
            return false;
        }
        SendMessageW(
            state.edit,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(state.editFont),
            TRUE);

        EnableWindow(owner, FALSE);
        ShowWindow(dialog, SW_SHOWNORMAL);
        UpdateWindow(dialog);
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));

        MSG message = {};
        while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN) {
                if (message.wParam == VK_RETURN) {
                    SendMessageW(dialog, WM_CQB_DIALOG_CONFIRM, 0, 0);
                    continue;
                }
                if (message.wParam == VK_ESCAPE) {
                    SendMessageW(dialog, WM_CLOSE, 0, 0);
                    continue;
                }
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (IsWindow(dialog)) DestroyWindow(dialog);
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        if (state.editFont) DeleteObject(state.editFont);
        if (state.editBrush) DeleteObject(state.editBrush);

        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
        }
        if (!state.accepted) return false;
        selectedValue = state.selectedValue;
        return true;
    }

} // namespace CodexQuotaBar
