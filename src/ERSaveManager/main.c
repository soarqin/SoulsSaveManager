#include "resource.h"
#include "version.h"

#include "config.h"
#include "ersave.h"
#include "locale.h"
#include "theme.h"
#include "theme_core.h"

#include "embedded_face_data.h"
#include "face_dialog.h"
#include "file_dialog.h"
#include "ui_controls.h"
#include "save_compress.h"

#include <md5.h>

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>

#define MAIN_WINDOW_CLASS L"ER_SAVE_FACE_MANAGER"

/*** Global variables ***/

/** @brief Global window handle for the main application window */
HWND main_window;

/*** Top-row controls ***/
/** @brief "Change Folder" button — opens folder picker dialog */
HWND button_change_folder;
/** @brief ComboBox showing available Steam save subfolders */
HWND combo_box_save_folder;
/** @brief "Manage Faces" button — opens face data dialog */
HWND button_manage_faces;

/*** Characters panel (left side) ***/
/** @brief Section label above the characters ListView */
HWND label_chars;
/** @brief ListView displaying all 10 character slots */
HWND list_view_chars;
/** @brief "Import" button below the characters ListView */
HWND button_import_char;
/** @brief "Export" button below the characters ListView */
HWND button_export_char;
/** @brief "Rename" button below the characters ListView */
HWND button_rename_char;

/*** Detail panel (right side) — per-character attribute display ***/
/** @brief Group box enclosing the attribute detail panel */
HWND detail_group;
/** @brief Static labels showing attribute names (Vigor, Mind, …) */
HWND detail_stat_labels[STAT_COUNT];
/** @brief Static labels showing attribute values */
HWND detail_stat_values[STAT_COUNT];
/** @brief Static label showing "Runes Held" text */
HWND detail_runes_label;
/** @brief Static label showing runes held value */
HWND detail_runes_value;
/** @brief Static label showing "Deaths" text */
HWND detail_deaths_label;
/** @brief Static label showing death count value */
HWND detail_deaths_value;

/*** Menu handles ***/
HMENU menu_bar = NULL;
/** @brief Dynamically built submenu for built-in NPC face presets */
HMENU embedded_face_data_menu = NULL;

/*** Shared resources ***/
/** @brief Default message font used by all controls */
HFONT default_font;

/*** Application data ***/
/** @brief Currently loaded save file data; NULL when no file is loaded */
er_save_data_t *save_data = NULL;

void update_char_list_view(int item, const er_char_data_t *char_data);
static void update_detail_panel(int slot);

/* add_folders_to_combo_box is defined in ui_controls.c */
extern void add_folders_to_combo_box(void);

