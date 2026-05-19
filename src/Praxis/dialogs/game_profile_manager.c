/**
 * @file game_profile_manager.c
 * @brief Implementation of the Game Profile Manager modal dialog.
 * @details Provides ListView display of game profiles plus Add/Edit/Delete/Close buttons.
 *          Sort, filter, and drag-reorder are intentionally NOT supported (Phase 2 guardrail).
 */

#include "game_profile_manager.h"
#include "edit_game_profile.h"

#include "../backend_registry.h"
#include "../locale.h"
#include "../profile_store.h"
#include "../profile_store_io.h"
#include "../resource.h"
#include "../theme.h"
#include "../../common/theme_core.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

/* Subclass ID for the listview installed by this dialog. Distinct from
 * theme_core's listview subclass so the two coexist in the chain. */
#define GPM_LISTVIEW_SUBCLASS_ID 0xC0DE0001U

/* Dialog state pointer stored in DWLP_USER. */
typedef struct gpm_state_s {
    profile_store_t *store;
    const wchar_t *ini_path;
} gpm_state_t;

/* Forward declaration: defined further down so it can call gpm_refresh_list. */
static LRESULT CALLBACK gpm_listview_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp, UINT_PTR uid, DWORD_PTR ref);

/* Truncate @p src so it fits column @p col of @p list using middle (path-style)
 * ellipsis, e.g. "C:\\Users\\Foo\\...\\path\\to\\file.txt". The result is
 * written to @p dst (capacity @p dst_chars wide chars). When the source path
 * already fits, or when the column is too narrow to compact even with a
 * middle ellipsis, @p dst contains a usable string (the original or whatever
 * PathCompactPathW could produce) and the listview's default end-ellipsis
 * gracefully clamps the rest. */
static void gpm_truncate_path_for_column(HWND list, int col,
        const wchar_t *src, wchar_t *dst, size_t dst_chars) {
    HDC hdc;
    HFONT font;
    HFONT old_font = NULL;
    int col_width;
    /* Listview cell text padding: ~6px on each side in the default style. */
    const int padding_px = 12;

    if (!dst || dst_chars == 0) return;
    if (!src) {
        dst[0] = L'\0';
        return;
    }
    /* Default: full copy. PathCompactPathW overwrites with a shorter form
     * if (and only if) truncation is needed and possible. */
    lstrcpynW(dst, src, (int)dst_chars);

    if (!list) return;
    col_width = ListView_GetColumnWidth(list, col);
    if (col_width <= padding_px) return;

    hdc = GetDC(list);
    if (!hdc) return;
    font = (HFONT)SendMessageW(list, WM_GETFONT, 0, 0);
    if (font) old_font = (HFONT)SelectObject(hdc, font);

    PathCompactPathW(hdc, dst, (UINT)(col_width - padding_px));

    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(list, hdc);
}

/* Look up the display name for a game_id_t via the backend registry. */
static const wchar_t *game_id_display_name(game_id_t gid) {
    const game_backend_t *backend = backend_registry_get_by_id(gid);
    if (backend && backend->display_name) {
        return backend->display_name;
    }
    return praxis_locale_str(STR_PRAXIS_UNKNOWN);
}

/* Initialize ListView columns once on dialog creation. The two path columns
 * (Save Dir / Tree Root) absorb the remaining width so the four columns fill
 * the listview client area without leaving empty space. */
