#pragma once

#include "Graphics/D2DCommon.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace CodexQuotaBar {

// All render targets use physical pixels (96-DPI target + pre-scaled geometry).
// Snap the actual text origin and baseline, not just the surrounding rectangle.
inline void DrawPixelText(
    ID2D1RenderTarget* target, IDWriteFactory* factory,
    const wchar_t* text, UINT32 length, IDWriteTextFormat* format,
    const D2D1_RECT_F& rect, ID2D1Brush* brush,
    D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE)
{
    if (!target || !factory || !format || !brush || !text || length == 0) return;
    const float width = (std::max)(1.0f, std::round(rect.right - rect.left));
    const float height = (std::max)(1.0f, std::round(rect.bottom - rect.top));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text, length, format, width, height, &layout))) return;
    const auto horizontal = format->GetTextAlignment();
    const auto vertical = format->GetParagraphAlignment();
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    DWRITE_LINE_METRICS line = {};
    UINT32 count = 0;
    const HRESULT lineResult = layout->GetLineMetrics(&line, 1, &count);
    if (FAILED(lineResult) && count > 1) {
        std::vector<DWRITE_LINE_METRICS> lines(count);
        if (SUCCEEDED(layout->GetLineMetrics(lines.data(), count, &count))) {
            line = {};
            for (const auto& item : lines) {
                line.height = (std::max)(line.height, item.height);
                line.baseline = (std::max)(line.baseline, item.baseline);
            }
        }
    }
    if (line.height > 0.0f) {
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
            (std::max)(1.0f, std::ceil(line.height)), std::round(line.baseline));
    }
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics))) return;
    float x = rect.left;
    float y = rect.top;
    if (horizontal == DWRITE_TEXT_ALIGNMENT_CENTER) x += (width - metrics.width) * 0.5f;
    else if (horizontal == DWRITE_TEXT_ALIGNMENT_TRAILING) x += width - metrics.width;
    if (vertical == DWRITE_PARAGRAPH_ALIGNMENT_CENTER) y += (height - metrics.height) * 0.5f;
    else if (vertical == DWRITE_PARAGRAPH_ALIGNMENT_FAR) y += height - metrics.height;
    const bool clip = (options & D2D1_DRAW_TEXT_OPTIONS_CLIP) != 0;
    if (clip) target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    target->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(y)), layout.Get(), brush);
    if (clip) target->PopAxisAlignedClip();
}

} // namespace CodexQuotaBar