bool handle_save_folder_selection(HWND hwnd) {
    /* Get selected Steam ID */
    int index = SendMessageW(combo_box_save_folder, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR) {
        lstrcpyW(config.save_subfolder, L"");
        return false;
    }

    wchar_t save_subfolder[32];
    SendMessageW(combo_box_save_folder, CB_GETLBTEXT, index, (LPARAM)save_subfolder);

    /* Build save file path */
    wchar_t save_path[MAX_PATH];
    lstrcpyW(save_path, config.save_path);
    PathAppendW(save_path, save_subfolder);
    PathAppendW(save_path, L"\\ER0000.sl2");

    /* Load new save data */
    er_save_data_t *new_save_data = er_save_data_load(save_path);
    if (!new_save_data) {
        int idx = SendMessageW(combo_box_save_folder, CB_FINDSTRING, -1, (LPARAM)config.save_subfolder);
        SendMessageW(combo_box_save_folder, CB_SETCURSEL, idx == CB_ERR ? 0 : idx, 0);
        MessageBoxW(hwnd, locale_str(STR_FAILED_LOAD_SAVE), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        return false;
    }

    /* Check if Steam ID matches */
    uint64_t user_id = er_save_get_userid(new_save_data);
    uint64_t folder_steam_user_id = wcstoull(save_subfolder, NULL, 10);
    if (user_id != folder_steam_user_id) {
        if (MessageBoxW(hwnd, locale_str(STR_STEAM_ID_MISMATCH), locale_str(STR_ERROR), MB_YESNO | MB_ICONWARNING) == IDNO) {
            int idx = SendMessageW(combo_box_save_folder, CB_FINDSTRING, -1, (LPARAM)config.save_subfolder);
            SendMessageW(combo_box_save_folder, CB_SETCURSEL, idx == CB_ERR ? 0 : idx, 0);
            return false;
        }
        er_save_resign_userid(new_save_data, folder_steam_user_id);
    }

    /* Free previous save data if exists */
    if (save_data) {
        er_save_data_free(save_data);
    }
    save_data = new_save_data;

    lstrcpyW(config.save_subfolder, save_subfolder);

    /* Clear characters ListView */
    ListView_DeleteAllItems(list_view_chars);

    /* Add characters to characters ListView */
    for (int i = 0; i < 10; i++) {
        const er_char_data_t *char_data = er_char_data_ref(save_data, i);
        wchar_t text[16];
        wsprintfW(text, L"%d", i + 1);
        LVITEMW lvi = {0};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = text;
        lvi.iItem = ListView_InsertItem(list_view_chars, &lvi);
        update_char_list_view(lvi.iItem, char_data);
    }

    return true;
}

static void open_dir_dialog_for_new_save_location(HWND hwnd) {
    PWSTR pszPath = file_dialog_open_folder(hwnd, config.save_path);
    if (!pszPath) {
        return;
    }
    lstrcpyW(config.save_path, pszPath);
    CoTaskMemFree(pszPath);

    add_folders_to_combo_box();
    SendMessageW(combo_box_save_folder, CB_SETCURSEL, 0, 0);
    if (handle_save_folder_selection(hwnd)) {
        save_config();
    }
}

static void on_menu_change_language(int idx) {
    if (idx == get_current_locale() || idx < 0 || idx >= locale_count()) {
        return;
    }

    /* Uncheck current locale menu item */
    CheckMenuItem(menu_bar, IDM_LOCALE_START + get_current_locale(), MF_UNCHECKED);

    /* Locale menu item selected */
    set_current_locale(idx);

    /* Tick menu item */
    CheckMenuItem(menu_bar, IDM_LOCALE_START + idx, MF_CHECKED);

    /* Save new language to config */
    save_config();

    /* Refresh all UI strings for the new locale */
    ui_refresh_language();
}

static void on_menu_change_compression(int id) {
    static const struct { UINT cmd; int level; } map[] = {
        { IDM_COMPRESSION_FAST,   ERSM_LEVEL_FAST   },
        { IDM_COMPRESSION_NORMAL, ERSM_LEVEL_NORMAL  },
        { IDM_COMPRESSION_MAX,    ERSM_LEVEL_MAX     }
    };
    for (int i = 0; i < 3; i++) {
        if ((UINT)id == map[i].cmd) {
            config.compression_level = map[i].level;
            save_config();
            break;
        }
    }
    ui_update_compression_menu();
}

static ersm_format_t detect_import_format(const wchar_t *path) {
    return ersm_detect_file_format(path);
}

void update_char_list_view(int item, const er_char_data_t *char_data) {
    wchar_t text[64];

    if (!char_data) {
        wsprintfW(text, L"%s", locale_str(STR_EMPTY));
        ListView_SetItemText(list_view_chars, item, 1, text);
        return;
    }

    wsprintfW(text, L"%s", er_char_data_get_name(char_data));
    ListView_SetItemText(list_view_chars, item, 1, text);

    er_char_info_t info = {0};
    er_char_data_info(char_data, &info);

    wsprintfW(text, L"%s", locale_str(info.body_type ? STR_TYPE_A : STR_TYPE_B));
    ListView_SetItemText(list_view_chars, item, 2, text);

    wsprintfW(text, L"%d", info.level);
    ListView_SetItemText(list_view_chars, item, 3, text);

    uint32_t in_game_time = info.in_game_time / 1000;
    if (in_game_time < 3600) {
        wsprintfW(text, L"%02d:%02d", in_game_time / 60, in_game_time % 60);
    } else {
        wsprintfW(text, L"%02d:%02d:%02d", in_game_time / 3600, (in_game_time % 3600) / 60, in_game_time % 60);
    }
    ListView_SetItemText(list_view_chars, item, 4, text);
}

/* Update the detail panel with attribute info for the selected character slot */
static void update_detail_panel(int slot) {
    if (slot < 0 || !save_data) {
        /* Clear all values */
        for (int i = 0; i < STAT_COUNT; i++) {
            SetWindowTextW(detail_stat_values[i], L"");
        }
        SetWindowTextW(detail_runes_value, L"");
        SetWindowTextW(detail_deaths_value, L"");
        return;
    }

    const er_char_data_t *char_data = er_char_data_ref(save_data, slot);
    if (!char_data) {
        for (int i = 0; i < STAT_COUNT; i++) {
            SetWindowTextW(detail_stat_values[i], L"-");
        }
        SetWindowTextW(detail_runes_value, L"-");
        SetWindowTextW(detail_deaths_value, L"-");
        return;
    }

    er_char_info_t info = {0};
    er_char_data_info(char_data, &info);

    wchar_t text[16];
    for (int i = 0; i < STAT_COUNT; i++) {
        wsprintfW(text, L"%d", info.stats[i]);
        SetWindowTextW(detail_stat_values[i], text);
    }

    wsprintfW(text, L"%d", info.runes_held);
    SetWindowTextW(detail_runes_value, text);

    wsprintfW(text, L"%d", info.death_count);
    SetWindowTextW(detail_deaths_value, text);
}

static void import_char_from_save_file(HWND hwnd, int item, const wchar_t *path) {
    er_save_simple_data_t *simple_save_data = er_save_simple_data_load(path);
    if (!simple_save_data) {
        return;
    }

    TASKDIALOG_BUTTON buttons[10] = {0};
    int button_count = 0;
    for (int i = 0; i < 10; i++) {
        const wchar_t *name = er_save_simple_data_get_char_name(simple_save_data, i);
        if (name == NULL || name[0] == L'\0') {
            continue;
        }
        buttons[button_count].nButtonID = i;
        buttons[button_count++].pszButtonText = name;
    }

    if (button_count == 0) {
        MessageBoxW(hwnd, locale_str(STR_NO_CHARACTER_FOUND), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        er_save_simple_data_free(simple_save_data);
        return;
    }

    TASKDIALOGCONFIG task_dialog_config;
    ZeroMemory(&task_dialog_config, sizeof(TASKDIALOGCONFIG));
    task_dialog_config.cbSize = sizeof(TASKDIALOGCONFIG);
    task_dialog_config.pszWindowTitle = locale_str(STR_IMPORT_CHARACTER);
    task_dialog_config.pszContent = locale_str(STR_SELECT_CHARACTER_CONTENT);
    task_dialog_config.nDefaultButton = IDCANCEL;
    task_dialog_config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    task_dialog_config.pRadioButtons = buttons;
    task_dialog_config.cRadioButtons = button_count;

    int button_id = 0;
    int radio_id = 0;
    HRESULT hr = TaskDialogIndirect(&task_dialog_config, &button_id, &radio_id, NULL);
    if (!SUCCEEDED(hr)) {
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        er_save_simple_data_free(simple_save_data);
        return;
    }
    if (button_id != IDOK) {
        /* User cancelled — silently return */
        er_save_simple_data_free(simple_save_data);
        return;
    }

    uint8_t *char_data = er_save_simple_data_slot_export(simple_save_data, radio_id);
    if (char_data && er_char_data_import_raw(save_data, item, char_data)) {
        update_char_list_view(item, er_char_data_ref(save_data, item));
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
    }

    er_save_simple_data_slot_free(char_data);
    er_save_simple_data_free(simple_save_data);
}

static void import_char_from_file(HWND hwnd, int item, const wchar_t *path) {
    er_char_data_t *char_data = er_char_data_from_file(path);
    if (char_data && er_char_data_import(save_data, item, char_data)) {
        update_char_list_view(item, char_data);
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
    }
    er_char_data_free(char_data);
}

/* Function to import character data from a file */
static void import_char_data(HWND hwnd, int item) {
    COMDLG_FILTERSPEC rgSpec[] = {
        { locale_str(STR_ALL_FILES), L"*.*" }
    };
    PWSTR pszPath = file_dialog_open(hwnd, locale_str(STR_IMPORT_CHARACTER), rgSpec, 1);
    if (!pszPath) {
        return;
    }

    ersm_format_t fmt = detect_import_format(pszPath);
    if (fmt == ERSM_FMT_ERSM_CONTAINER) {
        EnableWindow(hwnd, FALSE);
        SetCursor(LoadCursor(NULL, IDC_WAIT));
        size_t payload_size = 0;
        uint8_t data_type = 0;
        uint8_t *payload = ersm_decompress_from_file(pszPath, &payload_size, &data_type);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        EnableWindow(hwnd, TRUE);
        if (!payload) {
            MessageBoxW(hwnd, locale_str(STR_DECOMPRESSION_ERROR), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        } else if (data_type == ERSM_TYPE_CHAR_SLOT && payload_size == 0x28024Cu) {
            if (er_char_data_import_raw(save_data, item, payload)) {
                update_char_list_view(item, er_char_data_ref(save_data, item));
                MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, locale_str(STR_CHARACTER_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
            }
            LocalFree(payload);
        } else if (data_type == ERSM_TYPE_FULL_SAVE) {
            LocalFree(payload);
            wchar_t temp_path[MAX_PATH];
            uint8_t dummy_type = 0;
            EnableWindow(hwnd, FALSE);
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            bool ok = ersm_decompress_to_temp_file(pszPath, temp_path, &dummy_type);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            EnableWindow(hwnd, TRUE);
            if (ok) {
                import_char_from_save_file(hwnd, item, temp_path);
                DeleteFileW(temp_path);
            } else {
                MessageBoxW(hwnd, locale_str(STR_DECOMPRESSION_ERROR), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
            }
        } else {
            LocalFree(payload);
            MessageBoxW(hwnd, locale_str(STR_DECOMPRESSION_ERROR), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        }
    } else if (fmt == ERSM_FMT_BND4_RAW) {
        import_char_from_save_file(hwnd, item, pszPath);
    } else {
        import_char_from_file(hwnd, item, pszPath);
    }

    CoTaskMemFree(pszPath);
}

/* Function to export character data to a file */
static void export_char_data(HWND hwnd, int item) {
    COMDLG_FILTERSPEC rgSpec[] = {
        { locale_str(STR_ALL_FILES), L"*.*" }
    };
    PWSTR pszPath = file_dialog_save(hwnd, locale_str(STR_EXPORT_CHARACTER), rgSpec, 1);
    if (!pszPath) {
        return;
    }

    const er_char_data_t *ref = er_char_data_ref(save_data, item);
    if (!ref) {
        CoTaskMemFree(pszPath);
        return;
    }

    uint8_t *buf = LocalAlloc(LMEM_FIXED, 0x28024C);
    if (!buf) {
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_EXPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        CoTaskMemFree(pszPath);
        return;
    }

    if (!er_char_data_serialize(ref, buf, 0x28024C)) {
        LocalFree(buf);
        MessageBoxW(hwnd, locale_str(STR_CHARACTER_EXPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        CoTaskMemFree(pszPath);
        return;
    }

    EnableWindow(hwnd, FALSE);
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    bool ok = ersm_compress_to_file(pszPath, buf, 0x28024C, ERSM_TYPE_CHAR_SLOT, config.compression_level);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    EnableWindow(hwnd, TRUE);
    LocalFree(buf);
    MessageBoxW(hwnd, locale_str(ok ? STR_CHARACTER_EXPORT_SUCCESS : STR_CHARACTER_EXPORT_FAILED),
                locale_str(ok ? STR_SUCCESS : STR_ERROR),
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    CoTaskMemFree(pszPath);
}

static LRESULT CALLBACK rename_char_data_dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        /* Theme: paint dialog body and child controls in dark colors. */
        case WM_ERASEBKGND:
            if (theme_core_on_erasebkgnd(hwnd, (HDC)wparam)) {
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 1);
                return TRUE;
            }
            return FALSE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            INT_PTR br = theme_core_dlg_ctlcolor((HDC)wparam, msg);
            if (br) {
                return br;
            }
            return FALSE;
        }

        case WM_INITDIALOG:
            SetWindowTextW(hwnd, locale_str(STR_RENAME_CHARACTER));
            SetWindowTextW(GetDlgItem(hwnd, IDC_STATIC_CHARACTER_NAME), locale_str(STR_ENTER_NEW_NAME));
            SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_CHARACTER_NAME), (LPWSTR)lparam);
            SetWindowTextW(GetDlgItem(hwnd, IDOK), locale_str(STR_CONFIRM));
            SetWindowTextW(GetDlgItem(hwnd, IDCANCEL), locale_str(STR_CANCEL));
            Edit_LimitText(GetDlgItem(hwnd, IDC_EDIT_CHARACTER_NAME), 16);
            theme_apply_to_window(hwnd);
            return TRUE;
        case WM_SETTINGCHANGE:
            if (theme_core_on_setting_change(wparam, lparam)) {
                theme_apply_to_window(hwnd);
            }
            return FALSE;
        case WM_SYSCOLORCHANGE:
            if (theme_core_on_syscolor_change()) {
                theme_apply_to_window(hwnd);
            }
            return FALSE;
        case WM_THEMECHANGED:
            theme_apply_to_window(hwnd);
            return FALSE;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDOK: {
                    int item = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
                    if (item >= 0) {
                        wchar_t new_name[32];
                        Edit_GetText(GetDlgItem(hwnd, IDC_EDIT_CHARACTER_NAME), new_name, sizeof(new_name) / sizeof(wchar_t));
                        if (er_char_data_set_name(save_data, item, new_name)) {
                            ListView_SetItemText(list_view_chars, item, 1, new_name);
                        } else {
                            MessageBoxW(hwnd, locale_str(STR_RENAME_CHARACTER_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
                        }
                    }
                    EndDialog(hwnd, TRUE);
                    break;
                }
                case IDCANCEL: {
                    EndDialog(hwnd, FALSE);
                    break;
                }
            }
    }
    return FALSE;
}

static void rename_char_data(HWND hwnd, int item) {
    if (item == -1) return;
    const er_char_data_t *char_data = er_char_data_ref(save_data, item);
    if (char_data) {
        DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_RENAME_CHARACTER), hwnd, (DLGPROC)rename_char_data_dialog_proc, (LPARAM)er_char_data_get_name(char_data));
    }
}

/* Function to handle characters ListView popup menu */
static void list_view_chars_popup_menu(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    /* Get the item under the cursor */
    POINT pt;
    pt.x = GET_X_LPARAM(lparam);
    pt.y = GET_Y_LPARAM(lparam);
    ScreenToClient(list_view_chars, &pt);

    /* Get the item under the cursor */
    LVHITTESTINFO lvhti = {0};
    lvhti.pt = pt;
    int item = ListView_HitTest(list_view_chars, &lvhti);

    if (item < 0) {
        return;
    }

    /* Create popup menu */
    HMENU menu = CreatePopupMenu();

    if (menu) {
        /* Add menu items */
        AppendMenuW(menu, MF_BYPOSITION | MF_STRING, IDM_IMPORT_CHAR, locale_str(STR_IMPORT_CHARACTER));
        const er_char_data_t *char_data = er_char_data_ref(save_data, item);
        if (char_data) {
            AppendMenuW(menu, MF_BYPOSITION | MF_STRING, IDM_EXPORT_CHAR, locale_str(STR_EXPORT_CHARACTER));
            AppendMenuW(menu, MF_BYPOSITION | MF_STRING, IDM_RENAME_CHAR, locale_str(STR_RENAME_CHARACTER));
        }

        /* Convert window coordinates back to screen coordinates */
        ClientToScreen(list_view_chars, &pt);
        /* Show menu at cursor position */
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
    }
}

/* Function to handle window resize */
static void on_window_resize(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    /* Get window dimensions */
    int width = LOWORD(lparam);
    int height = HIWORD(lparam);

    ui_layout_controls(hwnd, width, height);
}

/* Window procedure */
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            HMODULE module = GetModuleHandle(NULL);

            /* Create all controls */
            ui_create_controls(hwnd, module);

            /* Apply theme to top-level window (dark titlebar, etc.) */
            theme_core_apply_to_window(hwnd);

            return 0;
        }

        /* Theme: dark backgrounds for all child controls. Falls through to
         * legacy light handler when dark mode is inactive. */
        case WM_CTLCOLORDLG:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HBRUSH br = theme_core_on_ctlcolor((HDC)wparam, msg);
            if (br) {
                return (LRESULT)br;
            }
            /* Legacy light path for STATIC controls (preserves prior look). */
            if (msg == WM_CTLCOLORSTATIC) {
                HDC hdc_static = (HDC)wparam;
                SetBkMode(hdc_static, OPAQUE);
                SetBkColor(hdc_static, GetSysColor(COLOR_WINDOW));
                SetTextColor(hdc_static, GetSysColor(COLOR_WINDOWTEXT));
                return (LRESULT)GetStockObject(WHITE_BRUSH);
            }
            break;
        }

        /* Theme: paint window client background with dark brush in dark mode. */
        case WM_ERASEBKGND:
            if (theme_core_on_erasebkgnd(hwnd, (HDC)wparam)) {
                return 1;
            }
            break;

        /* UAH menu bar dark painting (Win10 1809+). Notepad++ pattern with
         * DrawThemeTextEx. We DO NOT intercept WM_UAHMEASUREMENUITEM:
         * DefWindowProcW must run for that message so menu items get correct
         * widths/heights. Intercepting it without writing valid sizes produces
         * zero-sized items (the menu visually disappears). */
        case WM_UAHDRAWMENU:
            if (theme_core_on_uah_drawmenu(hwnd, lparam)) {
                return 0;
            }
            break;
        case WM_UAHDRAWMENUITEM:
            if (theme_core_on_uah_drawmenuitem(hwnd, lparam)) {
                return 0;
            }
            break;

        /* Paint over the 1px light separator under the menu bar after
         * non-client paint. theme_core_paint_uah_menu_underline is a no-op
         * in light mode. */
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            LRESULT r = DefWindowProcW(hwnd, msg, wparam, lparam);
            theme_core_paint_uah_menu_underline(hwnd);
            return r;
        }

        /* React to system theme, high-contrast, and system color changes. */
        case WM_SETTINGCHANGE: {
            if (theme_core_on_setting_change(wparam, lparam)) {
                theme_core_apply_to_window_and_children(hwnd);
            }
            break;
        }

        case WM_SYSCOLORCHANGE: {
            if (theme_core_on_syscolor_change()) {
                theme_core_apply_to_window_and_children(hwnd);
            }
            break;
        }

        case WM_THEMECHANGED:
            /* User toggled visual styles - re-apply our theme. */
            theme_core_apply_to_window_and_children(hwnd);
            break;

        case WM_SIZE: {
            on_window_resize(hwnd, wparam, lparam);
            return 0;
        }

        case WM_NOTIFY: {
            NMHDR *nmhdr = (NMHDR *)lparam;
            /* Dark theme custom-draw for the characters ListView. */
            if (nmhdr->hwndFrom == list_view_chars && nmhdr->code == NM_CUSTOMDRAW) {
                return theme_core_on_listview_customdraw((LPNMLVCUSTOMDRAW)lparam);
            }
            if (nmhdr->hwndFrom == list_view_chars && nmhdr->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *nmlv = (NMLISTVIEW *)lparam;
                if (nmlv->uChanged & LVIF_STATE) {
                    if (nmlv->uNewState & LVIS_SELECTED) {
                        /* Item selected - update detail panel and button state */
                        update_detail_panel(nmlv->iItem);
                        ui_update_char_buttons();
                    } else if ((nmlv->uOldState & LVIS_SELECTED) && !(nmlv->uNewState & LVIS_SELECTED)) {
                        /* Item deselected - clear detail panel if nothing else selected */
                        int sel = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
                        if (sel == -1) {
                            update_detail_panel(-1);
                        }
                        ui_update_char_buttons();
                    }
                }
            }
            return 0;
        }

        case WM_CONTEXTMENU: {
            if ((HWND)wparam == list_view_chars) {
                list_view_chars_popup_menu(hwnd, wparam, lparam);
            }
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_BUTTON_CHANGE_FOLDER:
                    open_dir_dialog_for_new_save_location(hwnd);
                    break;

                case IDC_BUTTON_MANAGE_FACES:
                    /* Open face data management as modal dialog */
                    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_FACE_DATA), hwnd, (DLGPROC)face_data_dialog_proc, 0);
                    break;

                case IDC_COMBO_SAVE_FOLDER: {
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        if (handle_save_folder_selection(hwnd)) {
                            /* Save new path to config */
                            save_config();
                        }
                    }
                    break;
                }

                case IDC_BUTTON_IMPORT_CHAR:
                case IDM_IMPORT_CHAR: {
                    /* Get selected item */
                    int item = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
                    if (item == -1) return 0;
                    import_char_data(hwnd, item);
                    update_detail_panel(item);
                    ui_update_char_buttons();
                    break;
                }

                case IDC_BUTTON_EXPORT_CHAR:
                case IDM_EXPORT_CHAR: {
                    /* Get selected item */
                    int item = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
                    if (item == -1) return 0;
                    export_char_data(hwnd, item);
                    break;
                }

                case IDC_BUTTON_RENAME_CHAR:
                case IDM_RENAME_CHAR: {
                    /* Get selected item */
                    int item = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
                    if (item == -1) return 0;
                    rename_char_data(hwnd, item);
                    ui_update_char_buttons();
                    break;
                }

                default: {
                    int id = LOWORD(wparam);
                    if (id >= IDM_LOCALE_START && id < IDM_LOCALE_START + 100) {
                        on_menu_change_language(id - IDM_LOCALE_START);
                    } else if (id == IDM_COMPRESSION_FAST || id == IDM_COMPRESSION_NORMAL || id == IDM_COMPRESSION_MAX) {
                        on_menu_change_compression(id);
                    } else if (theme_is_menu_command(id)) {
                        theme_handle_menu_command(id);
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lparam;
            mmi->ptMinTrackSize.x = 600;
            mmi->ptMinTrackSize.y = 400;
            return 0;
        }

        case WM_DESTROY:
            /* Save configuration before exiting */
            save_config();
            DestroyMenu(embedded_face_data_menu);
            ui_cleanup();   /* Release shared UI resources */
            theme_cleanup();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* Function to create the main window */
static HWND create_window(HINSTANCE instance, int cmd_show) {
    /* Register window class */
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    /* Class brush is used in light mode; dark mode is handled by the
     * WM_ERASEBKGND override in wnd_proc. */
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wc);

    wchar_t window_title[128];
    wsprintfW(window_title, L"%s v%s", locale_str(STR_APP_TITLE), VERSION_STR_W);

    /* Create main window with saved position and size if available */
    HWND hwnd;
    if (config.window_x != -1 && config.window_y != -1 && config.window_width > 0 && config.window_height > 0) {
        hwnd = CreateWindowW(
            MAIN_WINDOW_CLASS, window_title, WS_OVERLAPPEDWINDOW,
            config.window_x, config.window_y, config.window_width, config.window_height,
            NULL, NULL, instance, NULL);
    } else {
        /* Use default position and size */
        hwnd = CreateWindowW(
            MAIN_WINDOW_CLASS, window_title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 750, 480,
            NULL, NULL, instance, NULL);
    }

    if (!hwnd)
        return NULL;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    /* Center the window on screen if no saved position */
    if (config.window_x == -1 || config.window_y == -1) {
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int win_w = rc.right - rc.left;
        int win_h = rc.bottom - rc.top;

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);

        SetWindowPos(hwnd, NULL, (screen_w - win_w) / 2, (screen_h - win_h) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    }

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);

    return hwnd;
}

