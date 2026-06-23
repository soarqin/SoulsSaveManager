/**
 * @file ui_controls.c
 * @brief Main window UI creation, layout, and language refresh implementation
 * @details Implements functions for creating main window controls, performing
 *          layout on resize, refreshing UI text after a language change,
 *          and releasing shared UI resources.
 */

#include "ui_controls.h"
#include "version.h"
#include "ersave.h"
#include "locale.h"
#include "config.h"
#include "embedded_face_data.h"
#include "resource.h"
#include "save_compress.h"
#include "theme.h"
#include "theme_core.h"
#include "ui_layout.h"
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

/* Globals declared in main.c */
extern HWND main_window;
extern HWND button_change_folder;
extern HWND combo_box_save_folder;
extern HWND button_manage_faces;
extern HWND list_view_chars;
extern HWND label_chars;
extern HWND button_import_char;
extern HWND button_export_char;
extern HWND button_rename_char;
extern HWND detail_group;
extern HWND detail_stat_labels[];
extern HWND detail_stat_values[];
extern HWND detail_runes_label;
extern HWND detail_runes_value;
extern HWND detail_deaths_label;
extern HWND detail_deaths_value;
extern HMENU menu_bar;
extern HMENU embedded_face_data_menu;
extern HFONT default_font;
extern er_save_data_t *save_data;

/* Functions declared in main.c */
extern bool handle_save_folder_selection(HWND hwnd);
extern void update_char_list_view(int item, const er_char_data_t *char_data);

static HMENU compression_submenu_handle = NULL;

/* Mapping from stat index to locale string index */
static const locale_string_index_t stat_str_indices[STAT_COUNT] = {
    STR_VIGOR, STR_MIND, STR_ENDURANCE, STR_STRENGTH,
    STR_DEXTERITY, STR_INTELLIGENCE, STR_FAITH, STR_ARCANE
};

/* Extra detail labels (runes held, death count) shown below the stat rows */
static const locale_string_index_t extra_detail_str_indices[] = {
    STR_RUNES_HELD, STR_DEATH_COUNT
};
#define EXTRA_DETAIL_COUNT (sizeof(extra_detail_str_indices) / sizeof(extra_detail_str_indices[0]))

/* Helper to set stat label text with colon suffix */
static void set_stat_label_text(int idx) {
    wchar_t text[64];
    wsprintfW(text, L"%s:", locale_str(stat_str_indices[idx]));
    SetWindowTextW(detail_stat_labels[idx], text);
}

/* Helper to set extra detail label text with colon suffix */
static void set_extra_detail_label_text(HWND label, locale_string_index_t str_idx) {
    wchar_t text[64];
    wsprintfW(text, L"%s:", locale_str(str_idx));
    SetWindowTextW(label, text);
}

/* Function to create embedded face data menu */
static void create_embedded_face_data_menu(HWND hwnd) {
    if (embedded_face_data_menu) {
        DestroyMenu(embedded_face_data_menu);
    }
    /* Create embedded face data menu */
    embedded_face_data_menu = CreatePopupMenu();

    HMENU popup_menus[4] = {
        CreatePopupMenu(),
        CreatePopupMenu(),
        CreatePopupMenu(),
        CreatePopupMenu(),
    };

    int locale_idx = get_current_locale();
    /* Add embedded face data menu to menu bar */
    for (int i = 0; i < embedded_face_data_count; i++) {
        AppendMenuW(popup_menus[embedded_face_data[i].category], MF_STRING, IDM_EMBEDDED_FACE_DATA_START + i, embedded_face_data[i].name[locale_idx]);
    }

    AppendMenuW(embedded_face_data_menu, MF_POPUP, (UINT_PTR)popup_menus[0], locale_str(STR_NPC_BASE));
    AppendMenuW(embedded_face_data_menu, MF_POPUP, (UINT_PTR)popup_menus[1], locale_str(STR_NPC_BASE_NON_INTERACTABLE));
    AppendMenuW(embedded_face_data_menu, MF_POPUP, (UINT_PTR)popup_menus[2], locale_str(STR_NPC_DLC));
    AppendMenuW(embedded_face_data_menu, MF_POPUP, (UINT_PTR)popup_menus[3], locale_str(STR_NPC_DLC_NON_INTERACTABLE));
}

