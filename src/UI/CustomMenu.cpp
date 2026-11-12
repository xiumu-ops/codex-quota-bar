#include "UI/CustomMenu.h"
#include "UI/WindowEffects.h"
#include "Core/Layout.h"
#include "Graphics/PixelText.h"

#include <algorithm>
#include <cmath>

namespace CodexQuotaBar {

    namespace {

        constexpr UINT WM_CQB_MENU_END = WM_APP + 20;
        thread_local HWND activeMenu = nullptr;

        struct MenuRow {
            RECT rect{};
            std::wstring text;
        };

        struct MenuPopupState {
            std::vector<MenuRow> rows;
            std::vector<int> ids;
            int hover = -1;
            int pressedRow = -1;
            int result = 0;
            bool tracking = false;
            float dpiScale = 1.0f;
            ThemePalette palette;

            ComPtr<ID2D1Factory> pD2DFactory;
            ComPtr<IDWriteFactory> pDWriteFactory;
            ComPtr<IDWriteTextFormat> pFontMenu;
            ComPtr<ID2D1DCRenderTarget> pMenuRenderTarget;
            ComPtr<ID2D1SolidColorBrush> pMenuBrush;
            PixelTextCache textCache;
        };

        bool MeasureMenuText(
            IDWriteFactory* pDWriteFactory,
            IDWriteTextFormat* pFontMenu,
            const wchar_t* text,
            SIZE& outSize)
        {
            if (!pDWriteFactory || !pFontMenu || !text || !*text) return false;
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(pDWriteFactory->CreateTextLayout(
                    text, static_cast<UINT32>(wcslen(text)), pFontMenu, 10000.0f, 100.0f, &layout))) {
                return false;
            }
            DWRITE_TEXT_METRICS metrics = {};
            layout->GetMetrics(&metrics);
            outSize.cx = static_cast<LONG>(std::ceil(metrics.widthIncludingTrailingWhitespace));
            outSize.cy = static_cast<LONG>(std::ceil(metrics.height));
            return true;
        }