/*** --selftest harness (headless QA) ***/

/* Emit a single wide string to stdout, supporting both console and redirected output. */
static void st_out(const wchar_t *msg, size_t len) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD type = GetFileType(hOut);
    DWORD written;
    if (type == FILE_TYPE_CHAR) {
        WriteConsoleW(hOut, msg, (DWORD)len, &written, NULL);
    } else {
        /* File or pipe: write as UTF-8 */
        int utf8_size = WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, NULL, 0, NULL, NULL);
        if (utf8_size <= 0) {
            return;
        }
        char *utf8 = LocalAlloc(LMEM_FIXED, utf8_size);
        if (!utf8) {
            return;
        }
        WideCharToMultiByte(CP_UTF8, 0, msg, (int)len, utf8, utf8_size, NULL, NULL);
        WriteFile(hOut, utf8, (DWORD)utf8_size, &written, NULL);
        LocalFree(utf8);
    }
}

/* Formatted wide-char printf that honors any stdout redirect set by the parent process. */
static void st_printf(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    if (len < 0) {
        len = 1023;
    }
    buf[len] = L'\0';
    st_out(buf, (size_t)len);
}

/* Fill a buffer with a compressible but non-trivial pattern (LZMA-friendly). */
static void fill_compressible(uint8_t *buf, size_t size) {
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < size; i++) {
        if ((i & 0x3Fu) == 0) {
            s = s * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(s >> 24);
        } else {
            buf[i] = 0;
        }
    }
}