/* Fill the save-folder combo box with valid Steam subfolders */
void add_folders_to_combo_box(void) {
    /* Clear the combo box */
    SendMessageW(combo_box_save_folder, CB_RESETCONTENT, 0, 0);

    /* Get user's AppData\Roaming path */
    wchar_t search_path[MAX_PATH];
    if (config.save_path[0] == L'\0') return;

    /* Create search pattern for subdirectories */
    lstrcpyW(search_path, config.save_path);
    PathAppendW(search_path, L"\\*");
    WIN32_FIND_DATAW find_data;
    HANDLE find = FindFirstFileW(search_path, &find_data);

    if (find != INVALID_HANDLE_VALUE) {
        do {
            /* Skip "." and ".." directories */
            if (find_data.cFileName[0] == L'.' && (find_data.cFileName[1] == L'\0' || (find_data.cFileName[1] == L'.' && find_data.cFileName[2] == L'\0'))) {
                continue;
            }

            /* Skip non-directory entries */
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }

            /* Check if SteamID is valid */
            wchar_t *endptr;
            uint64_t steam_id = wcstoull(find_data.cFileName, &endptr, 10);
            /* Steam ID = (Universe << 56) | (Type << 52) | (Instance << 32) | AccountID
             *  Universe: 0-3
             *  Type: 1-10
             *  Instance: usually 1
             * So SteamID is always greater than 0x10000000000000ULL */
            if (*endptr != L'\0' || steam_id < 0x10000000000000ULL) {
                continue;
            }

            /* Check if ER0000.sl2 exists in the directory */
            wchar_t save_path[MAX_PATH];
            lstrcpyW(save_path, config.save_path);
            PathAppendW(save_path, find_data.cFileName);
            PathAppendW(save_path, L"\\ER0000.sl2");

            if (!PathFileExistsW(save_path)) {
                continue;
            }

            SendMessageW(combo_box_save_folder, CB_ADDSTRING, 0, (LPARAM)find_data.cFileName);
        } while (FindNextFileW(find, &find_data));

        FindClose(find);
    }
}

