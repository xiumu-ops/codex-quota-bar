#include "Graphics/Direct2DRenderer.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Layout.h"

#include <cmath>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cwchar>

namespace CodexQuotaBar {

    namespace {
        // 两条额度详情与底部同步状态值共用同一条视觉中心线。
        constexpr float kDetailCenterX = 222.0f;
        constexpr float kDetailHalfWidth = 105.0f;

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

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &m_pDWriteFactory);
        if (FAILED(hr)) return hr;

        CreateTextFormats();
        return S_OK;
    }

    void Direct2DRenderer::SetDpiScale(float dpiScale) {
        m_dpiScale = dpiScale;
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
        return SUCCEEDED(collection->FindFamilyName(fontFamily.c_str(), &index, &exists)) &&
               exists;
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
        m_pFontRowValue.Reset();
        m_pFontRowValueBold.Reset();
        m_pFontFooter.Reset();
        m_pFontStatsNum.Reset();
        m_pFontStatsLabel.Reset();
    }

    void Direct2DRenderer::CreateTextFormats() {
        if (!m_pDWriteFactory) return;

        const wchar_t* fontFamily = m_fontFamily.c_str();
        float s = m_dpiScale;

        // 全卡片统一 16px，字号层级仅靠字重区分
        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontRowTitle);

        // 右侧数值 / 未返回状态（16px；百分比为加粗变体）
        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontRowValue);

        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontRowValueBold);

        // 常驻卡片底部说明文本
        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontFooter);

        // 统计子卡片数值（加粗）与标签
        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontStatsNum);

        m_pDWriteFactory->CreateTextFormat(
            fontFamily, NULL,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f * s, L"zh-cn", &m_pFontStatsLabel);
    }

    HRESULT Direct2DRenderer::CreateDeviceResources() {
        if (m_pRenderTarget) return S_OK;
        if (!m_pD2DFactory || !m_hwnd) return E_UNEXPECTED;

        RECT rc = {};
        if (!GetClientRect(m_hwnd, &rc)) return HRESULT_FROM_WIN32(GetLastError());
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        if (size.width == 0 || size.height == 0) return E_INVALIDARG;
        m_width = size.width;
        m_height = size.height;

        HRESULT hr = CreateBackingSurface(size.width, size.height);
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

        hr = m_pRenderTarget->BindDC(m_memoryDc, &rc);
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
            { m_palette.trackBg, &m_pBrushTrackBg },
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
        RECT windowRect = {};
        if (!GetWindowRect(m_hwnd, &windowRect)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        POINT destination = { windowRect.left, windowRect.top };
        POINT source = { 0, 0 };
        SIZE size = { static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC screenDc = GetDC(nullptr);
        if (!screenDc) return HRESULT_FROM_WIN32(GetLastError());
        const BOOL presented = UpdateLayeredWindow(
            m_hwnd,
            screenDc,
            &destination,
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

        // 1. 绘制带逐像素 Alpha 的圆角悬浮卡片背景
        const bool miniVisible = miniMode && !expanded;
        const float outerRadius = miniVisible
            ? ScaledMiniBarMetrics(m_dpiScale).outerCornerRadius
            : ScaleF(static_cast<float>(CORNER_RADIUS), m_dpiScale);
        const D2D1_ROUNDED_RECT background = D2D1::RoundedRect(
            D2D1::RectF(0.0f, 0.0f, w, h), outerRadius, outerRadius);
        m_pRenderTarget->FillRoundedRectangle(background, m_pBrushSurface.Get());

        // 2. 迷你态只呈现额度与缩短的进度条；标准展开态追加详情卡片。
        if (miniVisible) {
            DrawMiniBar(snapshot);
        } else {
            const float collapsedH = ScaleF(static_cast<float>(COLLAPSED_HEIGHT), m_dpiScale);
            DrawCollapsedBar(0.0f, expanded, snapshot, syncState);
            if (expanded) {
                const float padX = ScaleF(8.0f, m_dpiScale);
                m_pRenderTarget->DrawLine(
                    D2D1::Point2F(padX, collapsedH), D2D1::Point2F(w - padX, collapsedH),
                    m_pBrushDivider.Get(), 1.0f);
                const float cardH = ScaleF(66.0f, m_dpiScale);
                DrawStatsSubCard(collapsedH + ScaleF(8.0f, m_dpiScale), cardH, snapshot.stats);
                DrawResetSubCard(
                    collapsedH + ScaleF(8.0f, m_dpiScale) + cardH + ScaleF(5.0f, m_dpiScale),
                    cardH, snapshot);
            }
        }

        // 3. 外描边保留在逐像素 Alpha 画面内，确保 OuterBorder 个性配置生效。
        const float strokeWidth = miniVisible
            ? ScaledMiniBarMetrics(m_dpiScale).outerBorderThickness
            : ScaleF(1.0f, m_dpiScale);
        const float strokeInset = strokeWidth / 2.0f;
        const float strokeRadius = (std::max)(0.0f, outerRadius - strokeInset);
        const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(
            D2D1::RectF(
                strokeInset, strokeInset,
                w - strokeInset, h - strokeInset),
            strokeRadius, strokeRadius);
        m_pRenderTarget->DrawRoundedRectangle(
            outer, m_pBrushBorder.Get(), strokeWidth);

        hr = m_pRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        } else if (SUCCEEDED(hr)) {
            hr = PresentLayeredWindow();
            if (FAILED(hr)) {
                DiscardDeviceResources();
            }
        }
        return hr;
    }

    void Direct2DRenderer::DrawMiniBar(const QuotaSnapshot& snapshot) {
        const MiniBarMetrics metrics = ScaledMiniBarMetrics(m_dpiScale);
        QuotaWindow unavailable;
        const QuotaWindow* windows[2] = {};
        int count = 0;
        if (snapshot.window.available) windows[count++] = &snapshot.window;
        if (snapshot.weekly.available) windows[count++] = &snapshot.weekly;
        if (count == 0) windows[count++] = &unavailable;

        const float segmentWidth = static_cast<float>(m_width / count);
        for (int i = 0; i < count; ++i) {
            DrawMiniQuota(segmentWidth * i, segmentWidth, *windows[i]);
        }

        if (count == 2) {
            const float dividerX = segmentWidth;
            const float dividerHeight = MiniDividerHeight(m_height, metrics);
            const float dividerTop =
                (static_cast<float>(m_height) - dividerHeight) / 2.0f;
            m_pRenderTarget->DrawLine(
                D2D1::Point2F(dividerX, dividerTop),
                D2D1::Point2F(dividerX, dividerTop + dividerHeight),
                m_pBrushDivider.Get(), metrics.dividerThickness);
        }
    }

    void Direct2DRenderer::DrawMiniQuota(
        float left,
        float width,
        const QuotaWindow& window)
    {
        const std::wstring value = window.available
            ? std::to_wstring(static_cast<int>(std::round(window.remainingPercent))) + L"%"
            : L"---";

        const MiniBarMetrics metrics = ScaledMiniBarMetrics(m_dpiScale);
        const float pillLeft = left + metrics.contentInset;
        const float pillRight = left + width - metrics.contentInset;
        const float pillTop = metrics.contentInset;
        const float pillBottom =
            static_cast<float>(m_height) - metrics.contentInset;
        const float pillW = (std::max)(0.0f, pillRight - pillLeft);
        const float pillRadius = metrics.pillCornerRadius;

        // 1. 胶囊槽底色（接入已有 TrackBackground 自定义外观逻辑）
        const D2D1_ROUNDED_RECT pillRect = D2D1::RoundedRect(
            D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom),
            pillRadius, pillRadius);
        m_pRenderTarget->FillRoundedRectangle(pillRect, m_pBrushTrackBg.Get());

        // 2. 拓宽内部进度条（接入已有 ProgressBrushFor 色彩梯度与自定义外观逻辑）
        if (window.available && window.remainingPercent > 0.0) {
            float fillW = std::max(pillRadius * 2.0f, pillW * static_cast<float>(window.remainingPercent / 100.0));
            fillW = std::min(fillW, pillW);
            ID2D1SolidColorBrush* statusBrush = ProgressBrushFor(window.remainingPercent);
            if (statusBrush && m_pBrushHalo) {
                D2D1_COLOR_F tintColor = statusBrush->GetColor();
                tintColor.a = 0.32f;
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

        // 3. 百分比文字（大小保持 16px Bold 不变，在胶囊内实现像素级水平与垂直双向绝对居中）
        if (m_pFontRowValueBold) {
            m_pFontRowValueBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pFontRowValueBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F textRect = D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom);
            ID2D1SolidColorBrush* brush = window.available
                ? ProgressBrushFor(window.remainingPercent)
                : m_pBrushProgressBlue.Get();
            m_pRenderTarget->DrawTextW(
                value.c_str(), static_cast<UINT32>(value.size()),
                m_pFontRowValueBold.Get(), textRect, brush);
        }
    }

    void Direct2DRenderer::DrawSyncIndicator(float topY, SyncState state) {
        float cx = ScaleF(349.0f, m_dpiScale);
        float cy = topY + ScaleF(76.5f, m_dpiScale);
        float coreRadius = ScaleF(4.0f, m_dpiScale);
        float haloRadius = ScaleF(6.0f, m_dpiScale);

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
            topY + static_cast<float>(Scale(3.0f, m_dpiScale)),
            L"窗口使用限额", quotaDetail(snapshot.window), snapshot.window);

        DrawQuotaRow(
            topY + static_cast<float>(Scale(33.0f, m_dpiScale)),
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

        if (m_pFontFooter) {
            const std::wstring footerTitle = L"同步服务状态";
            const float titleWidth = MeasureTextWidth(footerTitle, m_pFontFooter.Get());

            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontFooter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F footerTitleRect = D2D1::RectF(
                ScaleF(8.0f, m_dpiScale), topY + ScaleF(64.0f, m_dpiScale),
                ScaleF(8.0f, m_dpiScale) + titleWidth + ScaleF(4.0f, m_dpiScale),
                topY + ScaleF(86.0f, m_dpiScale));
            m_pRenderTarget->DrawTextW(
                footerTitle.c_str(), static_cast<UINT32>(footerTitle.size()),
                m_pFontFooter.Get(), footerTitleRect, m_pBrushMuted.Get());

            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            D2D1_RECT_F syncDetailRect = D2D1::RectF(
                ScaleF(kDetailCenterX - kDetailHalfWidth, m_dpiScale),
                topY + ScaleF(64.0f, m_dpiScale),
                ScaleF(kDetailCenterX + kDetailHalfWidth, m_dpiScale),
                topY + ScaleF(86.0f, m_dpiScale));
            m_pRenderTarget->DrawTextW(
                syncDetail, static_cast<UINT32>(wcslen(syncDetail)),
                m_pFontFooter.Get(), syncDetailRect, m_pBrushMuted.Get());
        }

        DrawSyncIndicator(topY, syncState);
        DrawChevron(topY, expanded);
    }

    void Direct2DRenderer::DrawChevron(float topY, bool expanded) {
        float cx = ScaleF(366.0f, m_dpiScale);
        float cy = topY + ScaleF(76.5f, m_dpiScale);
        float halfW = ScaleF(6.0f, m_dpiScale);
        float halfH = ScaleF(4.0f, m_dpiScale);

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

        m_pRenderTarget->DrawLine(p1, p2, m_pBrushChevron.Get(), ScaleF(2.0f, m_dpiScale));
        m_pRenderTarget->DrawLine(p2, p3, m_pBrushChevron.Get(), ScaleF(2.0f, m_dpiScale));
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
        float padX = static_cast<float>(Scale(8.0f, m_dpiScale));
        float w = static_cast<float>(m_width);
        float rightX = w - padX;

        std::wstring numStr;
        if (window.available) {
            const int pct = static_cast<int>(std::round(window.remainingPercent));
            numStr = std::to_wstring(pct) + L"%";
        } else {
            numStr = L"---";
        }
        const float numWidth = MeasureTextWidth(numStr, m_pFontRowValueBold.Get());
        const float numLeft = rightX - numWidth;

        const float titleWidth = MeasureTextWidth(title, m_pFontRowTitle.Get());

        // 1. 标题
        if (m_pFontRowTitle) {
            m_pFontRowTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pFontRowTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F titleRect = D2D1::RectF(padX, topY, padX + titleWidth + ScaleF(4.0f, m_dpiScale), topY + ScaleF(24.0f, m_dpiScale));
            m_pRenderTarget->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), m_pFontRowTitle.Get(), titleRect, m_pBrushText.Get());
        }

        // 每行中部辅助信息
        if (m_pFontFooter && !detail.empty()) {
            m_pFontFooter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pFontFooter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const float center = ScaleF(kDetailCenterX, m_dpiScale);
            const float boxHalfW = ScaleF(kDetailHalfWidth, m_dpiScale);
            D2D1_RECT_F detailRect = D2D1::RectF(
                center - boxHalfW, topY,
                center + boxHalfW, topY + ScaleF(24.0f, m_dpiScale));
            m_pRenderTarget->DrawTextW(
                detail.c_str(), static_cast<UINT32>(detail.size()),
                m_pFontFooter.Get(), detailRect, m_pBrushMuted.Get());
        }

        // 2. 右侧数值 / 状态
        IDWriteTextFormat* numFormat = m_pFontRowValueBold.Get();
        if (numFormat) {
            numFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            numFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F valRect = D2D1::RectF(
                numLeft, topY, rightX,
                topY + static_cast<float>(Scale(24.0f, m_dpiScale)));
            ID2D1SolidColorBrush* brush = window.available
                ? ProgressBrushFor(window.remainingPercent)
                : m_pBrushProgressBlue.Get();
            m_pRenderTarget->DrawTextW(numStr.c_str(), static_cast<UINT32>(numStr.size()), numFormat, valRect, brush);
        }

        // 3. 满宽细胶囊进度条
        const float trackX = padX;
        const float trackW = w - padX * 2.0f;
        const float trackY = topY + ScaleF(25.0f, m_dpiScale);
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
            : std::max(4.0f, ScaleF(5.0f, m_dpiScale));
        float trackRadius = trackH / 2.0f;

        D2D1_ROUNDED_RECT trackBgRect = D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH), trackRadius, trackRadius);
        m_pRenderTarget->FillRoundedRectangle(trackBgRect, m_pBrushTrackBg.Get());

        if (window.available && window.remainingPercent > 0.0) {
            float fillW = std::max(trackH, trackW * static_cast<float>(window.remainingPercent / 100.0));
            fillW = std::min(fillW, trackW);
            D2D1_ROUNDED_RECT fillRect = D2D1::RoundedRect(D2D1::RectF(trackX, trackY, trackX + fillW, trackY + trackH), trackRadius, trackRadius);
            m_pRenderTarget->FillRoundedRectangle(fillRect, ProgressBrushFor(window.remainingPercent));
        }
    }

    void Direct2DRenderer::DrawStatsSubCard(float topY, float height, const TokenStats& stats) {
        float padX = ScaleF(8.0f, m_dpiScale);
        float w = static_cast<float>(m_width);
        float cardW = w - padX * 2.0f;
        float cardCorner = ScaleF(6.0f, m_dpiScale);

        D2D1_ROUNDED_RECT subCardRect = D2D1::RoundedRect(D2D1::RectF(padX, topY, padX + cardW, topY + height), cardCorner, cardCorner);
        m_pRenderTarget->FillRoundedRectangle(subCardRect, m_pBrushStatsCardBg.Get());
        m_pRenderTarget->DrawRoundedRectangle(subCardRect, m_pBrushStatsCardBorder.Get(), 0.75f);

        float colW = cardW / 3.0f;
        struct ColData {
            const std::wstring& val;
            const wchar_t* label;
        } cols[] = {
            { stats.totalTokens, L"累计tokens" },
            { stats.peakTokens, L"峰值tokens" },
            { stats.longestTask, L"最长聊天" }
        };

        for (int i = 0; i < 3; ++i) {
            float colX = padX + i * colW;
            const std::wstring& val = cols[i].val;
            const wchar_t* numText = val.empty() ? L"---" : val.c_str();
            UINT32 numLen = val.empty() ? 3 : static_cast<UINT32>(val.size());

            if (m_pFontStatsNum) {
                m_pFontStatsNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colX, topY + ScaleF(8.0f, m_dpiScale), colX + colW, topY + ScaleF(32.0f, m_dpiScale));
                ID2D1SolidColorBrush* numBrush = val.empty() ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_pRenderTarget->DrawTextW(numText, numLen, m_pFontStatsNum.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colX, topY + ScaleF(36.0f, m_dpiScale), colX + colW, topY + ScaleF(58.0f, m_dpiScale));
                m_pRenderTarget->DrawTextW(cols[i].label, static_cast<UINT32>(wcslen(cols[i].label)), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
            }
        }
    }

    void Direct2DRenderer::DrawResetSubCard(float topY, float height, const QuotaSnapshot& snapshot) {
        float padX = ScaleF(8.0f, m_dpiScale);
        float w = static_cast<float>(m_width);
        float cardW = w - padX * 2.0f;
        float cardCorner = ScaleF(6.0f, m_dpiScale);

        D2D1_ROUNDED_RECT subCardRect = D2D1::RoundedRect(D2D1::RectF(padX, topY, padX + cardW, topY + height), cardCorner, cardCorner);
        m_pRenderTarget->FillRoundedRectangle(subCardRect, m_pBrushStatsCardBg.Get());
        m_pRenderTarget->DrawRoundedRectangle(subCardRect, m_pBrushStatsCardBorder.Get(), 0.75f);

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

        const float colW = cardW / 3.0f;
        const wchar_t* labels[] = { L"重置次数", L"过期时间", L"剩余天数" };
        const std::wstring* values[] = { &countText, &expiryText, &remainingDaysText };

        for (int i = 0; i < 3; ++i) {
            const float colX = padX + i * colW;
            const float colWidth = colW;

            if (m_pFontStatsNum) {
                m_pFontStatsNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F numRect = D2D1::RectF(colX, topY + ScaleF(8.0f, m_dpiScale), colX + colWidth, topY + ScaleF(32.0f, m_dpiScale));
                ID2D1SolidColorBrush* numBrush =
                    (*values[i] == L"--") ? m_pBrushMuted.Get() : m_pBrushText.Get();
                m_pRenderTarget->DrawTextW(values[i]->c_str(), static_cast<UINT32>(values[i]->size()), m_pFontStatsNum.Get(), numRect, numBrush);
            }

            if (m_pFontStatsLabel) {
                m_pFontStatsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_pFontStatsLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F lblRect = D2D1::RectF(colX, topY + ScaleF(36.0f, m_dpiScale), colX + colWidth, topY + ScaleF(58.0f, m_dpiScale));
                m_pRenderTarget->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), m_pFontStatsLabel.Get(), lblRect, m_pBrushMuted.Get());
            }
        }
    }

} // namespace CodexQuotaBar