/* Roundtrip test helper: compress + decompress at Fast/Normal/Max and verify byte equality. */
static int selftest_roundtrip(size_t payload_size, uint8_t data_type) {
    static const struct {
        int level;
        const wchar_t *name;
    } levels[] = {
        { ERSM_LEVEL_FAST,   L"Fast"   },
        { ERSM_LEVEL_NORMAL, L"Normal" },
        { ERSM_LEVEL_MAX,    L"Max"    },
    };

    uint8_t *src = LocalAlloc(LMEM_FIXED, payload_size);
    if (!src) {
        st_printf(L"roundtrip FAIL: allocate src\n");
        return 1;
    }
    fill_compressible(src, payload_size);

    wchar_t tmp_dir[MAX_PATH];
    wchar_t tmp_path[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_dir);
    if (!GetTempFileNameW(tmp_dir, L"ers", 0, tmp_path)) {
        LocalFree(src);
        st_printf(L"roundtrip FAIL: tempfile\n");
        return 1;
    }

    int result = 0;
    for (int li = 0; li < 3; li++) {
        if (!ersm_compress_to_file(tmp_path, src, payload_size, data_type, levels[li].level)) {
            st_printf(L"roundtrip FAIL: %s at compress\n", levels[li].name);
            result = 1;
            break;
        }
        size_t out_size = 0;
        uint8_t out_type = 0;
        uint8_t *out = ersm_decompress_from_file(tmp_path, &out_size, &out_type);
        if (!out) {
            st_printf(L"roundtrip FAIL: %s at decompress\n", levels[li].name);
            result = 1;
            break;
        }
        if (out_size != payload_size || out_type != data_type) {
            st_printf(L"roundtrip FAIL: %s size/type mismatch\n", levels[li].name);
            LocalFree(out);
            result = 1;
            break;
        }
        size_t mismatch = (size_t)-1;
        for (size_t i = 0; i < payload_size; i++) {
            if (src[i] != out[i]) {
                mismatch = i;
                break;
            }
        }
        LocalFree(out);
        if (mismatch != (size_t)-1) {
            st_printf(L"roundtrip FAIL: %s at offset %llu\n",
                      levels[li].name, (unsigned long long)mismatch);
            result = 1;
            break;
        }
        st_printf(L"roundtrip OK: %s\n", levels[li].name);
    }

    DeleteFileW(tmp_path);
    LocalFree(src);
    return result;
}