void ui_create_controls(HWND hwnd, HMODULE module) {
    /* Get system default font */
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0);
    default_font = CreateFontIndirectW(&ncm.lfMessageFont);

    /* Create Button */
    button_change_folder = CreateWindowW(
        L"BUTTON", locale_str(STR_CHANGE_SAVE_FOLDER),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        UI_MARGIN, UI_MARGIN, 160, UI_BTN_HEIGHT,
        hwnd, (HMENU)IDC_BUTTON_CHANGE_FOLDER, module, NULL
    );
    SendMessage(button_change_folder, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create ComboBox */
    combo_box_save_folder = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        UI_MARGIN + 160 + UI_GAP_SMALL, UI_MARGIN, 200, UI_COMBO_HEIGHT,
        hwnd, (HMENU)IDC_COMBO_SAVE_FOLDER, module, NULL
    );
    SendMessage(combo_box_save_folder, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create Characters Label */
    label_chars = CreateWindowW(
        L"STATIC", locale_str(STR_CHARACTERS),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        UI_MARGIN, UI_MARGIN + UI_BTN_HEIGHT + UI_GAP_MEDIUM, 200, 20,
        hwnd, (HMENU)6, module, NULL
    );
    SendMessage(label_chars, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create Characters ListView */
    int list_view_y = UI_MARGIN + UI_BTN_HEIGHT + UI_GAP_MEDIUM + 20 + UI_GAP_SMALL;
    list_view_chars = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        UI_MARGIN, list_view_y, 200, 280,
        hwnd, (HMENU)4, module, NULL
    );
    ListView_SetExtendedListViewStyleEx(list_view_chars,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT);
    SendMessage(list_view_chars, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Add columns to Characters ListView */
    LVCOLUMNW lvc;
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    lvc.iSubItem = 0;
    lvc.cx = 40;
    lvc.pszText = (LPWSTR)locale_str(STR_SLOT);
    ListView_InsertColumn(list_view_chars, 0, &lvc);

    lvc.iSubItem = 1;
    lvc.cx = 120;
    lvc.pszText = (LPWSTR)locale_str(STR_NAME);
    ListView_InsertColumn(list_view_chars, 1, &lvc);

    lvc.iSubItem = 2;
    lvc.cx = 80;
    lvc.pszText = (LPWSTR)locale_str(STR_BODY_TYPE);
    ListView_InsertColumn(list_view_chars, 2, &lvc);

    lvc.iSubItem = 3;
    lvc.cx = 60;
    lvc.pszText = (LPWSTR)locale_str(STR_LEVEL);
    ListView_InsertColumn(list_view_chars, 3, &lvc);

    lvc.iSubItem = 4;
    lvc.cx = 95;
    lvc.pszText = (LPWSTR)locale_str(STR_IN_GAME_TIME);
    ListView_InsertColumn(list_view_chars, 4, &lvc);

    /* Create detail panel group box for attributes */
    detail_group = CreateWindowW(
        L"BUTTON", locale_str(STR_ATTRIBUTES),
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        0, 0, 220, 250,
        hwnd, NULL, module, NULL
    );
    SendMessage(detail_group, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create stat label/value pairs inside detail panel */
    for (int i = 0; i < STAT_COUNT; i++) {
        detail_stat_labels[i] = CreateWindowW(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 90, 18,
            hwnd, NULL, module, NULL
        );
        SendMessage(detail_stat_labels[i], WM_SETFONT, (WPARAM)default_font, TRUE);
        set_stat_label_text(i);

        detail_stat_values[i] = CreateWindowW(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 60, 18,
            hwnd, NULL, module, NULL
        );
        SendMessage(detail_stat_values[i], WM_SETFONT, (WPARAM)default_font, TRUE);
    }

    /* Create extra detail label/value pairs (runes held, death count) */
    detail_runes_label = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 90, 18,
        hwnd, NULL, module, NULL
    );
    SendMessage(detail_runes_label, WM_SETFONT, (WPARAM)default_font, TRUE);
    set_extra_detail_label_text(detail_runes_label, STR_RUNES_HELD);

    detail_runes_value = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 60, 18,
        hwnd, NULL, module, NULL
    );
    SendMessage(detail_runes_value, WM_SETFONT, (WPARAM)default_font, TRUE);

    detail_deaths_label = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 90, 18,
        hwnd, NULL, module, NULL
    );
    SendMessage(detail_deaths_label, WM_SETFONT, (WPARAM)default_font, TRUE);
    set_extra_detail_label_text(detail_deaths_label, STR_DEATH_COUNT);

    detail_deaths_value = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 60, 18,
        hwnd, NULL, module, NULL
    );
    SendMessage(detail_deaths_value, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create character action buttons below the ListView (initially disabled) */
    button_import_char = CreateWindowW(
        L"BUTTON", locale_str(STR_IMPORT_CHARACTER),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 100, 25,
        hwnd, (HMENU)IDC_BUTTON_IMPORT_CHAR, module, NULL
    );
    SendMessage(button_import_char, WM_SETFONT, (WPARAM)default_font, TRUE);

    button_export_char = CreateWindowW(
        L"BUTTON", locale_str(STR_EXPORT_CHARACTER),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 100, 25,
        hwnd, (HMENU)IDC_BUTTON_EXPORT_CHAR, module, NULL
    );
    SendMessage(button_export_char, WM_SETFONT, (WPARAM)default_font, TRUE);

    button_rename_char = CreateWindowW(
        L"BUTTON", locale_str(STR_RENAME_CHARACTER),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 100, 25,
        hwnd, (HMENU)IDC_BUTTON_RENAME_CHAR, module, NULL
    );
    SendMessage(button_rename_char, WM_SETFONT, (WPARAM)default_font, TRUE);

    /* Create Manage Faces Button */
    button_manage_faces = CreateWindowW(
        L"BUTTON", locale_str(STR_MANAGE_FACES),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        380, UI_MARGIN, 120, UI_BTN_HEIGHT,
        hwnd, (HMENU)IDC_BUTTON_MANAGE_FACES, module, NULL
    );
    SendMessage(button_manage_faces, WM_SETFONT, (WPARAM)default_font, TRUE);

    add_folders_to_combo_box();

    /* Set initial ComboBox selection */
    if (config.save_subfolder[0] == L'\0') {
        SendMessageW(combo_box_save_folder, CB_SETCURSEL, 0, 0);
    } else {
        int idx = SendMessageW(combo_box_save_folder, CB_FINDSTRING, -1, (LPARAM)config.save_subfolder);
        SendMessageW(combo_box_save_folder, CB_SETCURSEL, idx == CB_ERR ? 0 : idx, 0);
    }
    handle_save_folder_selection(hwnd);

    /* Create menu bar */
    menu_bar = CreateMenu();
    HMENU locale_menu = CreatePopupMenu();

    /* Add locale options */
    int locale_cnt = locale_count();
    for (int i = 0; i < locale_cnt; i++) {
        AppendMenuW(locale_menu, MF_STRING, IDM_LOCALE_START + i, locale_name(i));
    }

    /* Add locale menu to menu bar */
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)locale_menu, locale_str(STR_LANGUAGE));
    SetMenu(hwnd, menu_bar);

    /* Tick menu item */
    CheckMenuItem(menu_bar, IDM_LOCALE_START + get_current_locale(), MF_CHECKED);

    /* Create Options menu with Compression submenu */
    HMENU options_menu = CreatePopupMenu();
    HMENU compression_submenu = CreatePopupMenu();
    AppendMenuW(compression_submenu, MF_STRING, IDM_COMPRESSION_FAST,   locale_str(STR_COMPRESSION_FAST));
    AppendMenuW(compression_submenu, MF_STRING, IDM_COMPRESSION_NORMAL, locale_str(STR_COMPRESSION_NORMAL));
    AppendMenuW(compression_submenu, MF_STRING, IDM_COMPRESSION_MAX,    locale_str(STR_COMPRESSION_MAX));
    AppendMenuW(options_menu, MF_POPUP, (UINT_PTR)compression_submenu, locale_str(STR_COMPRESSION_LEVEL));
    /* Theme submenu (System / Light / Dark). */
    theme_build_submenu(options_menu);
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)options_menu, locale_str(STR_OPTIONS));
    SetMenu(hwnd, menu_bar);
    compression_submenu_handle = compression_submenu;

    /* Create Tools menu */
    HMENU tools_menu = CreatePopupMenu();
    AppendMenuW(tools_menu, MF_STRING, IDM_TOOLS_DOWNPATCH_1_02_1, locale_str(STR_DOWNPATCH_1_02_1));
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)tools_menu, locale_str(STR_TOOLS));
    SetMenu(hwnd, menu_bar);

    /* Suppress the check-mark gutter: the Options menu has no checkable/icon items */
    {
        MENUINFO mi;
        ZeroMemory(&mi, sizeof(MENUINFO));
        mi.cbSize = sizeof(MENUINFO);
        mi.fMask  = MIM_STYLE;
        mi.dwStyle = MNS_NOCHECK;
        SetMenuInfo(options_menu, &mi);
    }

    /* Tick current compression level */
    static const UINT compression_cmds[] = { IDM_COMPRESSION_FAST, IDM_COMPRESSION_NORMAL, IDM_COMPRESSION_MAX };
    static const int  compression_lvls[] = { ERSM_LEVEL_FAST, ERSM_LEVEL_NORMAL, ERSM_LEVEL_MAX };
    for (int i = 0; i < 3; i++) {
        int state = (config.compression_level == compression_lvls[i]) ? MF_CHECKED : MF_UNCHECKED;
        CheckMenuItem(compression_submenu, compression_cmds[i], MF_BYCOMMAND | state);
    }

    create_embedded_face_data_menu(hwnd);

    /* Apply theme to all controls just created. The dark titlebar etc. is
     * applied separately on the top-level window. */
    theme_apply_to_window(hwnd);
}

