#include "Graphics/Direct2DRenderer.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Layout.h"

#include <cmath>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cwchar>
#include <utility>

namespace CodexQuotaBar {

    // 标准栏 / 展开态的全部几何位置统一由 StandardLayout 定义。
    using namespace StandardLayout;

    namespace {
        // 子卡片用的紧凑本地时间：M月D日 时:分（秒级时间戳）
        std::wstring FormatCompactTime(int64_t timestampSec) {
            if (timestampSec <= 0) return L"--";
            const time_t raw = static_cast<time_t>(timestampSec);
            tm local = {};
            if (localtime_s(&local, &raw) != 0) return L"--";
            wchar_t buffer[32] = {};
            swprintf_s(buffer, L"%02d月%02d日 %02d:%02d",
                local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
            return buffer;
        }

    }

    Direct2DRenderer::Direct2DRenderer()
        : m_palette(ThemePalette::Light())
    {
    }

    Direct2DRenderer::~Direct2DRenderer() {
        DiscardDeviceResources();
        DiscardTextFormats();
    }

    HRESULT Direct2DRenderer::Initialize(HWND hwnd) {
        m_hwnd = hwnd;

        D2D1_FACTORY_OPTIONS options = {};
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, &m_pD2DFactory);
        if (FAILED(hr)) return hr;

        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        hr = m_pD2DFactory->CreateStrokeStyle(&strokeProps, nullptr, 0, &m_pRoundStrokeStyle);
        if (FAILED(hr)) return hr;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &m_pDWriteFactory);
        if (FAILED(hr)) return hr;

