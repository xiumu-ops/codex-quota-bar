#include "UI/AppearanceDialog.h"

#include "Core/Appearance.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Layout.h"
#include "Core/Settings.h"
#include "UI/WindowEffects.h"

#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

namespace CodexQuotaBar {
namespace {

    constexpr UINT WM_CQB_APPEARANCE_CONFIRM = WM_APP + 22;
    constexpr int kEditControlBase = 100;
    constexpr int kTransparencyEditControlId = 200;
    constexpr int kFontEditControlId = 201;
    constexpr int kScaleEditControlId = 202;
    constexpr size_t kColorCount = 19;
    constexpr float kDialogWidth = 714.0f;
    constexpr float kDialogHeight = 496.0f;

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
        AppearanceApplyCallback applyCallback;
        int hoverButton = -1;
        int invalidField = -1;
        bool accepted = false;
        bool appliedNotice = false;
        bool done = false;
        float dpiScale = 1.0f;
        float windowCornerRadius = 8.0f;
        ThemePalette palette;
        std::wstring validationMessage;
        RECT transparencyOutline = {};
        HWND transparencyEdit = nullptr;
        bool transparencyFocused = false;
        bool transparencyInvalid = false;
        RECT scaleOutline = {};
        HWND scaleEdit = nullptr;
        bool scaleFocused = false;
        bool scaleInvalid = false;
        RECT fontOutline = {};
        HWND fontEdit = nullptr;
        bool fontFocused = false;
        bool fontInvalid = false;
        RECT fontBrowseRect = {};
        std::wstring currentFontFamily;
        RECT applyRect = {};
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

    bool InitializeDrawing(AppearanceDialogState& state);
    bool UpdateDialogFonts(AppearanceDialogState& state, float scale);
    float FitScaleToMonitor(HMONITOR monitor, float requestedScale);