/* Write a 0x28024C-byte compressible buffer as an ERSM char-slot container at Normal level. */
static int selftest_make_valid_ersm(const wchar_t *path) {
    const size_t size = 0x28024Cu;
    uint8_t *buf = LocalAlloc(LMEM_FIXED, size);
    if (!buf) {
        st_printf(L"make-valid-ersm: alloc failed\n");
        return 1;
    }
    fill_compressible(buf, size);
    bool ok = ersm_compress_to_file(path, buf, size, ERSM_TYPE_CHAR_SLOT, ERSM_LEVEL_NORMAL);
    LocalFree(buf);
    if (!ok) {
        st_printf(L"make-valid-ersm: compress failed\n");
        return 1;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fsz;
        if (GetFileSizeEx(h, &fsz)) {
            st_printf(L"wrote %llu bytes\n", (unsigned long long)fsz.QuadPart);
        }
        CloseHandle(h);
    }
    return 0;
}

/* Try to decompress a file and print the resulting size and type byte. */
static int selftest_decompress_file(const wchar_t *path) {
    size_t out_size = 0;
    uint8_t out_type = 0;
    uint8_t *out = ersm_decompress_from_file(path, &out_size, &out_type);
    if (!out) {
        st_printf(L"decompress failed: header rejected\n");
        return 1;
    }
    st_printf(L"decompress OK: size=%llu, type=%u\n",
              (unsigned long long)out_size, (unsigned)out_type);
    LocalFree(out);
    return 0;
}

