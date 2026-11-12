#include "Graphics/Direct2DRenderer.h"
#include "Graphics/PixelText.h"
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
        int64_t RemainingCreditDays(const QuotaSnapshot& snapshot) {
            const int64_t expiry = snapshot.resetCredits.earliestExpiresAt;
            if (expiry <= 0) return -1;
            const int64_t diff = expiry - static_cast<int64_t>(time(nullptr));
            return diff > 0 ? diff / 86400 + (diff % 86400 != 0) : 0;
        }

        size_t ProgressIndex(double remainingPercent) {
            if (remainingPercent <= 20.0) return 3;
            if (remainingPercent <= 35.0) return 2;
            if (remainingPercent <= 50.0) return 1;
            return 0;
        }
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
        m_textCache.Clear();
        for (auto& cached : m_miniTextLayouts) cached = {};
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
                    static_cast<float>(ScaleEven(baseSize, s)), L"zh-cn", &target)) && target) {
                target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };

        // 标题：14px 半粗体（单行禁止折行，杜绝分数缩放下尾字跳行）
        createSingleLineFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, m_pFontRowTitle);
        // 每行重置时间与状态说明：12px 常规体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, m_pFontRowDetail);
        // 详情卡片的时间与数值：14px 中等字重
        createSingleLineFormat(DWRITE_FONT_WEIGHT_MEDIUM, 14.0f, m_pFontRowValue);
        // 右侧百分比数值：20px 粗体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 20.0f, m_pFontRowValueBold);
        // Fit the longest percentage in the narrower dual-slot layout.
        createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f, m_pFontMiniValue);
        const MiniBarMetrics miniMetrics = ScaledMiniBarMetrics(
            s, WindowOuterCornerRadius(m_monitorDpiScale));
        const float dualBarWidth = static_cast<float>(MiniDualPhysicalWidth(s));
        const float miniAvailable = (std::max)(1.0f, MiniDualSlotWidth(dualBarWidth, miniMetrics) - 1.0f);
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
        if (requiredWidth > miniAvailable) {
            ComPtr<IDWriteTextFormat> fitted;
            const float fittedSize = (std::max)(2.0f, FloorEvenPixel(
                m_pFontMiniValue->GetFontSize() * miniAvailable / requiredWidth));
            createSingleLineFormat(DWRITE_FONT_WEIGHT_BOLD, fittedSize / s, fitted);
            if (fitted) m_pFontMiniValue = std::move(fitted);
        }
        if (m_pFontMiniValue) {
            m_pFontMiniValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pFontMiniValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // 常驻卡片底部说明文本：12px 常规体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, m_pFontFooter);
        // 统计子卡片数值：16px 半粗体
        createSingleLineFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, 16.0f, m_pFontStatsNum);
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

        for (size_t index = 0; index < m_pBrushProgressText.size(); ++index) {
            hr = m_pRenderTarget->CreateSolidColorBrush(
                m_palette.progressText[index], m_pBrushProgressText[index].GetAddressOf());
            if (FAILED(hr)) {
                DiscardDeviceResources();
                return hr;
            }
        }

        return S_OK;
    }

    void Direct2DRenderer::DiscardDeviceResources() {
        m_frameValid = false;
        for (auto& brush : m_pBrushProgressText) brush.Reset();
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
        SyncState syncState,
        bool forcePresent)
    {
        HRESULT hr = CreateDeviceResources();
        if (FAILED(hr)) return hr;

        if (HasSameContent(expanded, miniMode, snapshot, syncState)) {
            if (!forcePresent) return S_OK;
            hr = PresentLayeredWindow();
            if (FAILED(hr)) DiscardDeviceResources();
            return hr;
        }

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
        if (SUCCEEDED(hr)) {
            m_frameValid = true;
            m_lastExpanded = expanded;
            m_lastMini = miniMode;
            m_lastSnapshot = snapshot;
            m_lastSyncState = syncState;
            m_lastRemainingDays = RemainingCreditDays(snapshot);
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
            const float divLeft = pad + MiniDualSlotWidth(w, metrics);
            const float divRight = w - divLeft;

            DrawMiniQuotaSlot(pad, pillTop, divLeft, pillBottom, pillRadius, *windows[0]);
            DrawMiniQuotaSlot(divRight, pillTop, w - pad, pillBottom, pillRadius, *windows[1]);
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
        const size_t valueIndex = window.available
            ? static_cast<size_t>(std::round(std::clamp(window.remainingPercent, 0.0, 100.0)))
            : 101;
        const std::wstring value = window.available
            ? std::to_wstring(valueIndex) + L"%"
            : L"---";

        // 1. 胶囊槽底色（接入已有 TrackBackground 自定义外观与透明度逻辑）
        const D2D1_ROUNDED_RECT pillRect = D2D1::RoundedRect(
            D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom),
            pillRadius, pillRadius);
        m_pRenderTarget->FillRoundedRectangle(pillRect, m_pBrushTrackBg.Get());

        // 2. 裁剪完整圆角槽，让右端弧线随进度缩短自然过渡到直边。
        const float pillWidth = (std::max)(0.0f, pillRight - pillLeft);
        const float fillW = window.available
            ? RoundEvenPixel(pillWidth *
                static_cast<float>(std::clamp(window.remainingPercent, 0.0, 100.0) / 100.0))
            : 0.0f;
        if (fillW > 0.0f && pillBottom > pillTop) {
            ID2D1SolidColorBrush* statusBrush = ProgressBrushFor(window.remainingPercent);
            if (statusBrush && m_pBrushHalo) {
                const float backgroundOpacity =
                    1.0f - static_cast<float>(m_backgroundTransparency) / 100.0f;
                D2D1_COLOR_F tintColor = m_palette.miniFill[ProgressIndex(window.remainingPercent)];
                tintColor.a *= backgroundOpacity;
                m_pBrushHalo->SetColor(tintColor);

                const D2D1_RECT_F fillClip = D2D1::RectF(
                    pillLeft, pillTop, pillLeft + fillW, pillBottom);
                // 低额度时恢复小圆角，仍以实际比例限制宽度。
                const float lowFillRadius = (std::max)(0.0f,
                    FloorEvenPixel((std::min)(2.0f * pillRadius - fillW, fillW * 0.5f)));
                ComPtr<ID2D1RoundedRectangleGeometry> lowFillMask;
                if (lowFillRadius > 0.0f) {
                    m_pD2DFactory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(fillClip, lowFillRadius, lowFillRadius),
                        lowFillMask.GetAddressOf());
                }
                m_pRenderTarget->PushAxisAlignedClip(fillClip, D2D1_ANTIALIAS_MODE_ALIASED);
                if (lowFillMask) {
                    m_pRenderTarget->PushLayer(D2D1::LayerParameters(fillClip, lowFillMask.Get()), nullptr);
                }
                m_pRenderTarget->FillRoundedRectangle(pillRect, m_pBrushHalo.Get());
                if (lowFillMask) m_pRenderTarget->PopLayer();
                m_pRenderTarget->PopAxisAlignedClip();
            }
        }

        // 3. Center the visible glyphs, not the font's ascent/descent whitespace.
        // Layouts survive quota refreshes; font or DPI changes clear the cache.
        if (m_pFontMiniValue) {
            auto& cached = m_miniTextLayouts[valueIndex];
            if (!cached.layout) {
                constexpr float extent = 1024.0f;
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_pDWriteFactory->CreateTextLayout(
                        value.c_str(), static_cast<UINT32>(value.size()),
                        m_pFontMiniValue.Get(), extent, extent, layout.GetAddressOf()))) {
                    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    DWRITE_OVERHANG_METRICS ink = {};
                    if (SUCCEEDED(layout->GetOverhangMetrics(&ink))) {
                        cached.inkCenter = D2D1::Point2F(
                            (extent + ink.right - ink.left) * 0.5f,
                            (extent + ink.bottom - ink.top) * 0.5f);
                        cached.layout = std::move(layout);
                    }
                }
            }
            ID2D1SolidColorBrush* brush = window.available
                ? ProgressBrushFor(window.remainingPercent, true)
                : m_pBrushProgressBlue.Get();
            m_pRenderTarget->PushAxisAlignedClip(pillRect.rect, D2D1_ANTIALIAS_MODE_ALIASED);
            if (cached.layout) {
                const D2D1_POINT_2F origin = D2D1::Point2F(
                    std::round((pillLeft + pillRight) * 0.5f - cached.inkCenter.x),
                    std::round((pillTop + pillBottom) * 0.5f - cached.inkCenter.y));
                m_pRenderTarget->DrawTextLayout(origin, cached.layout.Get(), brush);
            } else {
                m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                    value.c_str(), static_cast<UINT32>(value.size()),
                    m_pFontMiniValue.Get(), pillRect.rect, brush);
            }
            m_pRenderTarget->PopAxisAlignedClip();
        }
    }

    float Direct2DRenderer::Px(float logicalPx) const {
        return SnapPx(logicalPx, m_dpiScale);
    }

    void Direct2DRenderer::DrawSyncIndicator(float centerY, SyncState state) {
        ID2D1SolidColorBrush* brush = m_pBrushSyncIdle.Get();
        if (state == SyncState::Syncing) brush = m_pBrushSyncBusy.Get();
        else if (state == SyncState::Synced) brush = m_pBrushSyncSuccess.Get();
        else if (state == SyncState::Failed) brush = m_pBrushProgressRed.Get();
        const float radius = static_cast<float>(ScaleEven(2.0f, m_dpiScale));
        m_pRenderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(Px(kSyncCenterX), std::round(centerY)), radius, radius),
            brush);
    }

    void Direct2DRenderer::DrawCollapsedBar(
        float topY,
        bool expanded,
        const QuotaSnapshot& snapshot,
        SyncState syncState)
    {
        const auto quotaDetail = [](const QuotaWindow& window) {
            return window.available && !window.resetTimeString.empty()
                ? window.resetTimeString + L" 重置"
                : std::wstring(L"官方当前未返回");
        };
        DrawQuotaRow(topY + Px(kRowTops[0]), L"窗口使用限额",
                     quotaDetail(snapshot.window), snapshot.window);
        DrawQuotaRow(topY + Px(kRowTops[1]), L"每周使用限额",
                     quotaDetail(snapshot.weekly), snapshot.weekly);

        const float footerTop = topY + Px(kFooterTop);
        const float footerBottom = footerTop + Px(kFooterHeight);
        const float centerY = (footerTop + footerBottom) * 0.5f;
        std::wstring status = L"等待同步";
        if (syncState == SyncState::Syncing) status = L"同步中";
        else if (syncState == SyncState::Failed) status = L"同步失败";
        else if (syncState == SyncState::Synced) {
            status = L"已同步";
            const time_t fetchedAt = std::chrono::system_clock::to_time_t(snapshot.fetchedAt);
            tm local = {};
            if (fetchedAt > 0 && localtime_s(&local, &fetchedAt) == 0) {
                wchar_t timeText[16] = {};
                swprintf_s(timeText, L"  %02d:%02d", local.tm_hour, local.tm_min);
                status += timeText;
            }
        }
        if (m_pFontFooter) {
            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontFooter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                status.c_str(), static_cast<UINT32>(status.size()), m_pFontFooter.Get(),
                D2D1::RectF(Px(30.0f), footerTop, Px(kExpandButtonLeft), footerBottom),
                m_pBrushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            const wchar_t* action = expanded ? L"收起详情" : L"展开详情";
            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                action, static_cast<UINT32>(wcslen(action)), m_pFontFooter.Get(),
                D2D1::RectF(Px(kExpandButtonLeft), footerTop, Px(kChevronCenterX - 12.0f), footerBottom),
                m_pBrushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        DrawSyncIndicator(centerY, syncState);
        DrawChevron(centerY, expanded);
    }

    void Direct2DRenderer::DrawChevron(float centerY, bool expanded) {
        const float cx = Px(kChevronCenterX);
        const float cy = std::round(centerY);
        const float halfW = static_cast<float>(ScaleEven(4.0f, m_dpiScale));
        const float halfH = static_cast<float>(ScaleEven(2.0f, m_dpiScale));
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

    bool Direct2DRenderer::HasSameContent(
        bool expanded, bool miniMode, const QuotaSnapshot& snapshot, SyncState syncState) const
    {
        if (!m_frameValid || expanded != m_lastExpanded || miniMode != m_lastMini) return false;
        const bool miniVisible = miniMode && !expanded;
        const auto sameQuota = [miniVisible](const QuotaWindow& a, const QuotaWindow& b) {
            return a.available == b.available && a.remainingPercent == b.remainingPercent &&
                (miniVisible || a.resetTimeString == b.resetTimeString);
        };
        if (!sameQuota(snapshot.window, m_lastSnapshot.window) ||
            !sameQuota(snapshot.weekly, m_lastSnapshot.weekly)) return false;
        if (miniVisible) return true;
        if (syncState != m_lastSyncState || snapshot.fetchedAt != m_lastSnapshot.fetchedAt) return false;
        if (!expanded) return true;
        const auto& before = m_lastSnapshot;
        return snapshot.stats.totalTokens == before.stats.totalTokens &&
            snapshot.stats.peakTokens == before.stats.peakTokens &&
            snapshot.stats.longestTask == before.stats.longestTask &&
            snapshot.resetCredits.countAvailable == before.resetCredits.countAvailable &&
            snapshot.resetCredits.availableCount == before.resetCredits.availableCount &&
            snapshot.resetCredits.earliestExpiresAt == before.resetCredits.earliestExpiresAt &&
            RemainingCreditDays(snapshot) == m_lastRemainingDays;
    }

    ID2D1SolidColorBrush* Direct2DRenderer::ProgressBrushFor(double remainingPercent, bool text) const {
        const size_t index = ProgressIndex(remainingPercent);
        if (text) return m_pBrushProgressText[index].Get();
        ID2D1SolidColorBrush* brushes[] = {
            m_pBrushProgressGreen.Get(), m_pBrushProgressYellow.Get(),
            m_pBrushProgressOrange.Get(), m_pBrushProgressRed.Get()
        };
        return brushes[index];
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
        const float left = Px(kPadX);
        const float right = static_cast<float>(m_width) - left;
        const float bottom = topY + Px(kRowTextHeight);
        const float valueLeft = right - Px(76.0f);
        if (m_pFontRowTitle) {
            m_pFontRowTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontRowTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                title.c_str(), static_cast<UINT32>(title.size()), m_pFontRowTitle.Get(),
                D2D1::RectF(left, topY, valueLeft, bottom), m_pBrushText.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (m_pFontRowValueBold) {
            const std::wstring value = window.available
                ? std::to_wstring(static_cast<int>(std::round(window.remainingPercent))) + L"%"
                : L"---";
            m_pFontRowValueBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_pFontRowValueBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                value.c_str(), static_cast<UINT32>(value.size()), m_pFontRowValueBold.Get(),
                D2D1::RectF(valueLeft, topY, right, bottom),
                window.available ? ProgressBrushFor(window.remainingPercent, true) : m_pBrushProgressBlue.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        DrawProgressTrack(left, topY + Px(kTrackOffset), right - left, window);
        if (m_pFontRowDetail) {
            const float detailTop = topY + Px(kDetailOffset);
            const float detailBottom = detailTop + Px(kDetailHeight);
            const float detailLeft = left + Px(80.0f);
            m_pFontRowDetail->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_pFontRowDetail->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                L"剩余额度", 4, m_pFontRowDetail.Get(),
                D2D1::RectF(left, detailTop, detailLeft, detailBottom),
                m_pBrushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            m_pFontRowDetail->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(),
                detail.c_str(), static_cast<UINT32>(detail.size()), m_pFontRowDetail.Get(),
                D2D1::RectF(detailLeft, detailTop, right, detailBottom),
                m_pBrushMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
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
        float trackRadius = FitCornerRadius(trackH, trackW, trackH);

        D2D1_ROUNDED_RECT trackBgRect = D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH), trackRadius, trackRadius);
        m_pRenderTarget->FillRoundedRectangle(trackBgRect, m_pBrushTrackBg.Get());

        const float fillWidth = window.available
            ? RoundEvenPixel(trackW * static_cast<float>(std::clamp(window.remainingPercent, 0.0, 100.0) / 100.0))
            : 0.0f;
        if (fillWidth > 0.0f) {
            m_pRenderTarget->PushAxisAlignedClip(
                D2D1::RectF(trackX, trackY, trackX + fillWidth, trackY + trackH),
                D2D1_ANTIALIAS_MODE_ALIASED);
            m_pRenderTarget->FillRoundedRectangle(trackBgRect, ProgressBrushFor(window.remainingPercent));
            m_pRenderTarget->PopAxisAlignedClip();
        }
    }

    void Direct2DRenderer::DrawStatsSubCard(float topY, float height, const TokenStats& stats) {
        const float padX = Px(kPadX);
        const float w = static_cast<float>(m_width);
        const float cardW = w - padX * 2.0f;
        const float cardCorner = SurfaceCornerRadius(m_dpiScale, cardW, height);

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
            const float colLeft = padX + RoundEvenPixel(static_cast<float>(i) * cardW / 3.0f);
            const float colRight = (i == 2)
                ? (padX + cardW)
                : (padX + RoundEvenPixel(static_cast<float>(i + 1) * cardW / 3.0f));
            const std::wstring& val = cols[i].val;
            const wchar_t* numText = val.empty() ? L"---" : val.c_str();
            UINT32 numLen = val.empty() ? 3 : static_cast<UINT32>(val.size());

            if (m_pFontStatsNum) {
                m_pFontStatsNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colLeft, topY + Px(8.0f), colRight, topY + Px(30.0f));
                ID2D1SolidColorBrush* numBrush = val.empty() ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(), numText, numLen, m_pFontStatsNum.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colLeft, topY + Px(32.0f), colRight, topY + Px(52.0f));
                m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(), cols[i].label, static_cast<UINT32>(wcslen(cols[i].label)), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
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
        const float cardCorner = SurfaceCornerRadius(m_dpiScale, cardW, height);

        DrawSubCardFrame(padX, topY, cardW, height, cardCorner);

        const int64_t expiry = snapshot.resetCredits.earliestExpiresAt;
        const std::wstring expiryText = expiry > 0 ? FormatCompactTime(expiry) : L"--";

        std::wstring remainingDaysText = L"--";
        if (expiry > 0) {
            remainingDaysText = std::to_wstring(RemainingCreditDays(snapshot)) + L" 天";
        }

        const std::wstring countText = snapshot.resetCredits.countAvailable
            ? std::to_wstring(snapshot.resetCredits.availableCount) + L" 次"
            : L"--";

        const wchar_t* labels[] = { L"重置次数", L"过期时间", L"剩余天数" };
        const std::wstring* values[] = { &countText, &expiryText, &remainingDaysText };

        for (int i = 0; i < 3; ++i) {
            const float colLeft = padX + RoundEvenPixel(static_cast<float>(i) * cardW / 3.0f);
            const float colRight = (i == 2)
                ? (padX + cardW)
                : (padX + RoundEvenPixel(static_cast<float>(i + 1) * cardW / 3.0f));

            if (m_pFontRowValue) {
                m_pFontRowValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontRowValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colLeft, topY + Px(8.0f), colRight, topY + Px(30.0f));
                ID2D1SolidColorBrush* numBrush =
                    (*values[i] == L"--") ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(), values[i]->c_str(), static_cast<UINT32>(values[i]->size()), m_pFontRowValue.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colLeft, topY + Px(32.0f), colRight, topY + Px(52.0f));
                m_textCache.Draw(m_pRenderTarget.Get(), m_pDWriteFactory.Get(), labels[i], static_cast<UINT32>(wcslen(labels[i])), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
            }
        }
    }

} // namespace CodexQuotaBar
