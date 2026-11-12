#pragma once

#include "Graphics/D2DCommon.h"
#include "Core/DpiHelper.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace CodexQuotaBar {

// Per-view, bounded cache. Alignment and color are applied at draw time;
// text, format identity and available dimensions determine the cached layout.
class PixelTextCache {
    struct Entry {
        std::wstring text;
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        float width = 0.0f, height = 0.0f;
        DWRITE_TEXT_METRICS metrics = {};
    };
    std::deque<Entry> m_entries;
    static constexpr size_t kCapacity = 96;

    const Entry* FindOrCreate(IDWriteFactory* factory, const wchar_t* text,
                             UINT32 length, IDWriteTextFormat* format, float width, float height) {
        const std::wstring_view value(text, length);
        for (const auto& entry : m_entries) {
            if (entry.format.Get() == format && entry.width == width && entry.height == height &&
                entry.text == value) return &entry;
        }
        Entry entry;
        entry.text = value;
        entry.format = format;
        entry.width = width;
        entry.height = height;
        if (FAILED(factory->CreateTextLayout(text, length, format, width, height, &entry.layout))) return nullptr;
        entry.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        entry.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_LINE_METRICS line = {};
        UINT32 count = 0;
        const HRESULT result = entry.layout->GetLineMetrics(&line, 1, &count);
        if (FAILED(result) && count > 1) {
            std::vector<DWRITE_LINE_METRICS> lines(count);
            if (SUCCEEDED(entry.layout->GetLineMetrics(lines.data(), count, &count))) {
                line = {};
                for (const auto& item : lines) {
                    line.height = (std::max)(line.height, item.height);
                    line.baseline = (std::max)(line.baseline, item.baseline);
                }
            }
        }
        if (line.height > 0.0f) {
            entry.layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                static_cast<float>(ScaleEven(std::ceil(line.height), 1.0f)), std::round(line.baseline));
        }
        if (FAILED(entry.layout->GetMetrics(&entry.metrics))) return nullptr;
        if (m_entries.size() == kCapacity) m_entries.pop_front();
        m_entries.push_back(std::move(entry));
        return &m_entries.back();
    }

public:
    void Clear() { m_entries.clear(); }

    void Draw(ID2D1RenderTarget* target, IDWriteFactory* factory,
              const wchar_t* text, UINT32 length, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, ID2D1Brush* brush,
              D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE) {
        if (!target || !factory || !format || !brush || !text || length == 0) return;
        const float width = (std::max)(2.0f, std::round(rect.right - rect.left));
        const float height = (std::max)(2.0f, std::round(rect.bottom - rect.top));
        const Entry* entry = FindOrCreate(factory, text, length, format, width, height);
        if (!entry) return;
        float x = rect.left, y = rect.top;
        const auto horizontal = format->GetTextAlignment();
        const auto vertical = format->GetParagraphAlignment();
        if (horizontal == DWRITE_TEXT_ALIGNMENT_CENTER) x += (width - entry->metrics.width) * 0.5f;
        else if (horizontal == DWRITE_TEXT_ALIGNMENT_TRAILING) x += width - entry->metrics.width;
        if (vertical == DWRITE_PARAGRAPH_ALIGNMENT_CENTER) y += (height - entry->metrics.height) * 0.5f;
        else if (vertical == DWRITE_PARAGRAPH_ALIGNMENT_FAR) y += height - entry->metrics.height;
        const bool clip = (options & D2D1_DRAW_TEXT_OPTIONS_CLIP) != 0;
        if (clip) target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
        target->DrawTextLayout(D2D1::Point2F(std::round(x), std::round(y)), entry->layout.Get(), brush);
        if (clip) target->PopAxisAlignedClip();
    }
};

} // namespace CodexQuotaBar