/* Print the detected container format for a file (ERSM / BND4 / UNKNOWN). */
static int selftest_detect_format(const wchar_t *path) {
    ersm_format_t fmt = ersm_detect_file_format(path);
    if (fmt == ERSM_FMT_ERSM_CONTAINER) {
        st_printf(L"format: ERSM\n");
    } else if (fmt == ERSM_FMT_BND4_RAW) {
        st_printf(L"format: BND4\n");
    } else {
        st_printf(L"format: UNKNOWN\n");
    }
    return 0;
}

/* Write a raw 0x28024C-byte compressible slot (no ERSM wrapper) to path. */
static int selftest_make_legacy_slot(const wchar_t *path) {
    const size_t size = 0x28024Cu;
    uint8_t *buf = LocalAlloc(LMEM_FIXED, size);
    if (!buf) {
        st_printf(L"make-legacy-slot: alloc failed\n");
        return 1;
    }
    fill_compressible(buf, size);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LocalFree(buf);
        st_printf(L"make-legacy-slot: create failed\n");
        return 1;
    }
    DWORD written;
    bool ok = WriteFile(f, buf, (DWORD)size, &written, NULL) && written == size;
    CloseHandle(f);
    LocalFree(buf);
    if (!ok) {
        st_printf(L"make-legacy-slot: write failed\n");
        return 1;
    }
    st_printf(L"wrote %llu bytes\n", (unsigned long long)size);
    return 0;
}