/**
 * @brief Calculate the optimal detail panel width for the current locale
 * @details Measures all stat label strings (with colon suffix) to determine
 *          the minimum width that prevents text overflow while avoiding
 *          excessive whitespace on the left side.
 * @param hwnd Window handle used to obtain a device context
 * @param out_label_w Receives the computed label column width in pixels
 * @return Recommended total width for the detail panel
 */
static int calculate_detail_panel_width(HWND hwnd, int *out_label_w) {
    HDC hdc = GetDC(hwnd);
    HFONT old_font = (HFONT)SelectObject(hdc, default_font);

    int max_label_w = 0;
    for (int i = 0; i < STAT_COUNT; i++) {
        wchar_t text[64];
        wsprintfW(text, L"%s:", locale_str(stat_str_indices[i]));
        SIZE sz;
        GetTextExtentPoint32W(hdc, text, lstrlenW(text), &sz);
        if (sz.cx > max_label_w) {
            max_label_w = sz.cx;
        }
    }
    /* Also measure extra detail labels (runes held, death count) */
    for (int i = 0; i < (int)EXTRA_DETAIL_COUNT; i++) {
        wchar_t text[64];
        wsprintfW(text, L"%s:", locale_str(extra_detail_str_indices[i]));
        SIZE sz;
        GetTextExtentPoint32W(hdc, text, lstrlenW(text), &sz);
        if (sz.cx > max_label_w) {
            max_label_w = sz.cx;
        }
    }

    /* Also measure the group box title to ensure it fits */
    const wchar_t *title = locale_str(STR_ATTRIBUTES);
    SIZE title_sz;
    GetTextExtentPoint32W(hdc, title, lstrlenW(title), &title_sz);

    SelectObject(hdc, old_font);
    ReleaseDC(hwnd, hdc);

    /* Layout: [UI_MARGIN pad | label | UI_GAP_SMALL gap | 50px value | UI_MARGIN pad] */
    int label_w = max_label_w + 4;
    int value_w = 50;
    int detail_w = UI_MARGIN + label_w + UI_GAP_SMALL + value_w + UI_MARGIN;

    /* Ensure group box title fits (title text + frame chrome) */
    int min_for_title = title_sz.cx + UI_MARGIN * 2;
    if (detail_w < min_for_title) {
        detail_w = min_for_title;
        label_w = detail_w - UI_MARGIN - UI_GAP_SMALL - value_w - UI_MARGIN;
    }

    /* Clamp to reasonable bounds */
    if (detail_w < 140) detail_w = 140;
    if (detail_w > 300) detail_w = 300;

    *out_label_w = label_w;
    return detail_w;
}

