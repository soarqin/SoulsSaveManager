/**
 * @file edit_game_profile.c
 * @brief Implementation of the Edit Game Profile modal dialog.
 * @details Captures Name, Game, original_save_dir, and tree_root for a game_profile_t.
 *          Browse buttons use file_dialog_open_folder for folder selection. For new
 *          profiles, the Name field is auto-filled with a unique name based on the
 *          selected backend's display_name, and the save_dir Browse picker uses the
 *          backend's auto-detected default directory as the initial folder when the
 *          field is empty. The auto-detected directory is only used as the picker's
 *          initial folder; it is NOT recorded in any persistent "last opened directory"
 *          history (the file_dialog module keeps no such history).
 */

#include "edit_game_profile.h"

#include "../backend_registry.h"
#include "../locale.h"
#include "../resource.h"
#include "../theme.h"
#include "../../common/theme_core.h"
#include "file_dialog.h"

#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

/* Dialog state stored in DWLP_USER. */
typedef struct egp_state_s {
    game_profile_t *gp;
    bool is_new;
    const profile_store_t *store;   /* May be NULL when no store is available. */
    /* Last value auto-written to the Name field. When the user changes the
     * Game dropdown, the Name field is auto-updated only if its current text
     * still matches this snapshot — i.e. the user has not edited it manually. */
    wchar_t last_auto_name[64];
} egp_state_t;

/* Resolve the backend currently selected in the Game combobox. Returns NULL if
 * no backend is selected, the combobox lookup fails, or the registry has no
 * matching entry. */
static const game_backend_t *egp_get_selected_backend(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, IDC_EGP_GAME);
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return NULL;
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (data == CB_ERR) return NULL;
    return backend_registry_get_by_id((game_id_t)data);
}

/* Populate the Game combobox with all registered backends. */
static void egp_populate_games(HWND combo, game_id_t selected) {
    size_t count = backend_registry_count();
    for (size_t i = 0; i < count; i++) {
        const game_backend_t *backend = backend_registry_get_at(i);
        if (!backend || !backend->display_name) continue;

        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)backend->display_name);
        if (idx < 0) continue;
        SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM)backend->id);
        if (selected == backend->id) {
            SendMessageW(combo, CB_SETCURSEL, idx, 0);
        }
    }

    if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

/* Open a folder picker and write the result into the named edit control.
 * For the save_dir field, when the field is empty, the selected backend's
 * auto-detected default save directory (if any) is used as the picker's
 * initial folder. The auto-detected path is consumed in-place only and is
 * NOT persisted to any "last opened directory" cache. */
static void egp_browse_into(HWND hwnd, int edit_id) {
    wchar_t current[MAX_PATH];
    GetDlgItemTextW(hwnd, edit_id, current, MAX_PATH);

    wchar_t auto_dir[MAX_PATH];
    auto_dir[0] = L'\0';
    const wchar_t *initial = current[0] ? current : NULL;
    if (initial == NULL && edit_id == IDC_EGP_SAVE_DIR) {
        const game_backend_t *backend = egp_get_selected_backend(hwnd);
        if (backend && backend->get_default_save_dir) {
            if (backend->get_default_save_dir(auto_dir, MAX_PATH)) {
                initial = auto_dir;
            }
        }
    }

    wchar_t *picked = file_dialog_open_folder(hwnd, initial);
    if (picked) {
        SetDlgItemTextW(hwnd, edit_id, picked);
        CoTaskMemFree(picked);
    }
}

/* Validate user input and copy to gp_inout. Returns false if validation failed. */
static bool egp_commit(HWND hwnd, egp_state_t *state) {
    wchar_t name[64];
    wchar_t save_dir[MAX_PATH];
    wchar_t tree_root[MAX_PATH];

    GetDlgItemTextW(hwnd, IDC_EGP_NAME, name, 64);
    GetDlgItemTextW(hwnd, IDC_EGP_SAVE_DIR, save_dir, MAX_PATH);
    GetDlgItemTextW(hwnd, IDC_EGP_TREE_ROOT, tree_root, MAX_PATH);

    if (name[0] == L'\0' || tree_root[0] == L'\0') {
        MessageBoxW(hwnd, praxis_locale_str(STR_PRAXIS_ERROR),
            praxis_locale_str(STR_PRAXIS_ERROR), MB_OK | MB_ICONERROR);
        return false;
    }

    HWND combo = GetDlgItem(hwnd, IDC_EGP_GAME);
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    game_id_t gid = GAME_ID_ELDEN_RING;
    if (sel != CB_ERR) {
        LRESULT data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
        if (data != CB_ERR) {
            gid = (game_id_t)data;
        }
    }

    lstrcpynW(state->gp->name, name, 64);
    lstrcpynW(state->gp->original_save_dir, save_dir, MAX_PATH);
    lstrcpynW(state->gp->tree_root, tree_root, MAX_PATH);
    state->gp->game_id = gid;
    return true;
}

/* Set the text of a static label to "<localized>:" using the given string index. */
static void egp_set_label(HWND hwnd, int control_id, praxis_string_index_t str_idx) {
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ls:", praxis_locale_str(str_idx));
    SetDlgItemTextW(hwnd, control_id, buf);
}

/* Auto-fill the Name field for a new profile when no name was pre-supplied.
 * Uses the selected backend's display_name as the base; appends " (N)" when
 * the base name is already taken in the store. */