        CreateTextFormats();
        return S_OK;
    }

    void Direct2DRenderer::SetDpiScale(float dpiScale) {
        SetScales(dpiScale, 1.0f);
    }

    void Direct2DRenderer::SetScales(float monitorDpiScale, float userScale) {
        const float safeDpi = monitorDpiScale > 0.0f ? monitorDpiScale : 1.0f;
        const float safeUser = userScale > 0.0f ? userScale : 1.0f;
        const float combined = safeDpi * safeUser;
        if (std::fabs(m_monitorDpiScale - safeDpi) < 1e-6f &&
            std::fabs(m_userScale - safeUser) < 1e-6f &&
            std::fabs(m_dpiScale - combined) < 1e-6f) {
            return;
        }
        m_monitorDpiScale = safeDpi;
        m_userScale = safeUser;
        m_dpiScale = combined;
        DiscardDeviceResources();
        DiscardTextFormats();
        CreateTextFormats();
    }

    bool Direct2DRenderer::FontFamilyExists(const std::wstring& fontFamily) const {
        if (!m_pDWriteFactory || fontFamily.empty()) return false;
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(m_pDWriteFactory->GetSystemFontCollection(&collection)) || !collection) {
            return false;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(fontFamily.c_str(), &index, &exists)) && exists) {
            return true;
        }
        const UINT32 familyCount = collection->GetFontFamilyCount();
        for (UINT32 i = 0; i < familyCount; ++i) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;
            ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(family->GetFamilyNames(&names)) || !names) continue;
            const UINT32 nameCount = names->GetCount();
            for (UINT32 j = 0; j < nameCount; ++j) {
                UINT32 length = 0;
                if (FAILED(names->GetStringLength(j, &length)) || length == 0) continue;
                std::wstring localizedName;
                localizedName.resize(length + 1);
                if (SUCCEEDED(names->GetString(j, &localizedName[0], length + 1))) {
                    localizedName.resize(length);
                    if (_wcsicmp(fontFamily.c_str(), localizedName.c_str()) == 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool Direct2DRenderer::SetAppearance(
        const ThemePalette& palette,
        const std::wstring& fontFamily,
        int backgroundTransparency,
        std::wstring* validationError)
    {
        m_palette = palette;
        m_backgroundTransparency = std::clamp(backgroundTransparency, 0, 90);
        bool validFont = FontFamilyExists(fontFamily);
        if (validFont) {
            m_fontFamily = fontFamily;
        } else {
            m_fontFamily = FontFamilyExists(L"Microsoft YaHei UI")
                ? L"Microsoft YaHei UI" : L"Segoe UI";
            if (validationError) {
                *validationError = L"- 系统中未找到字体族 \"" + fontFamily +
                    L"\"，已回退为 \"" + m_fontFamily + L"\"";
            }
        }

        DiscardDeviceResources();
        DiscardTextFormats();
        CreateTextFormats();
        return validFont;
    }

    void Direct2DRenderer::DiscardTextFormats() {
        m_pFontRowTitle.Reset();
        m_pFontRowDetail.Reset();
        m_pFontRowValue.Reset();
        m_pFontRowValueBold.Reset();
        m_pFontMiniValue.Reset();
        m_pFontFooter.Reset();
        m_pFontStatsNum.Reset();
        m_pFontStatsLabel.Reset();
    }

    void Direct2DRenderer::CreateTextFormats() {
        if (!m_pDWriteFactory) return;

        const wchar_t* fontFamily = m_fontFamily.c_str();
        const float s = m_dpiScale;

        auto createSingleLineFormat = [&](
            DWRITE_FONT_WEIGHT weight,
            float baseSize,
            ComPtr<IDWriteTextFormat>& target)
        {
            if (SUCCEEDED(m_pDWriteFactory->CreateTextFormat(
                    fontFamily, NULL,
                    weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    baseSize * s, L"zh-cn", &target)) && target) {
                target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };

        // 标题：14px 半粗体（单行禁止折行，杜绝分数缩放下尾字跳行）
        createSingleLineFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, m_pFontRowTitle);
        // 每行重置时间与状态说明：12px 常规体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, m_pFontRowDetail);
        // 右侧数值 / 未返回状态（常规体备用）：16px
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 16.0f, m_pFontRowValue);
        // 右侧百分比数值：16px 粗体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f, m_pFontRowValueBold);
        // Both mini slots share a fixed number field sized for the narrowest (dual) layout.
        createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f, m_pFontMiniValue);
        const MiniBarMetrics miniMetrics = ScaledMiniBarMetrics(
            s, WindowOuterCornerRadius(m_monitorDpiScale));
        const float miniSlotWidth = static_cast<float>(Scale(static_cast<float>(MINI_SINGLE_WIDTH), s)) -
                                    2.0f * miniMetrics.contentInset - MiniDividerPhysicalWidth(miniMetrics) / 2.0f;
        const float miniAvailable = (std::max)(1.0f, miniSlotWidth - 1.0f);
        auto* miniFormat = m_pFontMiniValue.Get();
        float widestDigit = 0.0f;
        for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
            widestDigit = (std::max)(widestDigit, MeasureTextWidth(std::wstring(1, digit), miniFormat));
        }
        const float requiredWidth = (std::max)({
            MeasureTextWidth(L"100%", miniFormat),
            2.0f * widestDigit + MeasureTextWidth(L"%", miniFormat),
            MeasureTextWidth(L"---", miniFormat)
        });
        m_miniValueWidth = (std::min)(miniAvailable, std::ceil(requiredWidth));
        if (requiredWidth > miniAvailable) {
            ComPtr<IDWriteTextFormat> fitted;
            createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f * miniAvailable / requiredWidth, fitted);
            if (fitted) m_pFontMiniValue = std::move(fitted);
        }
        // 常驻卡片底部说明文本：12px 常规体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, m_pFontFooter);
        // 统计子卡片数值（加粗）：16px 粗体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f, m_pFontStatsNum);
        // 统计子卡片标签：12px 常规体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, m_pFontStatsLabel);
    }

    HRESULT Direct2DRenderer::CreateDeviceResources() {
        if (m_pRenderTarget) return S_OK;
        if (!m_pD2DFactory || !m_hwnd) return E_UNEXPECTED;

        if (m_width == 0 || m_height == 0) {
            RECT clientRect = {};
            if (!GetClientRect(m_hwnd, &clientRect)) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            m_width = static_cast<UINT>((std::max)(0L, clientRect.right - clientRect.left));
            m_height = static_cast<UINT>((std::max)(0L, clientRect.bottom - clientRect.top));
        }
        if (m_width == 0 || m_height == 0) return E_INVALIDARG;

        HRESULT hr = CreateBackingSurface(m_width, m_height);
        if (FAILED(hr)) return hr;

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        hr = m_pD2DFactory->CreateDCRenderTarget(&props, &m_pRenderTarget);

        if (FAILED(hr)) {
            DiscardDeviceResources();
            return hr;
        }

        RECT bindRect = {
            0, 0,
            static_cast<LONG>(m_width),
            static_cast<LONG>(m_height)
        };
        hr = m_pRenderTarget->BindDC(m_memoryDc, &bindRect);
        if (FAILED(hr)) {
            DiscardDeviceResources();
            return hr;
        }

        m_pRenderTarget->SetDpi(96.0f, 96.0f);
        m_pRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        const float backgroundOpacity =
            1.0f - static_cast<float>(m_backgroundTransparency) / 100.0f;
        D2D1_COLOR_F surface = m_palette.surface;
        surface.a *= backgroundOpacity;
        D2D1_COLOR_F statsCardBackground = m_palette.statsCardBg;
        statsCardBackground.a *= backgroundOpacity;
        D2D1_COLOR_F trackBg = m_palette.trackBg;
        trackBg.a *= backgroundOpacity;

        // 创建画刷：任一失败即整体回滚
        struct BrushSpec {
            D2D1_COLOR_F color;
            ID2D1SolidColorBrush** slot;
        };
        const BrushSpec brushSpecs[] = {
            { surface, &m_pBrushSurface },
            { statsCardBackground, &m_pBrushStatsCardBg },
            { m_palette.statsCardBorder, &m_pBrushStatsCardBorder },
            { m_palette.text, &m_pBrushText },
            { m_palette.muted, &m_pBrushMuted },
            { trackBg, &m_pBrushTrackBg },
            { m_palette.progressBlue, &m_pBrushProgressBlue },
            { m_palette.progressGreen, &m_pBrushProgressGreen },
            { m_palette.progressYellow, &m_pBrushProgressYellow },
            { m_palette.progressOrange, &m_pBrushProgressOrange },
            { m_palette.progressRed, &m_pBrushProgressRed },
            { m_palette.border, &m_pBrushBorder },
            { m_palette.divider, &m_pBrushDivider },
            { m_palette.chevron, &m_pBrushChevron },
            { m_palette.syncSuccess, &m_pBrushSyncSuccess },
            { m_palette.syncIdle, &m_pBrushSyncIdle },
            { m_palette.syncBusy, &m_pBrushSyncBusy },
            { D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &m_pBrushHalo },
        };

        for (const BrushSpec& spec : brushSpecs) {
            hr = m_pRenderTarget->CreateSolidColorBrush(spec.color, spec.slot);
            if (FAILED(hr)) {
                DiscardDeviceResources();
                return hr;
            }
        }

        return S_OK;
    }

    void Direct2DRenderer::DiscardDeviceResources() {
        m_pBrushSurface.Reset();
        m_pBrushStatsCardBg.Reset();
        m_pBrushStatsCardBorder.Reset();
        m_pBrushText.Reset();
        m_pBrushMuted.Reset();
        m_pBrushTrackBg.Reset();
        m_pBrushProgressBlue.Reset();
        m_pBrushProgressGreen.Reset();
        m_pBrushProgressYellow.Reset();
        m_pBrushProgressOrange.Reset();
        m_pBrushProgressRed.Reset();
        m_pBrushBorder.Reset();
        m_pBrushDivider.Reset();
        m_pBrushChevron.Reset();
        m_pBrushSyncSuccess.Reset();
        m_pBrushSyncIdle.Reset();
        m_pBrushSyncBusy.Reset();
        m_pBrushHalo.Reset();
        m_pRenderTarget.Reset();
        DiscardBackingSurface();
    }

    HRESULT Direct2DRenderer::CreateBackingSurface(UINT width, UINT height) {
        DiscardBackingSurface();

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) return HRESULT_FROM_WIN32(GetLastError());

        m_memoryDc = CreateCompatibleDC(screenDc);
        if (!m_memoryDc) {
            const DWORD error = GetLastError();
            ReleaseDC(nullptr, screenDc);
            return HRESULT_FROM_WIN32(error);
        }

        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        m_dibBitmap = CreateDIBSection(
            screenDc,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &m_dibBits,
            nullptr,
            0);
        const DWORD createError = GetLastError();
        ReleaseDC(nullptr, screenDc);
        if (!m_dibBitmap || !m_dibBits) {
            DiscardBackingSurface();
            return HRESULT_FROM_WIN32(createError == ERROR_SUCCESS ? ERROR_NOT_ENOUGH_MEMORY : createError);
        }

        m_previousBitmap = SelectObject(m_memoryDc, m_dibBitmap);
        if (!m_previousBitmap || m_previousBitmap == HGDI_ERROR) {
            const DWORD selectError = GetLastError();
            m_previousBitmap = nullptr;
            DiscardBackingSurface();
            return HRESULT_FROM_WIN32(selectError == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : selectError);
        }
        return S_OK;
    }

    void Direct2DRenderer::DiscardBackingSurface() {
        if (m_memoryDc && m_previousBitmap) {
            SelectObject(m_memoryDc, m_previousBitmap);
        }
        m_previousBitmap = nullptr;
        if (m_dibBitmap) DeleteObject(m_dibBitmap);
        m_dibBitmap = nullptr;
        m_dibBits = nullptr;
        if (m_memoryDc) DeleteDC(m_memoryDc);
        m_memoryDc = nullptr;
    }

    HRESULT Direct2DRenderer::PresentLayeredWindow() {
        POINT source = { 0, 0 };
        SIZE size = { static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC screenDc = GetDC(nullptr);
        if (!screenDc) return HRESULT_FROM_WIN32(GetLastError());
        const BOOL presented = UpdateLayeredWindow(
            m_hwnd,
            screenDc,
            nullptr, // The window layout owns the physical screen position.
            &size,
            m_memoryDc,
            &source,
            0,
            &blend,
            ULW_ALPHA);
        const DWORD error = GetLastError();
        ReleaseDC(nullptr, screenDc);
        return presented
            ? S_OK
            : HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }

    HRESULT Direct2DRenderer::Resize(UINT width, UINT height) {
        if (m_width == width && m_height == height) return S_OK;
        m_width = width;
        m_height = height;
        DiscardDeviceResources();
        return S_OK;
    }

    HRESULT Direct2DRenderer::Render(
        bool expanded,
        bool miniMode,
        const QuotaSnapshot& snapshot,
        SyncState syncState)
    {
        HRESULT hr = CreateDeviceResources();
        if (FAILED(hr)) return hr;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->SetDpi(96.0f, 96.0f);
        m_pRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        float w = static_cast<float>(m_width);
        float h = static_cast<float>(m_height);

        // 1. 绘制带逐像素 Alpha 的圆角悬浮卡片背景：全应用外圆角始终基于基准 8px 与 DWM 窗口硬件圆角同步。
        const bool miniVisible = miniMode && !expanded;
        const float outerRadius = WindowOuterCornerRadius(m_monitorDpiScale);
        const MiniBarMetrics miniMetrics = ScaledMiniBarMetrics(m_dpiScale, outerRadius);
        const D2D1_ROUNDED_RECT background = D2D1::RoundedRect(
            D2D1::RectF(0.0f, 0.0f, w, h), outerRadius, outerRadius);
        m_pRenderTarget->FillRoundedRectangle(background, m_pBrushSurface.Get());

        // 2. 迷你态只呈现额度与缩短的进度条；标准展开态追加详情卡片。
        const float hairline = miniMetrics.outerBorderThickness;
        if (miniVisible) {
            DrawMiniBar(snapshot, miniMetrics);
        } else {
            const float collapsedH = Px(static_cast<float>(COLLAPSED_HEIGHT));
            DrawCollapsedBar(0.0f, expanded, snapshot, syncState);
            if (expanded) {
                const float padX = Px(kPadX);
                // 分隔线以整像素矩形填充，避免 1px 线条跨两个像素发虚。
                m_pRenderTarget->FillRectangle(
                    D2D1::RectF(padX, collapsedH, w - padX, collapsedH + hairline),
                    m_pBrushDivider.Get());
                const float cardH = Px(kCardHeight);
                const float statsTop = collapsedH + Px(kCardGapTop);
                DrawStatsSubCard(statsTop, cardH, snapshot.stats);
                DrawResetSubCard(statsTop + cardH + Px(kCardGap), cardH, snapshot);
            }
        }

        // 3. 外描边保留在逐像素 Alpha 画面内，确保 OuterBorder 个性配置生效。
        //    使用偶数像素描边宽度（>= 2px），内缩整像素，消除 0.5px 亚像素模糊。
        const float strokeWidth = (std::max)(2.0f, static_cast<float>(SnapEven(Scale(hairline, 1.0f))));
        const float strokeInset = strokeWidth * 0.5f;
        const float strokeRadius = (std::max)(0.0f, outerRadius - strokeInset);
        const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(
            D2D1::RectF(
                strokeInset, strokeInset,
                w - strokeInset, h - strokeInset),
            strokeRadius, strokeRadius);
        m_pRenderTarget->DrawRoundedRectangle(
            outer, m_pBrushBorder.Get(), strokeWidth, m_pRoundStrokeStyle.Get());

        hr = m_pRenderTarget->EndDraw();
        if (FAILED(hr)) {
            DiscardDeviceResources();
        } else {
            hr = PresentLayeredWindow();
            if (FAILED(hr)) {
                DiscardDeviceResources();
            }
        }
        return hr;
    }

    void Direct2DRenderer::DrawMiniBar(
        const QuotaSnapshot& snapshot,
        const MiniBarMetrics& metrics)
    {
        QuotaWindow unavailable;
        const QuotaWindow* windows[2] = {};
        int count = 0;
        if (snapshot.window.available) windows[count++] = &snapshot.window;
        if (snapshot.weekly.available) windows[count++] = &snapshot.weekly;
        if (count == 0) windows[count++] = &unavailable;

        const float w = static_cast<float>(m_width);
        const float h = static_cast<float>(m_height);
        const float pad = metrics.contentInset;
        const float pillTop = pad;
        const float pillBottom = h - pad;
        const float pillRadius = metrics.pillCornerRadius;

        if (count == 1) {
            const float pillLeft = pad;
            const float pillRight = w - pad;
            DrawMiniQuotaSlot(pillLeft, pillTop, pillRight, pillBottom, pillRadius, *windows[0]);
        } else if (count == 2) {
            const float centerX = std::round(w * 0.5f);
            const float divThick = MiniDividerPhysicalWidth(metrics);
            const float divHalf = divThick * 0.5f;
            const float divLeft = centerX - divHalf;
            const float divRight = centerX + divHalf;

            const float pill0Left = pad;
            const float pill0Right = divLeft - pad;
            const float pill1Left = divRight + pad;
            const float pill1Right = w - pad;

            DrawMiniQuotaSlot(pill0Left, pillTop, pill0Right, pillBottom, pillRadius, *windows[0]);
            DrawMiniQuotaSlot(pill1Left, pillTop, pill1Right, pillBottom, pillRadius, *windows[1]);

            int divHPx = static_cast<int>(std::round(MiniDividerHeight(static_cast<int>(m_height), metrics)));
            if (divHPx % 2 != 0) divHPx -= 1;
            if (divHPx < 2) divHPx = 2;
            const float dividerHeight = static_cast<float>(divHPx);
            const float dividerTop = std::round((h - dividerHeight) * 0.5f);
            const float dividerBottom = dividerTop + dividerHeight;
            const float divCorner = divHalf;
            const D2D1_ROUNDED_RECT divRect = D2D1::RoundedRect(
                D2D1::RectF(divLeft, dividerTop, divRight, dividerBottom),
                divCorner, divCorner);
            m_pRenderTarget->FillRoundedRectangle(divRect, m_pBrushDivider.Get());
        }
    }

    void Direct2DRenderer::DrawMiniQuotaSlot(
        float pillLeft,
        float pillTop,
        float pillRight,
        float pillBottom,
        float pillRadius,
        const QuotaWindow& window)
    {
        const std::wstring value = window.available
            ? std::to_wstring(static_cast<int>(std::round(window.remainingPercent))) + L"%"
            : L"---";

        const float pillW = (std::max)(0.0f, pillRight - pillLeft);

        // 1. 胶囊槽底色（接入已有 TrackBackground 自定义外观与透明度逻辑）
        const D2D1_ROUNDED_RECT pillRect = D2D1::RoundedRect(
            D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom),
            pillRadius, pillRadius);
        m_pRenderTarget->FillRoundedRectangle(pillRect, m_pBrushTrackBg.Get());

        // 2. 拓宽内部进度条（接入已有 ProgressBrushFor 色彩梯度与透明度继承）
        if (window.available && window.remainingPercent > 0.0) {
            float fillW = (std::max)(pillRadius * 2.0f, pillW * static_cast<float>(window.remainingPercent / 100.0));
            fillW = (std::min)(fillW, pillW);
            fillW = std::round(fillW);
            ID2D1SolidColorBrush* statusBrush = ProgressBrushFor(window.remainingPercent);
            if (statusBrush && m_pBrushHalo) {
                const float backgroundOpacity =
                    1.0f - static_cast<float>(m_backgroundTransparency) / 100.0f;
                D2D1_COLOR_F tintColor = statusBrush->GetColor();
                tintColor.a = 0.32f * backgroundOpacity;
                m_pBrushHalo->SetColor(tintColor);

                m_pRenderTarget->PushAxisAlignedClip(
                    D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                const D2D1_ROUNDED_RECT fillRect = D2D1::RoundedRect(
                    D2D1::RectF(pillLeft, pillTop, pillLeft + fillW, pillBottom),
                    pillRadius, pillRadius);
                m_pRenderTarget->FillRoundedRectangle(fillRect, m_pBrushHalo.Get());
                m_pRenderTarget->PopAxisAlignedClip();
            }
        }

        // 3. 百分比文字（动态绑定实际胶囊槽物理高度与基线，杜绝缩放时行高溢出导致的错位）
        if (m_pFontMiniValue) {
            const float pillH = pillBottom - pillTop;
            const float baseline = std::round(pillH * 0.75f);
            m_pFontMiniValue->SetLineSpacing(
                DWRITE_LINE_SPACING_METHOD_UNIFORM,
                pillH,
                baseline);
            m_pFontMiniValue->SetTextAlignment(window.available
                ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pFontMiniValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float textRight = window.available
                ? (std::min)(pillRight, std::round((pillLeft + pillRight + m_miniValueWidth) / 2.0f))
                : pillRight;
            const D2D1_RECT_F textRect = D2D1::RectF(pillLeft, pillTop, textRight, pillBottom);
            ID2D1SolidColorBrush* brush = window.available
                ? ProgressBrushFor(window.remainingPercent)
                : m_pBrushProgressBlue.Get();
            m_pRenderTarget->PushAxisAlignedClip(pillRect.rect, D2D1_ANTIALIAS_MODE_ALIASED);
            m_pRenderTarget->DrawTextW(
                value.c_str(), static_cast<UINT32>(value.size()),
                m_pFontMiniValue.Get(), textRect, brush);
            m_pRenderTarget->PopAxisAlignedClip();
        }
    }

    float Direct2DRenderer::Px(float logicalPx) const {
        return SnapPx(logicalPx, m_dpiScale);
    }

    void Direct2DRenderer::DrawSyncIndicator(float centerY, SyncState state) {
        const float cx = Px(kSyncCenterX);
        const float cy = std::round(centerY);
        const float coreRadius = static_cast<float>(ScaleEven(4.0f, m_dpiScale));
        const float haloRadius = static_cast<float>(ScaleEven(6.0f, m_dpiScale));

        ID2D1SolidColorBrush* coreBrush = m_pBrushSyncIdle.Get();
        bool hasHalo = true;

        D2D1_COLOR_F haloColor;
        switch (state) {
        case SyncState::Syncing:
            coreBrush = m_pBrushSyncBusy.Get();
            haloColor = m_palette.syncBusy;
            haloColor.a = 0.28f;
            break;
        case SyncState::Synced:
            coreBrush = m_pBrushSyncSuccess.Get();
            haloColor = m_palette.syncSuccess;
            haloColor.a = 0.28f;
            break;
        case SyncState::Failed:
            coreBrush = m_pBrushProgressRed.Get();
            haloColor = m_palette.progressRed;
            haloColor.a = 0.28f;
            break;
        case SyncState::Waiting:
        default:
            coreBrush = m_pBrushSyncIdle.Get();
            hasHalo = false;
            break;
        }

        if (hasHalo) {
            m_pBrushHalo->SetColor(haloColor);
            m_pRenderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(cx, cy), haloRadius, haloRadius),
                m_pBrushHalo.Get());
        }
        m_pRenderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), coreRadius, coreRadius),
            coreBrush);
    }

    void Direct2DRenderer::DrawCollapsedBar(
        float topY,
        bool expanded,
        const QuotaSnapshot& snapshot,
        SyncState syncState)
    {
        auto quotaDetail = [](const QuotaWindow& w) -> std::wstring {
            if (!w.available || w.resetTimeString.empty()) {
                return L"官方当前未返回";
            }
            return L"重置 " + w.resetTimeString;
        };

        DrawQuotaRow(
            topY + Px(kRowTops[0]),
            L"窗口使用限额", quotaDetail(snapshot.window), snapshot.window);

        DrawQuotaRow(
            topY + Px(kRowTops[1]),
            L"每周使用限额", quotaDetail(snapshot.weekly), snapshot.weekly);

        const wchar_t* syncDetail = L"等待";
        switch (syncState) {
        case SyncState::Syncing:
            syncDetail = L"同步中";
            break;
        case SyncState::Synced:
            syncDetail = L"成功";
            break;
        case SyncState::Failed:
            syncDetail = L"失败";
            break;
        case SyncState::Waiting:
        default:
            syncDetail = L"等待";
            break;
        }

        // 底部状态行：标题、状态值、同步点与展开箭头共用同一取整后的中线。
        const float footerTop = topY + Px(kFooterTop);
        const float footerBottom = footerTop + Px(kFooterHeight);
        const float footerCenterY = (footerTop + footerBottom) / 2.0f;

        if (m_pFontFooter) {
            const std::wstring footerTitle = L"同步服务状态";
            const float titleWidth = MeasureTextWidth(footerTitle, m_pFontFooter.Get());
            const float padX = Px(kPadX);

            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontFooter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F footerTitleRect = D2D1::RectF(
                padX, footerTop,
                padX + titleWidth + ScaleF(8.0f, m_dpiScale),
                footerBottom);
            m_pRenderTarget->DrawTextW(
                footerTitle.c_str(), static_cast<UINT32>(footerTitle.size()),
                m_pFontFooter.Get(), footerTitleRect, m_pBrushMuted.Get());

            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D1_RECT_F syncDetailRect = D2D1::RectF(
                Px(kDetailLeft), footerTop, Px(kDetailRight), footerBottom);
            m_pRenderTarget->DrawTextW(
                syncDetail, static_cast<UINT32>(wcslen(syncDetail)),
                m_pFontFooter.Get(), syncDetailRect, m_pBrushMuted.Get());
        }

        DrawSyncIndicator(footerCenterY, syncState);
        DrawChevron(footerCenterY, expanded);
    }

    void Direct2DRenderer::DrawChevron(float centerY, bool expanded) {
        const float cx = Px(kChevronCenterX);
        const float cy = std::round(centerY);
        const float halfW = static_cast<float>(ScaleEven(6.0f, m_dpiScale));
        const float halfH = static_cast<float>(ScaleEven(4.0f, m_dpiScale));
        const float strokeW = static_cast<float>(ScaleEven(2.0f, m_dpiScale));

        const bool pointsUp = expanded;

        D2D1_POINT_2F p1, p2, p3;
        if (pointsUp) {
            p1 = D2D1::Point2F(cx - halfW, cy + halfH);
            p2 = D2D1::Point2F(cx, cy - halfH);
            p3 = D2D1::Point2F(cx + halfW, cy + halfH);
        } else {
            p1 = D2D1::Point2F(cx - halfW, cy - halfH);
            p2 = D2D1::Point2F(cx, cy + halfH);
            p3 = D2D1::Point2F(cx + halfW, cy - halfH);
        }

        m_pRenderTarget->DrawLine(
            p1, p2, m_pBrushChevron.Get(), strokeW, m_pRoundStrokeStyle.Get());
        m_pRenderTarget->DrawLine(
            p2, p3, m_pBrushChevron.Get(), strokeW, m_pRoundStrokeStyle.Get());
    }

    ID2D1SolidColorBrush* Direct2DRenderer::ProgressBrushFor(double remainingPercent) const {
        if (remainingPercent <= 20.0) return m_pBrushProgressRed.Get();
        if (remainingPercent <= 35.0) return m_pBrushProgressOrange.Get();
        if (remainingPercent <= 50.0) return m_pBrushProgressYellow.Get();
        return m_pBrushProgressGreen.Get();
    }

    float Direct2DRenderer::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) const {
        if (!m_pDWriteFactory || !format || text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_pDWriteFactory->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), format,
                10000.0f, 100.0f, &layout))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics = {};
        layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace;
    }

    void Direct2DRenderer::DrawQuotaRow(
        float topY,
        const std::wstring& title,
        const std::wstring& detail,
        const QuotaWindow& window)
    {
        const float padX = Px(kPadX);
        const float w = static_cast<float>(m_width);
        const float rightX = w - padX;
        const float rowBottom = topY + Px(kRowTextHeight);

        std::wstring numStr;
        if (window.available) {
            const int pct = static_cast<int>(std::round(window.remainingPercent));
            numStr = std::to_wstring(pct) + L"%";
        } else {
            numStr = L"---";
        }
        const float pad8 = static_cast<float>(ScaleEven(8.0f, m_dpiScale));
        const float numWidth = MeasureTextWidth(numStr, m_pFontRowValueBold.Get());
        const float numLeft = std::round(rightX - numWidth - pad8);

        const float titleWidth = MeasureTextWidth(title, m_pFontRowTitle.Get());

        // 1. 标题（与详情、数值共用同一取整后的行框，保证垂直中线一致）
        if (m_pFontRowTitle) {
            m_pFontRowTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontRowTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F titleRect = D2D1::RectF(padX, topY, std::round(padX + titleWidth + pad8), rowBottom);
            m_pRenderTarget->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), m_pFontRowTitle.Get(), titleRect, m_pBrushText.Get());
        }

        // 每行中部辅助信息（使用 12px 字体与居中对齐）
        if (m_pFontRowDetail && !detail.empty()) {
            m_pFontRowDetail->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pFontRowDetail->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F detailRect = D2D1::RectF(
                Px(kDetailLeft), topY, Px(kDetailRight), rowBottom);
            m_pRenderTarget->DrawTextW(
                detail.c_str(), static_cast<UINT32>(detail.size()),
                m_pFontRowDetail.Get(), detailRect, m_pBrushMuted.Get());
        }

        // 2. 右侧数值 / 状态
        IDWriteTextFormat* numFormat = m_pFontRowValueBold.Get();
        if (numFormat) {
            numFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            numFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F valRect = D2D1::RectF(numLeft, topY, rightX, rowBottom);
            ID2D1SolidColorBrush* brush = window.available
                ? ProgressBrushFor(window.remainingPercent)
                : m_pBrushProgressBlue.Get();
            m_pRenderTarget->DrawTextW(numStr.c_str(), static_cast<UINT32>(numStr.size()), numFormat, valRect, brush);
        }

        // 3. 满宽圆角细胶囊进度条（起点与高度均为整像素，两行完全一致）
        const float trackX = padX;
        const float trackW = w - padX * 2.0f;
        const float trackY = topY + Px(kTrackOffset);
        DrawProgressTrack(trackX, trackY, trackW, window);
    }

    void Direct2DRenderer::DrawProgressTrack(
        float trackX,
        float trackY,
        float trackW,
        const QuotaWindow& window,
        float customHeight)
    {
        float trackH = customHeight > 0.0f
            ? customHeight
            : (std::max)(4.0f, Px(kTrackHeight));
        int trackHPx = static_cast<int>(std::round(trackH));
        if (trackHPx % 2 != 0) trackHPx += 1;
        trackH = static_cast<float>(trackHPx);
        float trackRadius = trackH * 0.5f;

        D2D1_ROUNDED_RECT trackBgRect = D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH), trackRadius, trackRadius);
        m_pRenderTarget->FillRoundedRectangle(trackBgRect, m_pBrushTrackBg.Get());

        if (window.available && window.remainingPercent > 0.0) {
            float fillW = std::max(trackH, trackW * static_cast<float>(window.remainingPercent / 100.0));
            fillW = std::min(fillW, trackW);
            fillW = std::round(fillW);
            D2D1_ROUNDED_RECT fillRect = D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + fillW, trackY + trackH), trackRadius, trackRadius);
            m_pRenderTarget->FillRoundedRectangle(fillRect, ProgressBrushFor(window.remainingPercent));
        }
    }

    void Direct2DRenderer::DrawStatsSubCard(float topY, float height, const TokenStats& stats) {
        const float padX = Px(kPadX);
        const float w = static_cast<float>(m_width);
        const float cardW = w - padX * 2.0f;
        const float cardCorner = Px(6.0f);

        DrawSubCardFrame(padX, topY, cardW, height, cardCorner);

        struct ColData {
            const std::wstring& val;
            const wchar_t* label;
        } cols[] = {
            { stats.totalTokens, L"累计tokens" },
            { stats.peakTokens, L"峰值tokens" },
            { stats.longestTask, L"最长聊天" }
        };

        for (int i = 0; i < 3; ++i) {
            const float colLeft = padX + std::round(static_cast<float>(i) * cardW / 3.0f);
            const float colRight = (i == 2)
                ? (padX + cardW)
                : (padX + std::round(static_cast<float>(i + 1) * cardW / 3.0f));
            const std::wstring& val = cols[i].val;
            const wchar_t* numText = val.empty() ? L"---" : val.c_str();
            UINT32 numLen = val.empty() ? 3 : static_cast<UINT32>(val.size());

            if (m_pFontStatsNum) {
                m_pFontStatsNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colLeft, topY + Px(8.0f), colRight, topY + Px(32.0f));
                ID2D1SolidColorBrush* numBrush = val.empty() ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_pRenderTarget->DrawTextW(numText, numLen, m_pFontStatsNum.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colLeft, topY + Px(36.0f), colRight, topY + Px(58.0f));
                m_pRenderTarget->DrawTextW(cols[i].label, static_cast<UINT32>(wcslen(cols[i].label)), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
            }
        }
    }

    void Direct2DRenderer::DrawSubCardFrame(
        float left, float top, float width, float height, float corner)
    {
        const D2D1_ROUNDED_RECT fillRect = D2D1::RoundedRect(
            D2D1::RectF(left, top, left + width, top + height), corner, corner);
        m_pRenderTarget->FillRoundedRectangle(fillRect, m_pBrushStatsCardBg.Get());
        // 细描边按偶数像素宽度（>= 2px）内缩整像素，在各 DPI 下均精确落在物理像素格上，避免 0.5px 亚像素模糊。
        const float strokeWidth = (std::max)(2.0f, static_cast<float>(SnapEven(Scale(1.0f, m_dpiScale))));
        const float inset = strokeWidth * 0.5f;
        const D2D1_ROUNDED_RECT strokeRect = D2D1::RoundedRect(
            D2D1::RectF(left + inset, top + inset, left + width - inset, top + height - inset),
            (std::max)(0.0f, corner - inset), (std::max)(0.0f, corner - inset));
        m_pRenderTarget->DrawRoundedRectangle(
            strokeRect, m_pBrushStatsCardBorder.Get(), strokeWidth, m_pRoundStrokeStyle.Get());
    }

    void Direct2DRenderer::DrawResetSubCard(float topY, float height, const QuotaSnapshot& snapshot) {
        const float padX = Px(kPadX);
        const float w = static_cast<float>(m_width);
        const float cardW = w - padX * 2.0f;
        const float cardCorner = Px(6.0f);

        DrawSubCardFrame(padX, topY, cardW, height, cardCorner);

        const int64_t expiry = snapshot.resetCredits.earliestExpiresAt;
        const std::wstring expiryText = expiry > 0 ? FormatCompactTime(expiry) : L"--";

        std::wstring remainingDaysText = L"--";
        if (expiry > 0) {
            const int64_t nowSec = static_cast<int64_t>(time(nullptr));
            const int64_t diff = expiry - nowSec;
            const long long days = diff > 0 ? (diff + 86399) / 86400 : 0;
            remainingDaysText = std::to_wstring(days) + L" 天";
        }

        const std::wstring countText = snapshot.resetCredits.countAvailable
            ? std::to_wstring(snapshot.resetCredits.availableCount) + L" 次"
            : L"--";

        const wchar_t* labels[] = { L"重置次数", L"过期时间", L"剩余天数" };
        const std::wstring* values[] = { &countText, &expiryText, &remainingDaysText };

        for (int i = 0; i < 3; ++i) {
            const float colLeft = padX + std::round(static_cast<float>(i) * cardW / 3.0f);
            const float colRight = (i == 2)
                ? (padX + cardW)
                : (padX + std::round(static_cast<float>(i + 1) * cardW / 3.0f));

            if (m_pFontStatsNum) {
                m_pFontStatsNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colLeft, topY + Px(8.0f), colRight, topY + Px(32.0f));
                ID2D1SolidColorBrush* numBrush =
                    (*values[i] == L"--") ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_pRenderTarget->DrawTextW(values[i]->c_str(), static_cast<UINT32>(values[i]->size()), m_pFontStatsNum.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colLeft, topY + Px(36.0f), colRight, topY + Px(58.0f));
                m_pRenderTarget->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
            }
        }
    }

} // namespace CodexQuotaBar
