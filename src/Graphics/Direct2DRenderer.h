#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"
#include "Core/Models.h"

namespace CodexQuotaBar {

    class Direct2DRenderer {
    public:
        Direct2DRenderer();
        ~Direct2DRenderer();

        HRESULT Initialize(HWND hwnd);
        void SetDpiScale(float dpiScale);
        bool SetAppearance(
            const ThemePalette& palette,
            const std::wstring& fontFamily,
            std::wstring* validationError = nullptr);
        const ThemePalette& Palette() const { return m_palette; }
        const std::wstring& FontFamily() const { return m_fontFamily; }
        HRESULT Resize(UINT width, UINT height);

        HRESULT Render(
            bool expanded,
            const QuotaSnapshot& snapshot,
            SyncState syncState);

    private:
        HRESULT CreateDeviceResources();
        void DiscardDeviceResources();
        void CreateTextFormats();
        void DiscardTextFormats();
        bool FontFamilyExists(const std::wstring& fontFamily) const;

        void DrawCollapsedBar(
            float topY,
            bool expanded,
            const QuotaSnapshot& snapshot,
            SyncState syncState);
        void DrawSyncIndicator(float topY, SyncState state);
        void DrawChevron(float topY, bool expanded);
        void DrawQuotaRow(
            float topY,
            const std::wstring& title,
            const std::wstring& detail,
            const QuotaWindow& window);
        void DrawStatsSubCard(float topY, float height, const TokenStats& stats);
        void DrawResetSubCard(float topY, float height, const QuotaSnapshot& snapshot);

        ID2D1SolidColorBrush* ProgressBrushFor(double remainingPercent) const;
        float MeasureTextWidth(
            const std::wstring& text,
            IDWriteTextFormat* format) const;

        HWND m_hwnd = NULL;
        float m_dpiScale = 1.0f;
        UINT m_width = 0;
        UINT m_height = 0;

        ThemePalette m_palette;
        std::wstring m_fontFamily = L"Microsoft YaHei UI";

        // 设备无关资源 (Device-Independent Resources)
        ComPtr<ID2D1Factory> m_pD2DFactory;
        ComPtr<IDWriteFactory> m_pDWriteFactory;
        ComPtr<IDWriteTextFormat> m_pFontRowTitle;
        ComPtr<IDWriteTextFormat> m_pFontRowValue;
        ComPtr<IDWriteTextFormat> m_pFontRowValueBold;
        ComPtr<IDWriteTextFormat> m_pFontFooter;
        ComPtr<IDWriteTextFormat> m_pFontStatsNum;
        ComPtr<IDWriteTextFormat> m_pFontStatsLabel;

        // 设备相关资源 (Device-Dependent Resources)
        ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
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