static void gpm_init_columns(HWND list) {
    LVCOLUMNW col;
    RECT rc;

    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_PROFILE_NAME);
    col.cx = 100;
    col.iSubItem = 0;
    ListView_InsertColumn(list, 0, &col);

    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_PROFILE_GAME);
    col.cx = 80;
    col.iSubItem = 1;
    ListView_InsertColumn(list, 1, &col);

    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_PROFILE_SAVE_DIR);
    col.cx = 80;
    col.iSubItem = 2;
    ListView_InsertColumn(list, 2, &col);

    col.pszText = (LPWSTR)praxis_locale_str(STR_PRAXIS_PROFILE_TREE_ROOT);
    col.cx = 80;
    col.iSubItem = 3;
    ListView_InsertColumn(list, 3, &col);

    /* Distribute the listview client width across the four columns:
     *   Name = 22%, Game = 14%, the two path columns split the remainder.
     * A 2px reserve keeps subpixel rounding from triggering a horizontal
     * scrollbar. The minimums protect very small DPI / dialog sizes from
     * collapsing a column to zero width. */
    if (GetClientRect(list, &rc)) {
        int total_w = rc.right - rc.left;
        if (total_w > 4) total_w -= 2;
        if (total_w > 0) {
            int name_w = total_w * 22 / 100;
            int game_w = total_w * 14 / 100;
            int dir_w  = (total_w - name_w - game_w) / 2;
            if (name_w < 80)  name_w = 80;
            if (game_w < 80)  game_w = 80;
            if (dir_w  < 100) dir_w  = 100;
            ListView_SetColumnWidth(list, 0, name_w);
            ListView_SetColumnWidth(list, 1, game_w);
            ListView_SetColumnWidth(list, 2, dir_w);
            ListView_SetColumnWidth(list, 3, dir_w);
        }
    }
}

/* Repopulate the ListView from the current store contents. Path columns
 * are pre-truncated with middle ellipsis to fit the current column widths;
 * the listview's column-resize subclass calls back into this function so
 * the displayed truncation tracks subsequent column drags. */
static void gpm_refresh_list(HWND list, const profile_store_t *store) {
    ListView_DeleteAllItems(list);

    for (size_t i = 0; i < store->game_count; i++) {
        const game_profile_t *gp = &store->games[i];
        LVITEMW item;
        wchar_t save_buf[MAX_PATH];
        wchar_t tree_buf[MAX_PATH];

        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = (LPWSTR)gp->name;
        item.lParam = (LPARAM)gp->id;
        int idx = ListView_InsertItem(list, &item);
        if (idx < 0) {
            continue;
        }

        ListView_SetItemText(list, idx, 1, (LPWSTR)game_id_display_name(gp->game_id));

        gpm_truncate_path_for_column(list, 2, gp->original_save_dir, save_buf, MAX_PATH);
        gpm_truncate_path_for_column(list, 3, gp->tree_root,         tree_buf, MAX_PATH);
        ListView_SetItemText(list, idx, 2, save_buf);
        ListView_SetItemText(list, idx, 3, tree_buf);
    }
}

/* Get the game_profile_t.id stored in the selected ListView row, or 0 if none. */
static int gpm_selected_game_id(HWND list) {
    int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel < 0) {
        return 0;
    }

    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(list, &item)) {
        return 0;
    }
    return (int)item.lParam;
}

/* Find a game profile by ID. Returns NULL if not found. */
static game_profile_t *gpm_find_game_by_id(profile_store_t *store, int id) {
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == id) {
            return &store->games[i];
        }
    }
    return NULL;
}

/* Count backup profiles whose parent_game_id matches the given id. */
static size_t gpm_count_children(const profile_store_t *store, int game_id) {
    size_t n = 0;
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].parent_game_id == game_id) {
            n++;
        }
    }
    return n;
}

/* Persist the store to disk via profile_store_io_save. */
static void gpm_persist(const profile_store_t *store, const wchar_t *ini_path) {
    if (ini_path && ini_path[0] != L'\0') {
        profile_store_io_save(store, ini_path);
    }
}

/* Handle the Add button: open dialog_edit_game_profile_show in create mode. */
static void gpm_handle_add(HWND hwnd, gpm_state_t *state) {
    game_profile_t gp;

    ZeroMemory(&gp, sizeof(gp));
    gp.game_id = GAME_ID_ELDEN_RING;

    if (dialog_edit_game_profile_show(hwnd, state->store, &gp, true) == IDOK) {
        if (profile_store_add_game(state->store, &gp) > 0) {
            gpm_persist(state->store, state->ini_path);
            gpm_refresh_list(GetDlgItem(hwnd, IDC_GPM_LIST), state->store);
        } else {
            MessageBoxW(hwnd, praxis_locale_str(STR_PRAXIS_ERROR),
                praxis_locale_str(STR_PRAXIS_ERROR), MB_OK | MB_ICONERROR);
        }
    }
}