    void LayoutAppearanceDialog(HWND hwnd, AppearanceDialogState& state) {
        const float scale = state.dpiScale;
        const auto pixels = [scale](float value) { return Scale(value, scale); };
        const int outlineWidth = pixels(152.0f);
        const int outlineHeight = pixels(28.0f);
        const int swatchWidth = pixels(36.0f);
        const int swatchHeight = pixels(24.0f);
        const int swatchOffsetY = (outlineHeight - swatchHeight) / 2;
        const int editPadX = pixels(6.0f);

        int measuredFontHeight = pixels(19.0f);
        if (HDC dc = GetDC(hwnd)) {
            HGDIOBJ oldFont = SelectObject(dc, state.editFont);
            TEXTMETRICW metrics = {};
            if (GetTextMetricsW(dc, &metrics) && metrics.tmHeight > 0) {
                measuredFontHeight = static_cast<int>(metrics.tmHeight);
            }
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(hwnd, dc);
        }
        const int editHeight = (std::max)(1, (std::min)(outlineHeight - 2, measuredFontHeight));
        const int editOffsetY = (outlineHeight - editHeight) / 2;
        const auto placeEdit = [&](HWND edit, const RECT& outline) {
            SetWindowPos(edit, nullptr, outline.left + editPadX, outline.top + editOffsetY,
                         outlineWidth - editPadX * 2, editHeight,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        };
        const auto outlineAt = [&](float x, float y) {
            const int left = pixels(x);
            const int top = pixels(y);
            return RECT{ left, top, left + outlineWidth, top + outlineHeight };
        };

        for (size_t index = 0; index < kColorCount; ++index) {
            const bool leftColumn = index < 10;
            const int row = static_cast<int>(leftColumn ? index : index - 10);
            const float columnX = leftColumn ? 18.0f : 386.0f;
            ColorField& field = state.fields[index];
            field.outline = outlineAt(columnX + 112.0f, 58.0f + row * 34.0f);
            const int swatchLeft = pixels(columnX + 274.0f);
            const LONG swatchTop = field.outline.top + swatchOffsetY;
            field.swatch = { swatchLeft, swatchTop, swatchLeft + swatchWidth, swatchTop + swatchHeight };
            placeEdit(field.edit, field.outline);
        }

        state.transparencyOutline = outlineAt(498.0f, 364.0f);
        state.scaleOutline = outlineAt(498.0f, 398.0f);
        state.fontOutline = outlineAt(130.0f, 398.0f);
        placeEdit(state.transparencyEdit, state.transparencyOutline);
        placeEdit(state.scaleEdit, state.scaleOutline);
        placeEdit(state.fontEdit, state.fontOutline);
        const int browseLeft = pixels(292.0f);
        state.fontBrowseRect = { browseLeft, state.fontOutline.top,
                                 browseLeft + pixels(36.0f), state.fontOutline.bottom };

        const int buttonTop = pixels(448.0f);
        const int buttonBottom = buttonTop + pixels(34.0f);
        const int buttonWidth = pixels(88.0f);
        const auto buttonAt = [&](float x) {
            const int left = pixels(x);
            return RECT{ left, buttonTop, left + buttonWidth, buttonBottom };
        };
        state.applyRect = buttonAt(412.0f);
        state.confirmRect = buttonAt(510.0f);
        state.cancelRect = buttonAt(608.0f);
    }

    bool CheckFontFamilyExists(IDWriteFactory* factory, const std::wstring& name) {
        if (!factory || name.empty()) return false;
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(factory->GetSystemFontCollection(&collection)) || !collection) return false;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(name.c_str(), &index, &exists)) && exists) {
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
                    if (_wcsicmp(name.c_str(), localizedName.c_str()) == 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }


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
                D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (!state->d2dFactory ||
            FAILED(state->d2dFactory->CreateDCRenderTarget(
                &properties, &state->renderTarget)) ||
            FAILED(state->renderTarget->CreateSolidColorBrush(
                state->palette.text, &state->brush))) {
            state->brush.Reset();
            state->renderTarget.Reset();
            return false;
        }
        state->renderTarget->SetDpi(96.0f, 96.0f);
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

    inline void DrawCrispRoundedRect(
        ID2D1RenderTarget* rt,
        ID2D1Brush* brush,
        const D2D1_RECT_F& rect,
        float cornerRadius,
        float strokeWidth)
    {
        const float inset = strokeWidth * 0.5f;
        const D2D1_ROUNDED_RECT stroked = D2D1::RoundedRect(
            D2D1::RectF(
                rect.left + inset, rect.top + inset,
                rect.right - inset, rect.bottom - inset),
            (std::max)(0.0f, cornerRadius - inset),
            (std::max)(0.0f, cornerRadius - inset));
        rt->DrawRoundedRectangle(stroked, brush, strokeWidth);
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
        const float radius = static_cast<float>(Scale(6.0f, state->dpiScale));
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        state->brush->SetColor(hovered ? state->palette.menuHover : state->palette.statsCardBg);
        state->renderTarget->FillRoundedRectangle(rounded, state->brush.Get());
        state->brush->SetColor(state->palette.statsCardBorder);
        DrawCrispRoundedRect(state->renderTarget.Get(), state->brush.Get(), rect, radius, 2.0f);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawText(state, text, state->bodyFont.Get(), rect, state->palette.text);
    }

    std::wstring ReadFieldText(HWND edit) {
        wchar_t value[128] = {};
        GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        std::wstring s = value;
        const auto start = s.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos) return L"";
        const auto end = s.find_last_not_of(L" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    bool TryReadTransparency(HWND edit, int& transparency) {
        const std::wstring value = ReadFieldText(edit);
        if (value.empty()) return false;
        wchar_t* end = nullptr;
        errno = 0;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str() || *end != L'\0' ||
            parsed < 0 || parsed > 90) {
            return false;
        }
        transparency = static_cast<int>(parsed);
        return IsValidBackgroundTransparency(transparency);
    }