/* Build a structurally valid BND4 save stub (12 slots) with the given user ID. */
static bool make_min_valid_sl2(const wchar_t *path, uint64_t user_id) {
    const uint32_t char_slot_size    = 0x280010u;  /* ER_CHAR_SLOT_FILE_SIZE */
    const uint32_t summary_slot_size = 0x60010u;   /* ER_SUMMARY_SLOT_FILE_SIZE */
    const uint32_t slot0_offset      = 0x300u;     /* ER_FILE_HEADER_SIZE */
    const uint32_t summary_data_size = 0x60000u;   /* ER_SUMMARY_DATA_SIZE */
    const uint32_t face_section_size = 0x11D0u;    /* ER_SUMMARY_FACE_SECTION_SIZE */

    const uint32_t summary_offset = slot0_offset + 10u * char_slot_size;
    const uint32_t index_offset   = summary_offset + summary_slot_size;
    const uint32_t total_size     = index_offset + summary_slot_size;
    const uint32_t summary_layout_size = face_section_size + 0x14u;

    uint8_t *file_data = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    if (!file_data) {
        return false;
    }

    /* BND4 magic */
    CopyMemory(file_data, "BND4", 4);
    /* Slot count = 12 */
    *(uint32_t *)(file_data + 0x0C) = 12u;

    /* Slot size + offset arrays: 10 char slots, 1 summary slot, 1 index slot */
    for (int i = 0; i < 10; i++) {
        *(uint32_t *)(file_data + 0x48 + i * 0x20) = char_slot_size;
        *(uint32_t *)(file_data + 0x50 + i * 0x20) = slot0_offset + (uint32_t)i * char_slot_size;
    }
    *(uint32_t *)(file_data + 0x48 + 10 * 0x20) = summary_slot_size;
    *(uint32_t *)(file_data + 0x50 + 10 * 0x20) = summary_offset;
    *(uint32_t *)(file_data + 0x48 + 11 * 0x20) = summary_slot_size;
    *(uint32_t *)(file_data + 0x50 + 11 * 0x20) = index_offset;

    /* Summary payload starts at summary_offset + 0x10 (after MD5 slot header) */
    uint8_t *summary_payload = file_data + summary_offset + 0x10;
    /* user_id at payload offset 0x04 */
    *(uint64_t *)(summary_payload + 0x04) = user_id;
    /* sz field at payload offset 0x150 spans face data, active slot, and padding before availability bytes */
    *(uint32_t *)(summary_payload + 0x150) = summary_layout_size;
    /* face-section size marker at payload offset 0x158 */
    *(uint32_t *)(summary_payload + 0x158) = face_section_size;

    /* MD5 of summary payload bytes goes in the 16-byte slot header prefix */
    md5_buffer(summary_payload, summary_data_size, file_data + summary_offset);

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LocalFree(file_data);
        return false;
    }
    DWORD written;
    bool ok = WriteFile(f, file_data, total_size, &written, NULL) && written == total_size;
    CloseHandle(f);
    LocalFree(file_data);
    return ok;
}

/* Compress an arbitrary source file as an ERSM FULL_SAVE container. */
static int selftest_make_valid_ersm_fullsave(const wchar_t *src_path, const wchar_t *dst_path) {
    HANDLE f = CreateFileW(src_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        st_printf(L"make-valid-ersm-fullsave: open source failed\n");
        return 1;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz)) {
        CloseHandle(f);
        st_printf(L"make-valid-ersm-fullsave: size query failed\n");
        return 1;
    }
    if (sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)ERSM_MAX_UNCOMPRESSED_SIZE) {
        CloseHandle(f);
        st_printf(L"make-valid-ersm-fullsave: source size out of range\n");
        return 1;
    }
    uint8_t *buf = LocalAlloc(LMEM_FIXED, (size_t)sz.QuadPart);
    if (!buf) {
        CloseHandle(f);
        st_printf(L"make-valid-ersm-fullsave: alloc failed\n");
        return 1;
    }
    DWORD read_bytes;
    if (!ReadFile(f, buf, (DWORD)sz.QuadPart, &read_bytes, NULL) || read_bytes != (DWORD)sz.QuadPart) {
        LocalFree(buf);
        CloseHandle(f);
        st_printf(L"make-valid-ersm-fullsave: read failed\n");
        return 1;
    }
    CloseHandle(f);

    bool ok = ersm_compress_to_file(dst_path, buf, (size_t)sz.QuadPart, ERSM_TYPE_FULL_SAVE, ERSM_LEVEL_NORMAL);
    LocalFree(buf);
    if (!ok) {
        st_printf(L"make-valid-ersm-fullsave: compress failed\n");
        return 1;
    }
    st_printf(L"wrote fullsave ERSM\n");
    return 0;
}

/* Provision a minimal save folder tree (root\76561...\ER0000.sl2) and write a matching INI. */
static int selftest_provision_save_folder(const wchar_t *root) {
    const uint64_t test_uid = 76561199999999999ULL;

    CreateDirectoryW(root, NULL); /* ignore "already exists" */

    wchar_t subfolder[MAX_PATH];
    lstrcpyW(subfolder, root);
    PathAppendW(subfolder, L"76561199999999999");
    CreateDirectoryW(subfolder, NULL);

    wchar_t sl2_path[MAX_PATH];
    lstrcpyW(sl2_path, subfolder);
    PathAppendW(sl2_path, L"ER0000.sl2");

    if (!make_min_valid_sl2(sl2_path, test_uid)) {
        st_printf(L"provision: sl2 write failed\n");
        return 1;
    }

    /* Build the INI path next to the running binary */
    wchar_t ini_path[MAX_PATH];
    GetModuleFileNameW(NULL, ini_path, MAX_PATH);
    PathRemoveFileSpecW(ini_path);
    PathAppendW(ini_path, L"ERSaveManager.ini");

    /* Compose the INI content as UTF-8 directly */
    char utf8_root[MAX_PATH * 4];
    WideCharToMultiByte(CP_UTF8, 0, root, -1, utf8_root, sizeof(utf8_root), NULL, NULL);

    char ini_buf[2048];
    int ini_len = _snprintf(ini_buf, sizeof(ini_buf),
        "[Settings]\r\n"
        "SavePath=%s\r\n"
        "SaveSubFolder=76561199999999999\r\n"
        "Language=0\r\n"
        "WindowX=-1\r\n"
        "WindowY=-1\r\n"
        "WindowWidth=0\r\n"
        "WindowHeight=0\r\n"
        "CompressionLevel=5\r\n", utf8_root);
    if (ini_len < 0 || ini_len >= (int)sizeof(ini_buf)) {
        st_printf(L"provision: ini path too long\n");
        return 1;
    }

    HANDLE fh = CreateFileW(ini_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        st_printf(L"provision: ini create failed\n");
        return 1;
    }
    DWORD written;
    bool ok = WriteFile(fh, ini_buf, (DWORD)ini_len, &written, NULL) && written == (DWORD)ini_len;
    CloseHandle(fh);
    if (!ok) {
        st_printf(L"provision: ini write failed\n");
        return 1;
    }

    st_printf(L"provisioned OK\n");
    return 0;
}