void ui_layout_controls(HWND hwnd, int width, int height) {
    int btn_face_w = 120;
    int label_w;
    int detail_w = calculate_detail_panel_width(hwnd, &label_w);
    int detail_x = width - UI_MARGIN - detail_w;
    int list_w = detail_x - UI_MARGIN - UI_MARGIN;
    int content_y = UI_MARGIN + UI_BTN_HEIGHT + UI_GAP_MEDIUM + 20 + UI_GAP_SMALL;
    int content_h = height - content_y - UI_MARGIN;

    /* Reserve space for character action buttons below the ListView */
    int btn_bar_h = UI_BTN_HEIGHT + UI_GAP_SMALL;  /* 25px button + 8px gap */
    int list_h = content_h - btn_bar_h;
    int btn_y = content_y + list_h + UI_GAP_SMALL;
    int btn_h = UI_BTN_HEIGHT;
    int btn_gap = UI_GAP_SMALL;
    int btn_w = (list_w - btn_gap * 2) / 3;

    /* 5 base + 3 char buttons + group box + 8 stat pairs + 4 extra detail = 29 */
    HDWP hdwp = BeginDeferWindowPos(5 + 3 + 1 + STAT_COUNT * 2 + 4);

    /* Top row */
    hdwp = DeferWindowPos(hdwp, button_change_folder, NULL,
        UI_MARGIN, UI_MARGIN, 160, UI_BTN_HEIGHT, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, combo_box_save_folder, NULL,
        UI_MARGIN + 160 + UI_GAP_SMALL, UI_MARGIN,
        width - (UI_MARGIN + 160 + UI_GAP_SMALL) - UI_GAP_SMALL - btn_face_w - UI_MARGIN,
        UI_COMBO_HEIGHT, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, button_manage_faces, NULL,
        width - UI_MARGIN - btn_face_w, UI_MARGIN, btn_face_w, UI_BTN_HEIGHT, SWP_NOZORDER);

    /* Characters label (above list only) */
    hdwp = DeferWindowPos(hdwp, label_chars, NULL,
        UI_MARGIN, UI_MARGIN + UI_BTN_HEIGHT + UI_GAP_MEDIUM, list_w, 20, SWP_NOZORDER);

    /* Characters ListView (left side, shortened for button bar) */
    hdwp = DeferWindowPos(hdwp, list_view_chars, NULL,
        UI_MARGIN, content_y, list_w, list_h, SWP_NOZORDER);

    /* Character action buttons (below ListView) */
    hdwp = DeferWindowPos(hdwp, button_import_char, NULL,
        UI_MARGIN, btn_y, btn_w, btn_h, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, button_export_char, NULL,
        UI_MARGIN + btn_w + btn_gap, btn_y, btn_w, btn_h, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, button_rename_char, NULL,
        UI_MARGIN + (btn_w + btn_gap) * 2, btn_y, btn_w, btn_h, SWP_NOZORDER);

    /* Detail panel group box (right side) */
    hdwp = DeferWindowPos(hdwp, detail_group, NULL,
        detail_x, UI_MARGIN + UI_BTN_HEIGHT + UI_GAP_MEDIUM, detail_w, content_h + 20, SWP_NOZORDER);

    /* Stat label/value rows inside the detail panel area */
    int row_h = UI_ROW_HEIGHT;
    int label_x = detail_x + UI_MARGIN;
    /* label_w is computed by calculate_detail_panel_width() above */
    int value_x = label_x + label_w + UI_GAP_SMALL;
    int value_w = 50;
    int first_row_y = content_y + UI_GAP_SMALL;

    for (int i = 0; i < STAT_COUNT; i++) {
        int row_y = first_row_y + i * row_h;
        hdwp = DeferWindowPos(hdwp, detail_stat_labels[i], NULL,
            label_x, row_y, label_w, 18, SWP_NOZORDER);
        hdwp = DeferWindowPos(hdwp, detail_stat_values[i], NULL,
            value_x, row_y, value_w, 18, SWP_NOZORDER);
    }

    /* Extra detail rows (runes held, death count) below the stat rows with a small gap */
    int extra_y = first_row_y + STAT_COUNT * row_h + UI_GAP_SMALL;
    hdwp = DeferWindowPos(hdwp, detail_runes_label, NULL,
        label_x, extra_y, label_w, 18, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, detail_runes_value, NULL,
        value_x, extra_y, value_w, 18, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, detail_deaths_label, NULL,
        label_x, extra_y + row_h, label_w, 18, SWP_NOZORDER);
    hdwp = DeferWindowPos(hdwp, detail_deaths_value, NULL,
        value_x, extra_y + row_h, value_w, 18, SWP_NOZORDER);

    /* Apply all window position changes at once */
    EndDeferWindowPos(hdwp);
}

