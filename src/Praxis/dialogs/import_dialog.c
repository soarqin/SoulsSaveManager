/**
 * @file import_dialog.c
 * @brief Import save files selection dialog implementation.
 * @details Modal dialog with a checkbox ListView showing scanned save files.
 *          The user selects items and clicks Import to confirm.
 */

#include "import_dialog.h"

#include "../locale.h"
#include "../resource.h"
#include "../theme.h"
#include "../../common/theme_core.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

/* Dialog state stored in DWLP_USER. */
typedef struct import_dialog_state_s {
    const import_scan_result_t *results;
    size_t count;
    bool *out_selected;
} import_dialog_state_t;

static void set_control_text(HWND hwnd, int id, praxis_string_index_t str_id) {
    HWND ctl = GetDlgItem(hwnd, id);
    if (ctl) {
        SetWindowTextW(ctl, praxis_locale_str(str_id));
    }
}

static void init_listview_columns(HWND hlist) {
    LVCOLUMNW col;
    int name_width = 140;
    int path_width = 180;
    int type_width = 90;
    int comp_width = 70;

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;

    col.cx = name_width;
    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_IMPORT_COLUMN_NAME);
    ListView_InsertColumn(hlist, 0, &col);

    col.cx = path_width;
    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_IMPORT_COLUMN_PATH);
    ListView_InsertColumn(hlist, 1, &col);

    col.cx = type_width;
    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_IMPORT_COLUMN_TYPE);
    ListView_InsertColumn(hlist, 2, &col);

    col.cx = comp_width;
    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_IMPORT_COLUMN_COMPRESSED);
    ListView_InsertColumn(hlist, 3, &col);
}

static void populate_listview(HWND hlist, const import_scan_result_t *results, size_t count) {
    LVITEMW item;
    size_t i;

    ListView_SetExtendedListViewStyle(hlist,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

    for (i = 0; i < count; i++) {
        const import_scan_result_t *r = &results[i];
        const wchar_t *type_str;
        const wchar_t *comp_str;
        wchar_t path_ellipsis[MAX_PATH];

        type_str = (r->kind == SAVE_KIND_FULL)
            ? praxis_locale_str(STR_PRAXIS_IMPORT_TYPE_FULL)
            : praxis_locale_str(STR_PRAXIS_IMPORT_TYPE_SLOT);

        comp_str = (r->format == ERSM_FMT_ERSM_CONTAINER)
            ? praxis_locale_str(STR_PRAXIS_IMPORT_YES)
            : praxis_locale_str(STR_PRAXIS_IMPORT_NO);

        lstrcpynW(path_ellipsis, r->full_path, MAX_PATH);

        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)i;
        item.pszText = (LPWSTR)r->file_name;
        ListView_InsertItem(hlist, &item);

        ListView_SetItemText(hlist, (int)i, 1, path_ellipsis);
        ListView_SetItemText(hlist, (int)i, 2, (LPWSTR)type_str);
        ListView_SetItemText(hlist, (int)i, 3, (LPWSTR)comp_str);
    }
}

static void set_all_checks(HWND hlist, size_t count, bool check) {
    size_t i;
    for (i = 0; i < count; i++) {
        ListView_SetCheckState(hlist, (int)i, check ? TRUE : FALSE);
    }
}

static void collect_selections(HWND hlist, size_t count, bool *out_selected) {
    size_t i;
    for (i = 0; i < count; i++) {
        out_selected[i] = ListView_GetCheckState(hlist, (int)i) != 0;
    }
}

static INT_PTR CALLBACK import_dialog_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    import_dialog_state_t *state;

    switch (msg) {
    case WM_INITDIALOG: {
        HWND hlist;
        state = (import_dialog_state_t *)lp;
        if (!state) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)state);

        SetWindowTextW(hwnd, praxis_locale_str(STR_PRAXIS_IMPORT_DIALOG_TITLE));
        set_control_text(hwnd, IDOK, STR_PRAXIS_IMPORT_BTN_IMPORT);
        set_control_text(hwnd, IDCANCEL, STR_PRAXIS_CANCEL);

        hlist = GetDlgItem(hwnd, IDC_IMPORT_LIST);
        if (hlist) {
            init_listview_columns(hlist);
            populate_listview(hlist, state->results, state->count);
        }

        praxis_theme_apply_to_window(hwnd);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_IMPORT_SELECT_ALL: {
            HWND hlist = GetDlgItem(hwnd, IDC_IMPORT_LIST);
            state = (import_dialog_state_t *)GetWindowLongPtrW(hwnd, DWLP_USER);
            if (hlist && state) {
                set_all_checks(hlist, state->count, true);
            }
            return TRUE;
        }
        case IDC_IMPORT_DESELECT_ALL: {
            HWND hlist = GetDlgItem(hwnd, IDC_IMPORT_LIST);
            state = (import_dialog_state_t *)GetWindowLongPtrW(hwnd, DWLP_USER);
            if (hlist && state) {
                set_all_checks(hlist, state->count, false);
            }
            return TRUE;
        }
        case IDOK: {
            HWND hlist = GetDlgItem(hwnd, IDC_IMPORT_LIST);
            state = (import_dialog_state_t *)GetWindowLongPtrW(hwnd, DWLP_USER);
            if (hlist && state) {
                collect_selections(hlist, state->count, state->out_selected);
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HBRUSH br = theme_core_on_ctlcolor((HDC)wp, msg);
        if (br) {
            return (INT_PTR)br;
        }
        break;
    }

    case WM_THEMECHANGED:
        praxis_theme_apply_to_window(hwnd);
        break;
    }

    return FALSE;
}

int dialog_import_show(HWND hwnd, const import_scan_result_t *results, size_t count,
                       bool *out_selected) {
    import_dialog_state_t state;
    state.results = results;
    state.count = count;
    state.out_selected = out_selected;
    return (int)DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_IMPORT_DIALOG),
                                hwnd, import_dialog_proc, (LPARAM)&state);
}