/* Parse --selftest <sub> [args...] and dispatch. Returns process exit code. */
static int run_selftest(LPWSTR cmd_line) {
    (void)cmd_line;

    /* Only attach/allocate a console if stdout is not already redirected to a file/pipe. */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = hOut ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    bool redirected = (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE);
    if (!redirected) {
        AllocConsole();
        FILE *fp = NULL;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2 || wcscmp(argv[1], L"--selftest") != 0 || argc < 3) {
        st_printf(L"usage: --selftest <subcommand> [args...]\n");
        if (argv) LocalFree(argv);
        return 2;
    }

    const wchar_t *sub = argv[2];
    int result;

    if (wcscmp(sub, L"roundtrip") == 0) {
        result = selftest_roundtrip(0x28024Cu, ERSM_TYPE_CHAR_SLOT);
    } else if (wcscmp(sub, L"roundtrip-large") == 0) {
        /* 30 MB exceeds the CHAR_SLOT size contract, so use FULL_SAVE for the large test. */
        result = selftest_roundtrip(30u * 1024u * 1024u, ERSM_TYPE_FULL_SAVE);
    } else if (wcscmp(sub, L"make-valid-ersm") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest make-valid-ersm <path>\n");
            result = 2;
        } else {
            result = selftest_make_valid_ersm(argv[3]);
        }
    } else if (wcscmp(sub, L"decompress-file") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest decompress-file <path>\n");
            result = 2;
        } else {
            result = selftest_decompress_file(argv[3]);
        }
    } else if (wcscmp(sub, L"detect-format") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest detect-format <path>\n");
            result = 2;
        } else {
            result = selftest_detect_format(argv[3]);
        }
    } else if (wcscmp(sub, L"make-legacy-slot") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest make-legacy-slot <path>\n");
            result = 2;
        } else {
            result = selftest_make_legacy_slot(argv[3]);
        }
    } else if (wcscmp(sub, L"make-bnd4-stub") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest make-bnd4-stub <path>\n");
            result = 2;
        } else {
            result = make_min_valid_sl2(argv[3], 76561199999999999ULL) ? 0 : 1;
        }
    } else if (wcscmp(sub, L"make-valid-ersm-fullsave") == 0) {
        if (argc < 5) {
            st_printf(L"usage: --selftest make-valid-ersm-fullsave <src> <dst>\n");
            result = 2;
        } else {
            result = selftest_make_valid_ersm_fullsave(argv[3], argv[4]);
        }
    } else if (wcscmp(sub, L"make-min-valid-sl2") == 0) {
        if (argc < 5) {
            st_printf(L"usage: --selftest make-min-valid-sl2 <path> <user_id>\n");
            result = 2;
        } else {
            uint64_t uid = _wcstoui64(argv[4], NULL, 10);
            result = make_min_valid_sl2(argv[3], uid) ? 0 : 1;
        }
    } else if (wcscmp(sub, L"provision-save-folder") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest provision-save-folder <root>\n");
            result = 2;
        } else {
            result = selftest_provision_save_folder(argv[3]);
        }
    } else if (wcscmp(sub, L"dump-active-slot") == 0) {
        if (argc < 4) {
            st_printf(L"usage: --selftest dump-active-slot <sl2_path>\n");
            result = 2;
        } else {
            er_save_data_t *save = er_save_data_load(argv[3]);
            if (!save) {
                st_printf(L"dump-active-slot: failed to load save\n");
                result = 1;
            } else {
                int byte_val = er_save_debug_get_active_slot_byte(save);
                uint32_t offset = er_save_debug_get_active_offset(save);
                if (byte_val < 0) {
                    st_printf(L"dump-active-slot: active_offset out of bounds\n");
                    er_save_data_free(save);
                    result = 1;
                } else {
                    st_printf(L"active_slot_byte=0x%02X active_offset=0x%04X\n", (unsigned)byte_val, (unsigned)offset);
                    er_save_data_free(save);
                    result = 0;
                }
            }
        }
    } else if (wcscmp(sub, L"write-active-slot") == 0) {
        if (argc < 5) {
            st_printf(L"usage: --selftest write-active-slot <sl2_path> <slot>\n");
            result = 2;
        } else {
            int slot = _wtoi(argv[4]);
            if (slot < 0 || slot > 9) {
                st_printf(L"write-active-slot: slot must be 0..9\n");
                result = 2;
            } else {
                er_save_data_t *save = er_save_data_load(argv[3]);
                if (!save) {
                    st_printf(L"write-active-slot: failed to load save\n");
                    result = 1;
                } else {
                    bool ok = er_save_debug_set_active_slot_byte(save, (uint8_t)slot, argv[3]);
                    er_save_data_free(save);
                    result = ok ? 0 : 1;
                }
            }
        }
    } else {
        st_printf(L"usage: --selftest <subcommand> [args...]\n");
        result = 2;
    }

    LocalFree(argv);
    return result;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, LPWSTR cmd_line, int cmd_show) {
    /* Initialize common controls */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    /* Initialize COM for IFileDialog (file_dialog_open/save/open_folder) */
    HRESULT com_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool com_initialized = SUCCEEDED(com_hr) || com_hr == S_FALSE;

    /* Enable visual styles */
    SetThemeAppProperties(STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT);

    /* Load configuration */
    load_config();

    /* Initialize theme system from loaded config (must precede window creation
     * so the dark titlebar attribute is applied on first paint). */
    theme_init_from_config();

    /* Initialize LZMA SDK */
    save_compress_init();

    /* --selftest: headless QA mode — runs tests and exits without showing a window */
    if (cmd_line && cmd_line[0] != L'\0' && wcsncmp(cmd_line, L"--selftest", 10) == 0) {
        return run_selftest(cmd_line);
    }

    /* Create main window */
    main_window = create_window(instance, cmd_show);
    if (!main_window)
        return 1;

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (com_initialized) CoUninitialize();
    return (int)msg.wParam;
}