        bool EnsureMenuRenderTarget(MenuPopupState* st) {
            if (st->pMenuRenderTarget) return true;
            if (!st->pD2DFactory) return false;

            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(st->pD2DFactory->CreateDCRenderTarget(&props, &st->pMenuRenderTarget)) ||
                FAILED(st->pMenuRenderTarget->CreateSolidColorBrush(
                    st->palette.surface, &st->pMenuBrush))) {
                st->pMenuBrush.Reset();
                st->pMenuRenderTarget.Reset();
                return false;
            }
            st->pMenuRenderTarget->SetDpi(96.0f, 96.0f);
            st->pMenuRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            return true;
        }

        bool RenderMenuSurface(HDC hdc, const RECT& clientRect, MenuPopupState* st) {
            if (!EnsureMenuRenderTarget(st) || !hdc) return false;

            RECT bound = clientRect;
            if (FAILED(st->pMenuRenderTarget->BindDC(hdc, &bound))) return false;

            const float w = static_cast<float>(clientRect.right - clientRect.left);
            const float h = static_cast<float>(clientRect.bottom - clientRect.top);
            const float padX = static_cast<float>(ScaleEven(12.0f, st->dpiScale));
            const float hoverRadius = SurfaceCornerRadius(st->dpiScale, w, h);
            const float hairline = static_cast<float>(ScaleEven(1.0f, st->dpiScale));

            st->pMenuRenderTarget->BeginDraw();
            st->pMenuRenderTarget->SetDpi(96.0f, 96.0f);

            // 背景与主面板 surface 完全一致；窗口外形由 DWM 圆角裁剪
            st->pMenuBrush->SetColor(st->palette.surface);
            st->pMenuRenderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, w, h), st->pMenuBrush.Get());

            if (st->pFontMenu) {
                st->pFontMenu->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                st->pFontMenu->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            for (size_t i = 0; i < st->rows.size(); ++i) {
                const MenuRow& row = st->rows[i];
                const D2D1_RECT_F rc = D2D1::RectF(
                    static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
                    static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom));

                if (row.text.empty()) {
                    // 分隔线按整像素矩形填充，避免 1px 线条落在偶数行高中心跨两行发虚
                    const float lineTop = std::floor((rc.top + rc.bottom - hairline) * 0.5f);
                    st->pMenuBrush->SetColor(st->palette.menuDivider);
                    st->pMenuRenderTarget->FillRectangle(
                        D2D1::RectF(padX, lineTop, w - padX, lineTop + hairline),
                        st->pMenuBrush.Get());
                    continue;
                }

                if (static_cast<int>(i) == st->hover) {
                    // 悬停高亮：圆角药丸（浅灰）
                    st->pMenuBrush->SetColor(st->palette.menuHover);
                    D2D1_ROUNDED_RECT hoverRect =
                        D2D1::RoundedRect(rc, hoverRadius, hoverRadius);
                    st->pMenuRenderTarget->FillRoundedRectangle(hoverRect, st->pMenuBrush.Get());
                }

                if (st->pFontMenu) {
                    st->pMenuBrush->SetColor(st->ids[i] != 0 ? st->palette.text : st->palette.muted);
                    D2D1_RECT_F textRect = D2D1::RectF(rc.left + padX, rc.top, rc.right - padX, rc.bottom);
                    st->textCache.Draw(st->pMenuRenderTarget.Get(), st->pDWriteFactory.Get(),
                        row.text.c_str(), static_cast<UINT32>(row.text.size()),
                        st->pFontMenu.Get(), textRect, st->pMenuBrush.Get());
                }
            }

            const HRESULT hr = st->pMenuRenderTarget->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                st->pMenuBrush.Reset();
                st->pMenuRenderTarget.Reset();
                return false;
            }
            return SUCCEEDED(hr);
        }

        int HitMenuRow(const MenuPopupState& state, LPARAM lParam) {
            const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            for (size_t i = 0; i < state.rows.size(); ++i) {
                const auto& row = state.rows[i];
                if (!row.text.empty() && PtInRect(&row.rect, pt)) return static_cast<int>(i);
            }
            return -1;
        }

        bool IsMenuRowEnabled(const MenuPopupState& state, int row) {
            return row >= 0 && state.ids[static_cast<size_t>(row)] != 0;
        }

        void FinishMenu(HWND hwnd, MenuPopupState* state, int result = 0) {
            if (!state || !state->tracking) return;
            state->tracking = false;
            state->result = result;
            PostMessageW(hwnd, WM_CQB_MENU_END, 0, 0);
        }

        LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            if (msg == WM_NCCREATE) {
                CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }

            MenuPopupState* st = reinterpret_cast<MenuPopupState*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                if (st) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    RenderMenuSurface(hdc, rc, st);
                }
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_MOUSEMOVE: {
                if (!st || !st->tracking) break;
                const int row = HitMenuRow(*st, lParam);
                const int newHover = IsMenuRowEnabled(*st, row) ? row : -1;
                if (newHover != st->hover) {
                    st->hover = newHover;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN:
                if (st && st->tracking) {
                    const int row = HitMenuRow(*st, lParam);
                    st->pressedRow = IsMenuRowEnabled(*st, row) ? row : -1;
                    if (row < 0) FinishMenu(hwnd, st);
                }
                return 0;

            case WM_LBUTTONUP: {
                if (!st || !st->tracking) return 0;
                const int row = HitMenuRow(*st, lParam);
                if (row >= 0 && row == st->pressedRow) {
                    FinishMenu(hwnd, st, st->ids[static_cast<size_t>(row)]);
                }
                st->pressedRow = -1;
                return 0;
            }

            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_CANCELMODE:
            case WM_CAPTURECHANGED:
            case WM_CLOSE:
                FinishMenu(hwnd, st);
                return 0;

            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) FinishMenu(hwnd, st);
                break; // DefWindowProc assigns keyboard focus on activation.

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) FinishMenu(hwnd, st);
                return 0;
            }

            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

    } // namespace

    void CustomMenu::Dismiss(HWND owner) {
        const HWND hwnd = activeMenu;
        if (!hwnd || GetWindow(hwnd, GW_OWNER) != owner) return;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        if (GetCapture() == hwnd) ReleaseCapture();
        ShowWindow(hwnd, SW_HIDE);
    }

    int CustomMenu::Show(
        HWND owner,
        float dpiScale,
        int screenX,
        int screenY,
        const std::vector<MenuItem>& items,
        const ThemePalette& palette,
        const std::wstring& fontFamily)
    {
        if (activeMenu) return 0;
        MenuPopupState st;
        st.dpiScale = dpiScale;
        st.palette = palette;

        // 初始化 Direct2D 与 DirectWrite
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, &st.pD2DFactory))) {
            return 0;
        }
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &st.pDWriteFactory))) {
            return 0;
        }

        if (FAILED(st.pDWriteFactory->CreateTextFormat(
                fontFamily.c_str(), NULL,
                DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                static_cast<float>(ScaleEven(14.0f, dpiScale)), L"zh-cn", &st.pFontMenu))) {
            return 0;
        }
        st.pFontMenu->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        // 1. 测量与行几何
        float maxTextW = 0.0f;
        for (const auto& it : items) {
            if (it.text.empty()) continue;
            SIZE sz = {};
            if (MeasureMenuText(st.pDWriteFactory.Get(), st.pFontMenu.Get(), it.text.c_str(), sz)) {
                maxTextW = (std::max)(maxTextW, static_cast<float>(sz.cx));
            }
        }

        const int surfacePad = ScaleEven(4.0f, dpiScale);   // 菜单内容到窗口边
        const int itemHPad = ScaleEven(8.0f, dpiScale);     // 文字上下留白
        const int sepH = ScaleEven(8.0f, dpiScale);         // 分隔线行高
        int menuW = static_cast<int>(std::ceil(maxTextW)) + ScaleEven(24.0f + 8.0f, dpiScale) + ScaleEven(8.0f, dpiScale);
        if (menuW % 2 != 0) menuW++;

        int y = surfacePad;
        for (const auto& it : items) {
            MenuRow row;
            row.text = it.text;
            if (it.text.empty()) {
                row.rect = { surfacePad, y, menuW - surfacePad, y + sepH };
                y += sepH;
            } else {
                SIZE sz = {};
                MeasureMenuText(st.pDWriteFactory.Get(), st.pFontMenu.Get(), it.text.c_str(), sz);
                int rowH = static_cast<int>(sz.cy) + itemHPad;
                if (rowH % 2 != 0) rowH++;
                row.rect = { surfacePad, y, menuW - surfacePad, y + rowH };
                y += rowH;
            }
            st.rows.push_back(std::move(row));
            st.ids.push_back(it.id);
        }
        int menuH = y + surfacePad;
        if (menuH % 2 != 0) menuH++;

        // 2. 屏幕钳制
        HMONITOR hMon = MonitorFromPoint({ screenX, screenY }, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(hMon, &mi);
        if (screenX + menuW > mi.rcWork.right) screenX = mi.rcWork.right - menuW;
        if (screenY + menuH > mi.rcWork.bottom) screenY = mi.rcWork.bottom - menuH;
        if (screenX < mi.rcWork.left) screenX = mi.rcWork.left;
        if (screenY < mi.rcWork.top) screenY = mi.rcWork.top;

        // 3. 注册类（一次）并创建 popup 窗口
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = { 0 };
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.style = 0;
            wc.lpfnWndProc = &MenuWndProc;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = L"Codex-Quota-Bar_Menu";
            RegisterClassExW(&wc);
            registered = true;
        }

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"Codex-Quota-Bar_Menu", L"", WS_POPUP,
            screenX, screenY, menuW, menuH,
            owner, NULL, GetModuleHandleW(NULL), &st);
        if (!hwnd) return 0;

        ApplyPopupWindowEffects(hwnd);

        // Activation can cancel mouse capture. Finish it before tracking starts.
        ShowWindow(hwnd, SW_SHOW);
        if (GetActiveWindow() != hwnd) {
            DestroyWindow(hwnd);
            return 0;
        }
        SetCapture(hwnd);
        st.tracking = true;
        activeMenu = hwnd;
        UpdateWindow(hwnd);

        // 4. 模态循环
        MSG msg = {};
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (msg.message == WM_CQB_MENU_END && msg.hwnd == hwnd) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        st.tracking = false;
        activeMenu = nullptr;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);

        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
        }

        return st.result;
    }

} // namespace CodexQuotaBar
