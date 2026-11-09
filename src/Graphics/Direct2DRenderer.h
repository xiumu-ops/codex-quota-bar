#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"
#include "Core/Models.h"

namespace CodexQuotaBar {

    struct MiniBarMetrics;

    class Direct2DRenderer {
    public:
        Direct2DRenderer();
        ~Direct2DRenderer();

        HRESULT Initialize(HWND hwnd);
        void SetDpiScale(float dpiScale);
        void SetScales(float monitorDpiScale, float userScale);
        bool SetAppearance(
            const ThemePalette& palette,
            const std::wstring& fontFamily,
            int backgroundTransparency,
            std::wstring* validationError = nullptr);
        const ThemePalette& Palette() const { return m_palette; }
        const std::wstring& FontFamily() const { return m_fontFamily; }
        bool FontFamilyExists(const std::wstring& fontFamily) const;
        HRESULT Resize(UINT width, UINT height);

        HRESULT Render(
            bool expanded,
            bool miniMode,
            const QuotaSnapshot& snapshot,
            SyncState syncState);

    private:
        HRESULT CreateDeviceResources();
        void DiscardDeviceResources();
        HRESULT CreateBackingSurface(UINT width, UINT height);
        void DiscardBackingSurface();
        HRESULT PresentLayeredWindow();
        void CreateTextFormats();
        void DiscardTextFormats();

        void DrawCollapsedBar(
            float topY,
            bool expanded,
            const QuotaSnapshot& snapshot,
            SyncState syncState);
        void DrawMiniBar(
            const QuotaSnapshot& snapshot,
            const MiniBarMetrics& metrics);
        void DrawMiniQuotaSlot(
            float pillLeft,
            float pillTop,
            float pillRight,
            float pillBottom,
            float pillRadius,
            const QuotaWindow& window);
        void DrawSyncIndicator(float centerY, SyncState state);
        void DrawChevron(float centerY, bool expanded);
        // 逻辑像素 → 统一取整的物理像素，所有矩形边缘与派生坐标均经过它。
        float Px(float logicalPx) const;
        void DrawQuotaRow(
            float topY,
            const std::wstring& title,
            const std::wstring& detail,
            const QuotaWindow& window);
        void DrawProgressTrack(
            float trackX,
            float trackY,
            float trackW,
            const QuotaWindow& window,
            float customHeight = 0.0f);
        void DrawStatsSubCard(float topY, float height, const TokenStats& stats);
        void DrawResetSubCard(float topY, float height, const QuotaSnapshot& snapshot);
        void DrawSubCardFrame(float left, float top, float width, float height, float corner);

        ID2D1SolidColorBrush* ProgressBrushFor(double remainingPercent) const;
        float MeasureTextWidth(
            const std::wstring& text,
            IDWriteTextFormat* format) const;

        HWND m_hwnd = NULL;
        float m_monitorDpiScale = 1.0f;
        float m_userScale = 1.0f;
        float m_dpiScale = 1.0f;
        float m_miniValueWidth = 0.0f;
        UINT m_width = 0;
        UINT m_height = 0;

        ThemePalette m_palette;
        std::wstring m_fontFamily = L"Microsoft YaHei UI";
        int m_backgroundTransparency = 0;

        // 设备无关资源 (Device-Independent Resources)
        ComPtr<ID2D1Factory> m_pD2DFactory;
        ComPtr<ID2D1StrokeStyle> m_pRoundStrokeStyle;
        ComPtr<IDWriteFactory> m_pDWriteFactory;
        ComPtr<IDWriteTextFormat> m_pFontRowTitle;
        ComPtr<IDWriteTextFormat> m_pFontRowDetail;
        ComPtr<IDWriteTextFormat> m_pFontRowValue;
        ComPtr<IDWriteTextFormat> m_pFontRowValueBold;
        ComPtr<IDWriteTextFormat> m_pFontMiniValue;
        ComPtr<IDWriteTextFormat> m_pFontFooter;
        ComPtr<IDWriteTextFormat> m_pFontStatsNum;
        ComPtr<IDWriteTextFormat> m_pFontStatsLabel;

        // 设备相关资源 (Device-Dependent Resources)
        ComPtr<ID2D1DCRenderTarget> m_pRenderTarget;
        HDC m_memoryDc = nullptr;
        HBITMAP m_dibBitmap = nullptr;
        HGDIOBJ m_previousBitmap = nullptr;
        void* m_dibBits = nullptr;
        ComPtr<ID2D1SolidColorBrush> m_pBrushSurface;
        ComPtr<ID2D1SolidColorBrush> m_pBrushStatsCardBg;
        ComPtr<ID2D1SolidColorBrush> m_pBrushStatsCardBorder;
        ComPtr<ID2D1SolidColorBrush> m_pBrushText;
        ComPtr<ID2D1SolidColorBrush> m_pBrushMuted;
        ComPtr<ID2D1SolidColorBrush> m_pBrushTrackBg;
        ComPtr<ID2D1SolidColorBrush> m_pBrushProgressBlue;
        ComPtr<ID2D1SolidColorBrush> m_pBrushProgressGreen;
        ComPtr<ID2D1SolidColorBrush> m_pBrushProgressYellow;
        ComPtr<ID2D1SolidColorBrush> m_pBrushProgressOrange;
        ComPtr<ID2D1SolidColorBrush> m_pBrushProgressRed;
        ComPtr<ID2D1SolidColorBrush> m_pBrushBorder;
        ComPtr<ID2D1SolidColorBrush> m_pBrushDivider;
        ComPtr<ID2D1SolidColorBrush> m_pBrushChevron;
        ComPtr<ID2D1SolidColorBrush> m_pBrushSyncSuccess;
        ComPtr<ID2D1SolidColorBrush> m_pBrushSyncIdle;
        ComPtr<ID2D1SolidColorBrush> m_pBrushSyncBusy;
        ComPtr<ID2D1SolidColorBrush> m_pBrushHalo;
    };

} // namespace CodexQuotaBar