    bool TryReadScale(HWND edit, int& scalePercent) {
        const std::wstring value = ReadFieldText(edit);
        if (value.empty()) return false;
        wchar_t* end = nullptr;
        errno = 0;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str() || *end != L'\0' ||
            parsed < MIN_USER_SCALE_PERCENT || parsed > MAX_USER_SCALE_PERCENT ||
            (parsed % USER_SCALE_PERCENT_STEP) != 0) {
            return false;
        }
        scalePercent = static_cast<int>(parsed);
        return true;
    }

    bool RenderDialog(HDC hdc, const RECT& clientRect, AppearanceDialogState* state) {
        if (!hdc || !EnsureRenderTarget(state)) return false;
        RECT bound = clientRect;
        if (FAILED(state->renderTarget->BindDC(hdc, &bound))) return false;

        const float width = static_cast<float>(clientRect.right);
        const float height = static_cast<float>(clientRect.bottom);
        const float scale = state->dpiScale;
        const float hairline = (std::max)(1.0f, std::round(scale));
        state->renderTarget->BeginDraw();
        state->renderTarget->SetDpi(96.0f, 96.0f);
        state->brush->SetColor(state->palette.surface);
        state->renderTarget->FillRectangle(D2D1::RectF(0, 0, width, height), state->brush.Get());

        state->titleFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->bodyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        state->hintFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        state->hintFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        DrawText(
            state, L"编辑个性外观", state->titleFont.Get(),
            D2D1::RectF(
                static_cast<float>(Scale(16.0f, scale)),
                static_cast<float>(Scale(8.0f, scale)),
                width,
                static_cast<float>(Scale(42.0f, scale))),
            state->palette.text);
        const float dividerPadX = static_cast<float>(Scale(12.0f, scale));
        const float dividerTop = static_cast<float>(Scale(44.0f, scale));
        state->brush->SetColor(state->palette.menuDivider);
        state->renderTarget->FillRectangle(
            D2D1::RectF(dividerPadX, dividerTop, width - dividerPadX, dividerTop + hairline),
            state->brush.Get());

        const float labelW = static_cast<float>(Scale(108.0f, scale));
        const float leftColX = static_cast<float>(Scale(18.0f, scale));
        const float rightColX = static_cast<float>(Scale(386.0f, scale));

        for (size_t index = 0; index < kColorCount; ++index) {
            ColorField& field = state->fields[index];
            const D2D1_RECT_F outline = D2D1::RectF(
                static_cast<float>(field.outline.left),
                static_cast<float>(field.outline.top),
                static_cast<float>(field.outline.right),
                static_cast<float>(field.outline.bottom));
            const int column = index < 10 ? 0 : 1;
            const float labelLeft = column == 0 ? leftColX : rightColX;
            DrawText(
                state, kColorSpecs[index].label, state->bodyFont.Get(),
                D2D1::RectF(
                    labelLeft,
                    static_cast<float>(field.outline.top),
                    labelLeft + labelW,
                    static_cast<float>(field.outline.bottom)),
                state->palette.text);

            const float radius = static_cast<float>(Scale(5.0f, scale));
            state->brush->SetColor(state->palette.trackBg);
            state->renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(outline, radius, radius), state->brush.Get());
            state->brush->SetColor(
                field.invalid
                    ? state->palette.progressRed
                    : (field.focused
                        ? state->palette.progressGreen
                        : state->palette.statsCardBorder));
            DrawCrispRoundedRect(
                state->renderTarget.Get(),
                state->brush.Get(),
                outline,
                radius,
                2.0f);

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
            DrawCrispRoundedRect(
                state->renderTarget.Get(),
                state->brush.Get(),
                swatch,
                radius,
                2.0f);
        }

        DrawText(
            state, L"背景透明度", state->bodyFont.Get(),
            D2D1::RectF(
                rightColX,
                static_cast<float>(state->transparencyOutline.top),
                rightColX + labelW,
                static_cast<float>(state->transparencyOutline.bottom)),
            state->palette.text);
        const D2D1_RECT_F transparencyOutline = D2D1::RectF(
            static_cast<float>(state->transparencyOutline.left),
            static_cast<float>(state->transparencyOutline.top),
            static_cast<float>(state->transparencyOutline.right),
            static_cast<float>(state->transparencyOutline.bottom));
        const float transparencyRadius = static_cast<float>(Scale(5.0f, scale));
        state->brush->SetColor(state->palette.trackBg);
        state->renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(
                transparencyOutline, transparencyRadius, transparencyRadius),
            state->brush.Get());
        state->brush->SetColor(
            state->transparencyInvalid
                ? state->palette.progressRed
                : (state->transparencyFocused
                    ? state->palette.progressGreen
                    : state->palette.statsCardBorder));
        DrawCrispRoundedRect(
            state->renderTarget.Get(),
            state->brush.Get(),
            transparencyOutline,
            transparencyRadius,
            2.0f);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const float percentLeft = static_cast<float>(Scale(660.0f, scale));
        const float percentWidth = static_cast<float>(Scale(36.0f, scale));
        DrawText(
            state, L"%", state->bodyFont.Get(),
            D2D1::RectF(
                percentLeft,
                static_cast<float>(state->transparencyOutline.top),
                percentLeft + percentWidth,
                static_cast<float>(state->transparencyOutline.bottom)),
            state->palette.muted);

        // 缩放大小：放置于背景透明度下，与原本UI（Label/Edit/Percent）严格对齐
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawText(
            state, L"缩放大小", state->bodyFont.Get(),
            D2D1::RectF(
                rightColX,
                static_cast<float>(state->scaleOutline.top),
                rightColX + labelW,
                static_cast<float>(state->scaleOutline.bottom)),
            state->palette.text);
        const D2D1_RECT_F scaleOutline = D2D1::RectF(
            static_cast<float>(state->scaleOutline.left),
            static_cast<float>(state->scaleOutline.top),
            static_cast<float>(state->scaleOutline.right),
            static_cast<float>(state->scaleOutline.bottom));
        const float scaleRadius = static_cast<float>(Scale(5.0f, scale));
        state->brush->SetColor(state->palette.trackBg);
        state->renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(scaleOutline, scaleRadius, scaleRadius),
            state->brush.Get());
        state->brush->SetColor(
            state->scaleInvalid
                ? state->palette.progressRed
                : (state->scaleFocused
                    ? state->palette.progressGreen
                    : state->palette.statsCardBorder));
        DrawCrispRoundedRect(
            state->renderTarget.Get(),
            state->brush.Get(),
            scaleOutline,
            scaleRadius,
            2.0f);
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(
            state, L"%", state->bodyFont.Get(),
            D2D1::RectF(
                percentLeft,
                static_cast<float>(state->scaleOutline.top),
                percentLeft + percentWidth,
                static_cast<float>(state->scaleOutline.bottom)),
            state->palette.muted);

        // 界面字体：重置为居左对齐，与“额度偏低”等左列标签严格平齐
        state->bodyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawText(
            state, L"界面字体", state->bodyFont.Get(),
            D2D1::RectF(
                leftColX,
                static_cast<float>(state->fontOutline.top),
                leftColX + labelW,
                static_cast<float>(state->fontOutline.bottom)),
            state->palette.text);
        const D2D1_RECT_F fontOutline = D2D1::RectF(
            static_cast<float>(state->fontOutline.left),
            static_cast<float>(state->fontOutline.top),
            static_cast<float>(state->fontOutline.right),
            static_cast<float>(state->fontOutline.bottom));
        const float fontRadius = static_cast<float>(Scale(5.0f, scale));
        state->brush->SetColor(state->palette.trackBg);
        state->renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(fontOutline, fontRadius, fontRadius), state->brush.Get());
        state->brush->SetColor(
            state->fontInvalid
                ? state->palette.progressRed
                : (state->fontFocused
                    ? state->palette.progressGreen
                    : state->palette.statsCardBorder));
        DrawCrispRoundedRect(
            state->renderTarget.Get(),
            state->brush.Get(),
            fontOutline,
            fontRadius,
            2.0f);

        DrawButton(state, state->fontBrowseRect, L"浏览", state->hoverButton == 3);

        const wchar_t* hint = state->invalidField >= 0
            ? state->validationMessage.c_str()
            : (state->appliedNotice
                ? L"个性外观与字体已应用"
                : L"透明度 0%–90%；缩放 20%–180%（步进20%）；点击应用生效");
        const float hintRight = static_cast<float>(state->applyRect.left - Scale(8.0f, scale));
        DrawText(
            state, hint, state->hintFont.Get(),
            D2D1::RectF(
                leftColX,
                static_cast<float>(Scale(444.0f, scale)),
                hintRight,
                static_cast<float>(Scale(482.0f, scale))),
            state->invalidField >= 0
                ? state->palette.progressRed
                : (state->appliedNotice
                    ? state->palette.syncSuccess
                    : state->palette.muted));

        DrawButton(state, state->applyRect, L"应用", state->hoverButton == 0);
        DrawButton(state, state->confirmRect, L"确定", state->hoverButton == 1);
        DrawButton(state, state->cancelRect, L"取消", state->hoverButton == 2);

        const float borderStroke = (std::max)(2.0f, static_cast<float>(SnapEven(Scale(1.0f, scale))));
        const float borderInset = borderStroke * 0.5f;
        const float outerCorner = (std::max)(0.0f, state->windowCornerRadius - borderInset);
        state->brush->SetColor(state->palette.statsCardBorder);
        state->renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(borderInset, borderInset, width - borderInset, height - borderInset),
                outerCorner, outerCorner),
            state->brush.Get(), borderStroke);

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

    void CommitDialog(
        HWND hwnd,
        AppearanceDialogState* state,
        bool closeAfterApply)
    {
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
        int transparency = 0;
        state->transparencyInvalid = !TryReadTransparency(
            state->transparencyEdit, transparency);
        if (state->invalidField < 0 && state->transparencyInvalid) {
            state->invalidField = static_cast<int>(kColorCount);
        }

        const std::wstring fontName = ReadFieldText(state->fontEdit);
        state->fontInvalid = fontName.empty() || !CheckFontFamilyExists(state->writeFactory.Get(), fontName);
        if (state->invalidField < 0 && state->fontInvalid) {
            state->invalidField = static_cast<int>(kColorCount) + 1;
        }

        int scalePercent = 100;
        state->scaleInvalid = !TryReadScale(state->scaleEdit, scalePercent);
        if (state->invalidField < 0 && state->scaleInvalid) {
            state->invalidField = static_cast<int>(kColorCount) + 2;
        }

        if (state->invalidField >= 0) {
            HWND edit = nullptr;
            if (state->invalidField < static_cast<int>(kColorCount)) {
                state->validationMessage = L"“";
                state->validationMessage += kColorSpecs[state->invalidField].label;
                state->validationMessage += L"”必须使用 #RRGGBB 格式";
                edit = state->fields[state->invalidField].edit;
            } else if (state->invalidField == static_cast<int>(kColorCount)) {
                state->validationMessage = L"背景透明度必须是 0 至 90 的整数";
                edit = state->transparencyEdit;
            } else if (state->invalidField == static_cast<int>(kColorCount) + 1) {
                if (fontName.empty()) {
                    state->validationMessage = L"界面字体名称不能为空";
                } else {
                    state->validationMessage = L"未找到字体族“" + fontName + L"”，请确认已安装";
                }
                edit = state->fontEdit;
            } else {
                state->validationMessage = L"缩放大小必须是 20 至 180 且为 20 倍数的整数";
                edit = state->scaleEdit;
            }
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (!state->applyCallback ||
            !state->applyCallback(colors, transparency, fontName, scalePercent)) {
            return;
        }

        state->currentFontFamily = fontName;
        if (UpdateDialogFonts(*state, state->dpiScale)) {
            LayoutAppearanceDialog(hwnd, *state);
        }

        AppearanceSettings previewAppearance;
        previewAppearance.colors = colors;
        state->palette = ThemePalette::Custom(previewAppearance);
        HBRUSH replacementBrush = CreateSolidBrush(ToColorRef(state->palette.trackBg));
        if (replacementBrush) {
            DeleteObject(state->editBrush);
            state->editBrush = replacementBrush;
        }
        state->brush.Reset();
        state->renderTarget.Reset();
        state->appliedNotice = true;
        RedrawWindow(
            hwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        if (closeAfterApply) CloseDialog(hwnd, state, true);
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
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (!state->editFont || !suggested || LOWORD(wParam) == 0) return 0;
            const float monitorScale = static_cast<float>(LOWORD(wParam)) / BASE_DPI;
            const HMONITOR monitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
            const float scale = FitScaleToMonitor(monitor, monitorScale);
            if (scale != state->dpiScale && !UpdateDialogFonts(*state, scale)) return 0;
            state->windowCornerRadius = WindowOuterCornerRadius(monitorScale);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         Scale(kDialogWidth, scale), Scale(kDialogHeight, scale),
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
            LayoutAppearanceDialog(hwnd, *state);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
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
                    state->appliedNotice = false;
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
                    if (state->invalidField < 0 && state->transparencyInvalid) {
                        state->invalidField = static_cast<int>(kColorCount);
                        state->validationMessage =
                            L"背景透明度必须是 0 至 90 的整数";
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (controlId == kTransparencyEditControlId) {
                if (HIWORD(wParam) == EN_SETFOCUS) {
                    state->transparencyFocused = true;
                }
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                    state->transparencyFocused = false;
                }
                if (HIWORD(wParam) == EN_CHANGE) {
                    state->transparencyInvalid = false;
                    state->appliedNotice = false;
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
            if (controlId == kScaleEditControlId) {
                if (HIWORD(wParam) == EN_SETFOCUS) {
                    state->scaleFocused = true;
                }
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                    state->scaleFocused = false;
                }
                if (HIWORD(wParam) == EN_CHANGE) {
                    state->scaleInvalid = false;
                    state->appliedNotice = false;
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
            if (controlId == kFontEditControlId) {
                if (HIWORD(wParam) == EN_SETFOCUS) {
                    state->fontFocused = true;
                }
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                    state->fontFocused = false;
                }
                if (HIWORD(wParam) == EN_CHANGE) {
                    const std::wstring fontName = ReadFieldText(state->fontEdit);
                    state->fontInvalid = fontName.empty() || !CheckFontFamilyExists(state->writeFactory.Get(), fontName);
                    state->appliedNotice = false;
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
                    if (state->invalidField < 0 && state->transparencyInvalid) {
                        state->invalidField = static_cast<int>(kColorCount);
                        state->validationMessage =
                            L"背景透明度必须是 0 至 90 的整数";
                    }
                    if (state->invalidField < 0 && state->fontInvalid) {
                        state->invalidField = static_cast<int>(kColorCount) + 1;
                        if (fontName.empty()) {
                            state->validationMessage = L"界面字体名称不能为空";
                        } else {
                            state->validationMessage =
                                L"未找到字体族“" + fontName + L"”，请确认已安装";
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
            if (PtInRect(&state->applyRect, point)) hover = 0;
            else if (PtInRect(&state->confirmRect, point)) hover = 1;
            else if (PtInRect(&state->cancelRect, point)) hover = 2;
            else if (PtInRect(&state->fontBrowseRect, point)) hover = 3;
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
            if (PtInRect(&state->applyRect, point)) {
                CommitDialog(hwnd, state, false);
            } else if (PtInRect(&state->confirmRect, point)) {
                CommitDialog(hwnd, state, true);
            } else if (PtInRect(&state->cancelRect, point)) {
                CloseDialog(hwnd, state, false);
            } else if (PtInRect(&state->fontBrowseRect, point)) {
                LOGFONTW lf = {};
                const std::wstring currentName = ReadFieldText(state->fontEdit);
                wcsncpy_s(lf.lfFaceName, currentName.c_str(), _TRUNCATE);
                lf.lfCharSet = DEFAULT_CHARSET;
                CHOOSEFONTW cf = { sizeof(CHOOSEFONTW) };
                cf.hwndOwner = hwnd;
                cf.lpLogFont = &lf;
                cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
                if (ChooseFontW(&cf)) {
                    SetWindowTextW(state->fontEdit, lf.lfFaceName);
                    state->fontInvalid = false;
                    state->appliedNotice = false;
                    state->invalidField = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_CQB_APPEARANCE_CONFIRM:
            CommitDialog(hwnd, state, true);
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

    bool InitializeDrawing(AppearanceDialogState& state) {
        D2D1_FACTORY_OPTIONS options = {};
        return SUCCEEDED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory), &options, &state.d2dFactory)) &&
            SUCCEEDED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), &state.writeFactory));
    }

    bool UpdateDialogFonts(AppearanceDialogState& state, float scale) {
        const wchar_t* family = state.currentFontFamily.c_str();
        const auto createFormat = [&](DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** format) {
            return SUCCEEDED(state.writeFactory->CreateTextFormat(
                family, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                std::round(size * scale), L"zh-cn", format));
        };
        ComPtr<IDWriteTextFormat> title, body, hint;
        if (!createFormat(DWRITE_FONT_WEIGHT_BOLD, 16.0f, title.GetAddressOf()) ||
            !createFormat(DWRITE_FONT_WEIGHT_REGULAR, 14.0f, body.GetAddressOf()) ||
            !createFormat(DWRITE_FONT_WEIGHT_REGULAR, 12.0f, hint.GetAddressOf())) return false;
        HFONT font = CreateFontW(
            -Scale(14.0f, scale), 0, 0, 0, FW_REGULAR,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
        if (!font) return false;

        title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        hint->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        state.titleFont = std::move(title);
        state.bodyFont = std::move(body);
        state.hintFont = std::move(hint);
        state.dpiScale = scale;

        HFONT previous = std::exchange(state.editFont, font);
        const auto setFont = [font](HWND edit) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        };
        for (const auto& field : state.fields) setFont(field.edit);
        setFont(state.transparencyEdit);
        setFont(state.scaleEdit);
        setFont(state.fontEdit);
        if (previous) DeleteObject(previous);
        return true;
    }

    float FitScaleToMonitor(HMONITOR monitor, float requestedScale) {
        MONITORINFO info = { sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info)) return requestedScale;
        const float workWidth = static_cast<float>(info.rcWork.right - info.rcWork.left);
        const float workHeight = static_cast<float>(info.rcWork.bottom - info.rcWork.top);
        return (std::min)({ requestedScale, workWidth / (kDialogWidth + 12.0f),
                            workHeight / (kDialogHeight + 12.0f) });
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

    bool ShowAppearanceDialog(
        HWND owner,
        float dpiScale,
        const std::map<std::wstring, std::wstring>& currentColors,
        int currentBackgroundTransparency,
        const std::wstring& currentFontFamily,
        int currentUserScalePercent,
        const ThemePalette& palette,
        const AppearanceApplyCallback& applyCallback)
    {
        AppearanceDialogState state;
        state.dpiScale = FitScaleToMonitor(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), dpiScale);
        state.palette = palette;
        state.applyCallback = applyCallback;
        state.currentFontFamily = currentFontFamily;
        if (!InitializeDrawing(state)) return false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(WNDCLASSEXW);
            windowClass.style = 0;
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
        const int width = Scale(kDialogWidth, scale);
        const int height = Scale(kDialogHeight, scale);
        const POINT position = CenterAndClamp(owner, width, height);

        HWND dialog = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
            L"Codex-Quota-Bar_Appearance", L"编辑个性外观", WS_POPUP,
            position.x, position.y, width, height,
            owner, nullptr, GetModuleHandleW(nullptr), &state);
        if (!dialog) return false;

        UINT dialogDpi = GetDpiForWindow(dialog);
        if (dialogDpi == 0) dialogDpi = BASE_DPI;
        state.windowCornerRadius = WindowOuterCornerRadius(
            static_cast<float>(dialogDpi) / static_cast<float>(BASE_DPI), false);
        ApplyPopupWindowEffects(dialog);
        state.editBrush = CreateSolidBrush(ToColorRef(state.palette.trackBg));
        if (!state.editBrush) {
            DestroyWindow(dialog);
            return false;
        }

        const auto createEdit = [&](const wchar_t* value, INT_PTR id, DWORD style, WPARAM limit) {
            const HWND edit = CreateWindowExW(
                0, L"EDIT", value,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL | style,
                0, 0, 0, 0, dialog, reinterpret_cast<HMENU>(id), GetModuleHandleW(nullptr), nullptr);
            if (edit) SendMessageW(edit, EM_SETLIMITTEXT, limit, 0);
            return edit;
        };

        const auto defaults = DefaultAppearanceColors();
        for (size_t index = 0; index < kColorCount; ++index) {
            const auto found = currentColors.find(kColorSpecs[index].key);
            const std::wstring& value = found != currentColors.end()
                ? found->second : defaults.at(kColorSpecs[index].key);
            state.fields[index].edit = createEdit(
                value.c_str(), kEditControlBase + static_cast<INT_PTR>(index), ES_UPPERCASE, 7);
            if (!state.fields[index].edit) {
                DestroyWindow(dialog);
                DeleteObject(state.editBrush);
                return false;
            }
        }
        state.transparencyEdit = createEdit(
            std::to_wstring(currentBackgroundTransparency).c_str(), kTransparencyEditControlId, ES_NUMBER, 2);
        state.scaleEdit = createEdit(
            std::to_wstring(currentUserScalePercent).c_str(), kScaleEditControlId, ES_NUMBER, 3);
        state.fontEdit = createEdit(currentFontFamily.c_str(), kFontEditControlId, 0, 64);
        if (!state.transparencyEdit || !state.scaleEdit || !state.fontEdit || !UpdateDialogFonts(state, scale)) {
            DestroyWindow(dialog);
            DeleteObject(state.editBrush);
            return false;
        }
        LayoutAppearanceDialog(dialog, state);

        state.fontInvalid = currentFontFamily.empty() || !CheckFontFamilyExists(state.writeFactory.Get(), currentFontFamily);

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
        return state.accepted;
    }

} // namespace CodexQuotaBar