static void egp_auto_fill_name(HWND hwnd, const egp_state_t *state) {
    if (!state || !state->gp || !state->is_new) return;
    if (state->gp->name[0] != L'\0') return;

    const game_backend_t *backend = egp_get_selected_backend(hwnd);
    const wchar_t *base_name = (backend && backend->display_name)
        ? backend->display_name : praxis_locale_str(STR_PRAXIS_PROFILE);

    wchar_t unique_name[64];
    if (profile_store_find_unique_game_name(state->store, base_name, unique_name, 64)) {
        SetDlgItemTextW(hwnd, IDC_EGP_NAME, unique_name);
    }
}

static INT_PTR CALLBACK egp_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    egp_state_t *state = (egp_state_t *)GetWindowLongPtrW(hwnd, DWLP_USER);

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

    case WM_INITDIALOG:
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)lp);
        state = (egp_state_t *)lp;
        if (!state || !state->gp) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }

        SetWindowTextW(hwnd, praxis_locale_str(STR_PRAXIS_GAME_PROFILE));
        SetDlgItemTextW(hwnd, IDOK,     praxis_locale_str(STR_PRAXIS_BTN_OK));
        SetDlgItemTextW(hwnd, IDCANCEL, praxis_locale_str(STR_PRAXIS_BTN_CANCEL));

        /* Localize static labels (.rc embeds English fallback text). */
        egp_set_label(hwnd, IDC_EGP_LBL_NAME,      STR_PRAXIS_PROFILE_NAME);
        egp_set_label(hwnd, IDC_EGP_LBL_GAME,      STR_PRAXIS_PROFILE_GAME);
        egp_set_label(hwnd, IDC_EGP_LBL_SAVE_DIR,  STR_PRAXIS_PROFILE_SAVE_DIR);
        egp_set_label(hwnd, IDC_EGP_LBL_TREE_ROOT, STR_PRAXIS_PROFILE_TREE_ROOT);

        if (!state->is_new) {
            SetDlgItemTextW(hwnd, IDC_EGP_NAME, state->gp->name);
            SetDlgItemTextW(hwnd, IDC_EGP_SAVE_DIR, state->gp->original_save_dir);
            SetDlgItemTextW(hwnd, IDC_EGP_TREE_ROOT, state->gp->tree_root);
        } else {
            /* Honor caller-supplied pre-fill values (e.g. migration wizard). */
            if (state->gp->name[0] != L'\0') {
                SetDlgItemTextW(hwnd, IDC_EGP_NAME, state->gp->name);
            }
            if (state->gp->original_save_dir[0] != L'\0') {
                SetDlgItemTextW(hwnd, IDC_EGP_SAVE_DIR, state->gp->original_save_dir);
            }
            if (state->gp->tree_root[0] != L'\0') {
                SetDlgItemTextW(hwnd, IDC_EGP_TREE_ROOT, state->gp->tree_root);
            }
        }

        egp_populate_games(GetDlgItem(hwnd, IDC_EGP_GAME), state->gp->game_id);

        /* Auto-fill empty Name field with a unique name derived from the selected
         * backend. Must run AFTER the combobox is populated and selected. */
        egp_auto_fill_name(hwnd, state);

        /* Snapshot the auto-generated Name so later CBN_SELCHANGE on the Game
         * dropdown can detect whether the user has edited the field manually. */
        GetDlgItemTextW(hwnd, IDC_EGP_NAME, state->last_auto_name, 64);

        SendMessageW(GetDlgItem(hwnd, IDC_EGP_NAME), EM_LIMITTEXT, 63, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_EGP_SAVE_DIR), EM_LIMITTEXT, MAX_PATH - 1, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_EGP_TREE_ROOT), EM_LIMITTEXT, MAX_PATH - 1, 0);
        praxis_theme_apply_to_window(hwnd);
        return TRUE;

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
        /* Auto-refresh the Name field when the user changes the Game dropdown,
         * but only for new profiles and only when the field still matches the
         * last auto-generated value (i.e. the user has not edited it). */
        if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_EGP_GAME) {
            if (state && state->is_new) {
                wchar_t current_name[64];
                GetDlgItemTextW(hwnd, IDC_EGP_NAME, current_name, 64);
                if (lstrcmpW(current_name, state->last_auto_name) == 0) {
                    const game_backend_t *backend = egp_get_selected_backend(hwnd);
                    const wchar_t *base = (backend && backend->display_name)
                        ? backend->display_name
                        : praxis_locale_str(STR_PRAXIS_PROFILE);
                    wchar_t new_name[64];
                    if (profile_store_find_unique_game_name(state->store, base, new_name, 64)) {
                        SetDlgItemTextW(hwnd, IDC_EGP_NAME, new_name);
                        lstrcpynW(state->last_auto_name, new_name, 64);
                    }
                }
            }
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_EGP_BROWSE_SAVE:
            egp_browse_into(hwnd, IDC_EGP_SAVE_DIR);
            return TRUE;
        case IDC_EGP_BROWSE_TREE:
            egp_browse_into(hwnd, IDC_EGP_TREE_ROOT);
            return TRUE;
        case IDOK:
            if (state && egp_commit(hwnd, state)) {
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

INT_PTR dialog_edit_game_profile_show(HWND parent,
                                      const profile_store_t *store,
                                      game_profile_t *gp_inout,
                                      bool is_new) {
    egp_state_t state;

    if (!gp_inout) {
        return IDCANCEL;
    }

    state.gp = gp_inout;
    state.is_new = is_new;
    state.store = store;

    return DialogBoxParamW(GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDD_EDIT_GAME_PROFILE),
        parent, egp_dlg_proc, (LPARAM)&state);
}