/* Handle the Edit button: open dialog_edit_game_profile_show in edit mode. */
static void gpm_handle_edit(HWND hwnd, gpm_state_t *state) {
    HWND list = GetDlgItem(hwnd, IDC_GPM_LIST);
    int id = gpm_selected_game_id(list);
    if (id == 0) {
        return;
    }

    game_profile_t *gp = gpm_find_game_by_id(state->store, id);
    if (!gp) {
        return;
    }

    game_profile_t copy = *gp;
    if (dialog_edit_game_profile_show(hwnd, state->store, &copy, false) == IDOK) {
        if (profile_store_update_game(state->store, id, &copy)) {
            gpm_persist(state->store, state->ini_path);
            gpm_refresh_list(list, state->store);
        }
    }
}

/* Handle the Delete button: confirm cascade and then delete. */
static void gpm_handle_delete(HWND hwnd, gpm_state_t *state) {
    HWND list = GetDlgItem(hwnd, IDC_GPM_LIST);
    int id = gpm_selected_game_id(list);
    if (id == 0) {
        return;
    }

    game_profile_t *gp = gpm_find_game_by_id(state->store, id);
    if (!gp) {
        return;
    }

    size_t child_count = gpm_count_children(state->store, id);
    wchar_t msg[512];

    _snwprintf(msg, 512, L"%ls\r\n\r\n%ls: %ls\r\n%ls: %zu",
        praxis_locale_str(STR_PRAXIS_CONFIRM_DELETE_GAME),
        praxis_locale_str(STR_PRAXIS_PROFILE_NAME),
        gp->name,
        praxis_locale_str(STR_PRAXIS_BACKUP_PROFILE),
        child_count);
    msg[511] = L'\0';

    int rc = MessageBoxW(hwnd, msg, praxis_locale_str(STR_PRAXIS_CONFIRM),
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (rc != IDYES) {
        return;
    }

    if (profile_store_delete_game(state->store, id)) {
        gpm_persist(state->store, state->ini_path);
        gpm_refresh_list(list, state->store);
    }
}

/* Listview subclass that re-truncates the path columns whenever the user
 * resizes a column (drag end or divider double-click). The state pointer
 * is passed via DWORD_PTR ref at SetWindowSubclass time; it remains valid
 * for the entire dialog lifetime since DialogBoxParamW blocks until the
 * dialog ends. */
static LRESULT CALLBACK gpm_listview_subclass(HWND hwnd, UINT msg, WPARAM wp,
                                              LPARAM lp, UINT_PTR uid, DWORD_PTR ref) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, gpm_listview_subclass, uid);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_NOTIFY) {
        NMHDR *nmh = (NMHDR *)lp;
        if (nmh) {
            HWND header = (HWND)SendMessageW(hwnd, LVM_GETHEADER, 0, 0);
            if (header && nmh->hwndFrom == header &&
                (nmh->code == HDN_ENDTRACK || nmh->code == HDN_DIVIDERDBLCLICK)) {
                /* Let the listview default handler commit the new column
                 * widths first, then refresh items so the path-column
                 * middle-ellipsis reflects the updated widths. */
                LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                gpm_state_t *state = (gpm_state_t *)ref;
                if (state && state->store) {
                    gpm_refresh_list(hwnd, state->store);
                }
                return r;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK gpm_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    gpm_state_t *state = (gpm_state_t *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg) {
    /* Theme: paint dialog body and child controls in dark colors. */
    case WM_ERASEBKGND:
        if (theme_core_on_erasebkgnd(hwnd, (HDC)wp)) {
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 1);
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        INT_PTR br = theme_core_dlg_ctlcolor((HDC)wp, msg);
        if (br) {
            return br;
        }
        return FALSE;
    }

    case WM_INITDIALOG: {
        HWND list;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)lp);
        state = (gpm_state_t *)lp;
        SetWindowTextW(hwnd, praxis_locale_str(STR_PRAXIS_MANAGE_GAME_PROFILES));
        SetDlgItemTextW(hwnd, IDC_GPM_ADD,    praxis_locale_str(STR_PRAXIS_BTN_ADD));
        SetDlgItemTextW(hwnd, IDC_GPM_EDIT,   praxis_locale_str(STR_PRAXIS_BTN_EDIT));
        SetDlgItemTextW(hwnd, IDC_GPM_DELETE, praxis_locale_str(STR_PRAXIS_BTN_DELETE));
        SetDlgItemTextW(hwnd, IDC_GPM_CLOSE,  praxis_locale_str(STR_PRAXIS_BTN_CLOSE));
        list = GetDlgItem(hwnd, IDC_GPM_LIST);
        gpm_init_columns(list);
        if (state) {
            gpm_refresh_list(list, state->store);
        }
        praxis_theme_apply_to_window(hwnd);
        /* Install column-resize subclass AFTER theme apply so this subclass
         * sees WM_NOTIFY messages first; theme_core's listview subclass
         * doesn't intercept HDN_* so the order doesn't strictly matter, but
         * being on top keeps the refresh hook closest to the source. */
        if (state && list) {
            SetWindowSubclass(list, gpm_listview_subclass,
                              GPM_LISTVIEW_SUBCLASS_ID, (DWORD_PTR)state);
        }
        return TRUE;
    }

    case WM_SETTINGCHANGE:
        if (theme_core_on_setting_change(wp, lp)) {
            praxis_theme_apply_to_window(hwnd);
        }
        return FALSE;

    case WM_SYSCOLORCHANGE:
        if (theme_core_on_syscolor_change()) {
            praxis_theme_apply_to_window(hwnd);
        }
        return FALSE;

    case WM_THEMECHANGED:
        praxis_theme_apply_to_window(hwnd);
        return FALSE;

    case WM_COMMAND:
        if (!state) {
            return FALSE;
        }
        switch (LOWORD(wp)) {
        case IDC_GPM_ADD:
            gpm_handle_add(hwnd, state);
            return TRUE;
        case IDC_GPM_EDIT:
            gpm_handle_edit(hwnd, state);
            return TRUE;
        case IDC_GPM_DELETE:
            gpm_handle_delete(hwnd, state);
            return TRUE;
        case IDC_GPM_CLOSE:
        case IDOK:
        case IDCANCEL:
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        return FALSE;

    case WM_NOTIFY: {
            LPNMHDR nmh = (LPNMHDR)lp;
            if (nmh && nmh->idFrom == IDC_GPM_LIST && nmh->code == NM_CUSTOMDRAW) {
                LRESULT r = theme_core_on_listview_customdraw((LPNMLVCUSTOMDRAW)lp);
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, r);
                return TRUE;
            }
            if (nmh && nmh->idFrom == IDC_GPM_LIST && nmh->code == NM_DBLCLK && state) {
                gpm_handle_edit(hwnd, state);
                return TRUE;
            }
        }
        return FALSE;

    case WM_CLOSE:
        EndDialog(hwnd, IDOK);
        return TRUE;
    }

    return FALSE;
}

INT_PTR dialog_game_profile_manager_show(HWND parent, profile_store_t *store, const wchar_t *ini_path) {
    gpm_state_t state;

    if (!store) {
        return IDCANCEL;
    }

    state.store = store;
    state.ini_path = ini_path;

    return DialogBoxParamW(GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_GAME_PROFILE_MANAGER),
        parent, gpm_dlg_proc, (LPARAM)&state);
}