void ui_refresh_language(void) {
    /* Rebuild embedded face data menu for the new locale */
    create_embedded_face_data_menu(main_window);

    wchar_t window_title[128];
    wsprintfW(window_title, L"%s v%s", locale_str(STR_APP_TITLE), VERSION_STR_W);
    SetWindowTextW(main_window, window_title);
    SetWindowTextW(button_change_folder, locale_str(STR_CHANGE_SAVE_FOLDER));
    SetWindowTextW(button_manage_faces, locale_str(STR_MANAGE_FACES));
    SetWindowTextW(label_chars, locale_str(STR_CHARACTERS));
    SetWindowTextW(button_import_char, locale_str(STR_IMPORT_CHARACTER));
    SetWindowTextW(button_export_char, locale_str(STR_EXPORT_CHARACTER));
    SetWindowTextW(button_rename_char, locale_str(STR_RENAME_CHARACTER));

    /* Update detail panel labels */
    SetWindowTextW(detail_group, locale_str(STR_ATTRIBUTES));
    for (int i = 0; i < STAT_COUNT; i++) {
        set_stat_label_text(i);
    }
    set_extra_detail_label_text(detail_runes_label, STR_RUNES_HELD);
    set_extra_detail_label_text(detail_deaths_label, STR_DEATH_COUNT);

    /* Update menu title */
    HMENU locale_menu = GetSubMenu(menu_bar, 0);
    ModifyMenuW(menu_bar, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)locale_menu, locale_str(STR_LANGUAGE));
    DrawMenuBar(main_window);

    /* Rebuild compression submenu strings for new locale */
    if (compression_submenu_handle) {
        /* Remove all 3 items and re-add with new locale strings */
        while (GetMenuItemCount(compression_submenu_handle) > 0)
            RemoveMenu(compression_submenu_handle, 0, MF_BYPOSITION);
        AppendMenuW(compression_submenu_handle, MF_STRING, IDM_COMPRESSION_FAST,   locale_str(STR_COMPRESSION_FAST));
        AppendMenuW(compression_submenu_handle, MF_STRING, IDM_COMPRESSION_NORMAL, locale_str(STR_COMPRESSION_NORMAL));
        AppendMenuW(compression_submenu_handle, MF_STRING, IDM_COMPRESSION_MAX,    locale_str(STR_COMPRESSION_MAX));
        /* Re-apply checkmark */
        static const UINT cmds[] = { IDM_COMPRESSION_FAST, IDM_COMPRESSION_NORMAL, IDM_COMPRESSION_MAX };
        static const int  lvls[] = { ERSM_LEVEL_FAST, ERSM_LEVEL_NORMAL, ERSM_LEVEL_MAX };
        for (int i = 0; i < 3; i++) {
            CheckMenuItem(compression_submenu_handle, cmds[i],
                MF_BYCOMMAND | (config.compression_level == lvls[i] ? MF_CHECKED : MF_UNCHECKED));
        }
        /* Also update the Options menu title and Compression submenu title */
        HMENU options_menu = GetSubMenu(menu_bar, 1); /* index 1 = Options (after Language) */
        if (options_menu) {
            ModifyMenuW(options_menu, 0, MF_BYPOSITION | MF_POPUP,
                (UINT_PTR)compression_submenu_handle, locale_str(STR_COMPRESSION_LEVEL));
        }
        ModifyMenuW(menu_bar, 1, MF_BYPOSITION | MF_POPUP,
            (UINT_PTR)options_menu, locale_str(STR_OPTIONS));

        /* Refresh Theme submenu strings (System/Light/Dark). The submenu is
         * a child of options_menu at index 1 (after Compression at index 0). */
        theme_rebuild_submenu_strings();
        if (options_menu) {
            ModifyMenuW(options_menu, 1, MF_BYPOSITION | MF_POPUP,
                (UINT_PTR)GetSubMenu(options_menu, 1), locale_str(STR_THEME));
        }
        DrawMenuBar(main_window);
    }

    /* Refresh Tools menu (index 2 = Tools, after Language and Options) */
    HMENU tools_menu = GetSubMenu(menu_bar, 2);
    if (tools_menu) {
        ModifyMenuW(tools_menu, IDM_TOOLS_DOWNPATCH_1_02_1, MF_BYCOMMAND | MF_STRING,
            IDM_TOOLS_DOWNPATCH_1_02_1, locale_str(STR_DOWNPATCH_1_02_1));
        ModifyMenuW(menu_bar, 2, MF_BYPOSITION | MF_POPUP,
            (UINT_PTR)tools_menu, locale_str(STR_TOOLS));
        DrawMenuBar(main_window);
    }

    /* Update characters ListView columns */
    LVCOLUMNW lvc;
    lvc.mask = LVCF_TEXT;

    lvc.iSubItem = 0;
    lvc.pszText = (LPWSTR)locale_str(STR_SLOT);
    ListView_SetColumn(list_view_chars, 0, &lvc);

    lvc.iSubItem = 1;
    lvc.pszText = (LPWSTR)locale_str(STR_NAME);
    ListView_SetColumn(list_view_chars, 1, &lvc);

    lvc.iSubItem = 2;
    lvc.pszText = (LPWSTR)locale_str(STR_BODY_TYPE);
    ListView_SetColumn(list_view_chars, 2, &lvc);

    lvc.iSubItem = 3;
    lvc.pszText = (LPWSTR)locale_str(STR_LEVEL);
    ListView_SetColumn(list_view_chars, 3, &lvc);

    lvc.iSubItem = 4;
    lvc.pszText = (LPWSTR)locale_str(STR_IN_GAME_TIME);
    ListView_SetColumn(list_view_chars, 4, &lvc);

    /* Force relayout to adapt detail panel width for the new locale */
    RECT rc;
    GetClientRect(main_window, &rc);
    ui_layout_controls(main_window, rc.right, rc.bottom);

    if (!save_data) {
        return;
    }

    /* Update characters ListView items */
    for (int i = 0; i < 10; i++) {
        const er_char_data_t *char_data = er_char_data_ref(save_data, i);
        update_char_list_view(i, char_data);
    }
}

void ui_update_char_buttons(void) {
    int sel = ListView_GetNextItem(list_view_chars, -1, LVNI_SELECTED);
    bool has_selection = (sel >= 0 && save_data != NULL);
    bool has_char = false;
    if (has_selection) {
        has_char = er_char_data_ref(save_data, sel) != NULL;
    }
    EnableWindow(button_import_char, has_selection);
    EnableWindow(button_export_char, has_char);
    EnableWindow(button_rename_char, has_char);
}

void ui_cleanup(void) {
    DeleteObject(default_font);
}

void ui_update_compression_menu(void) {
    if (!compression_submenu_handle) return;
    static const UINT cmds[] = { IDM_COMPRESSION_FAST, IDM_COMPRESSION_NORMAL, IDM_COMPRESSION_MAX };
    static const int  lvls[] = { ERSM_LEVEL_FAST, ERSM_LEVEL_NORMAL, ERSM_LEVEL_MAX };
    for (int i = 0; i < 3; i++) {
        CheckMenuItem(compression_submenu_handle, cmds[i],
            MF_BYCOMMAND | (config.compression_level == lvls[i] ? MF_CHECKED : MF_UNCHECKED));
    }
    DrawMenuBar(main_window);
}
