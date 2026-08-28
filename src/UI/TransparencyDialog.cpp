#include "UI/TransparencyDialog.h"

#include "Core/Appearance.h"
#include "resources/resource.h"

#include <cerrno>
#include <cwchar>
#include <iterator>
#include <string>

namespace CodexQuotaBar {
namespace {

    struct DialogState {
        int currentValue = 0;
        int selectedValue = 0;
    };

    void CenterOnOwner(HWND dialog, HWND owner) {
        RECT dialogRect = {};
        RECT ownerRect = {};
        if (!GetWindowRect(dialog, &dialogRect) ||
            !owner || !GetWindowRect(owner, &ownerRect)) {
            return;
        }

        const int width = dialogRect.right - dialogRect.left;
        const int height = dialogRect.bottom - dialogRect.top;
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
        SetWindowPos(
            dialog, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    INT_PTR CALLBACK TransparencyDialogProc(
        HWND dialog,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        if (message == WM_INITDIALOG) {
            auto* state = reinterpret_cast<DialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            SetDlgItemTextW(
                dialog,
                IDC_TRANSPARENCY_EDIT,
                std::to_wstring(state->currentValue).c_str());
            SendDlgItemMessageW(
                dialog, IDC_TRANSPARENCY_EDIT, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            CenterOnOwner(dialog, GetParent(dialog));
            return TRUE;
        }

        if (message != WM_COMMAND) return FALSE;
        const WORD command = LOWORD(wParam);
        if (command == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (command != IDOK) return FALSE;

        wchar_t buffer[16] = {};
        const int length = GetDlgItemTextW(
            dialog, IDC_TRANSPARENCY_EDIT, buffer, static_cast<int>(std::size(buffer)));
        wchar_t* end = nullptr;
        errno = 0;
        const long value = length > 0 ? std::wcstol(buffer, &end, 10) : -1;
        if (errno != 0 || end == buffer || *end != L'\0' ||
            value < 0 || value > 90 ||
            !IsValidBackgroundTransparency(static_cast<int>(value))) {
            MessageBoxW(
                dialog,
                L"请输入 0 至 90 的整数。",
                L"透明度格式错误",
                MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_TRANSPARENCY_EDIT));
            SendDlgItemMessageW(
                dialog, IDC_TRANSPARENCY_EDIT, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            return TRUE;
        }

        auto* state = reinterpret_cast<DialogState*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        if (!state) return TRUE;
        state->selectedValue = static_cast<int>(value);
        EndDialog(dialog, IDOK);
        return TRUE;
    }

} // namespace

    bool ShowTransparencyDialog(HWND owner, int currentValue, int& selectedValue) {
        DialogState state = { currentValue, currentValue };
        const INT_PTR result = DialogBoxParamW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_TRANSPARENCY_DIALOG),
            owner,
            TransparencyDialogProc,
            reinterpret_cast<LPARAM>(&state));
        if (result != IDOK) return false;
        selectedValue = state.selectedValue;
        return true;
    }

} // namespace CodexQuotaBar
