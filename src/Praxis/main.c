/**
 * @file main.c
 * @brief Praxis application entry point and main window procedure.
 */

#include "config.h"
#include "../common/config_core.h"
#include "../common/theme_core.h"
#include "backend_registry.h"
#include "hotkey.h"
#include "locale.h"
#include "resource.h"
#include "file_dialog.h"
#include "praxis_hotkey_actions.h"
#include "save_tree.h"
#include "save_watcher.h"
#include "profile_store.h"
#include "profile_store_io.h"
#include "praxis_main_menu.h"
#include "praxis_toast.h"
#include "praxis_window_common.h"
#include "theme.h"
#include "toolbar.h"
#include "dialogs/edit_game_profile.h"
#include "dialogs/game_profile_manager.h"
#include "dialogs/edit_backup_profile.h"
#include "dialogs/hotkey_settings.h"
#include "../common/ersave.h"
#include "../common/save_compress.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

praxis_app_t g_app = {0};
profile_store_t g_profile_store;
HANDLE g_log_file = INVALID_HANDLE_VALUE;

static LRESULT CALLBACK praxis_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void praxis_window_on_create(HWND hwnd, HINSTANCE instance);
static LRESULT praxis_window_on_command(HWND hwnd, WPARAM wp);
static void praxis_first_launch_setup(const wchar_t *ini_path);

#define IDT_REFRESH_DEBOUNCE 1001
#define WM_WATCHER_NOTIFY (WM_APP + 1)

static void praxis_window_on_create(HWND hwnd, HINSTANCE instance) {
    wchar_t ini[MAX_PATH];

    g_app.save_tree = save_tree_create(hwnd, instance, IDC_TREE_VIEW);
    if (!g_app.save_tree) return;
    g_app.toolbar = toolbar_create(hwnd, instance);
    if (g_app.toolbar) {
        RECT client_rect;
        if (GetClientRect(hwnd, &client_rect)) {
            int client_width = client_rect.right - client_rect.left;
            int top_h = toolbar_get_top_height(g_app.toolbar);
            toolbar_layout_top(g_app.toolbar, client_width);
            toolbar_layout_bottom(g_app.toolbar, client_width, top_h);
        }
    }
    g_app.status_bar = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)(uintptr_t)IDC_STATUS_BAR, instance, NULL);
    if (!g_app.status_bar) {
        destroy_main_children();
        return;
    }
    save_tree_set_root(g_app.save_tree, praxis_config.tree_root);
    if (praxis_config.tree_root[0] != L'\0') g_app.save_watcher = save_watcher_start(hwnd, praxis_config.tree_root, WM_WATCHER_NOTIFY);
    profile_store_init(&g_profile_store);
    if (config_core_get_app_ini_path(ini, MAX_PATH, L"Praxis.ini")) profile_store_io_load(&g_profile_store, ini);
    populate_toolbar_profiles();
    apply_active_profile_ui(hwnd, WM_WATCHER_NOTIFY);
    g_app.main_window = hwnd;
    praxis_main_menu_apply_locale_strings(hwnd);
    register_hotkeys(hwnd);
    /* Toast must be created last so it sits on top of every other child in
     * the sibling Z-order; show() will additionally HWND_TOP it on demand. */
    g_app.toast = praxis_toast_create(hwnd, instance);
    /* Apply theme to top-level window AND every child created above. */
    praxis_theme_apply_to_window(hwnd);
}

/* Show the standard green success toast for a backup/restore action.
 * Acts only when the result indicates success; failures fall through to
 * show_error_toast(). The success color is resolved at call time so it
 * tracks the active theme without needing a reapply path. Passing 0 as
 * border_color tells the toast to use its default theme edge border. */
static void show_success_toast(praxis_string_index_t msg, praxis_action_result_t result) {
    if (result != PRAXIS_ACTION_OK || !g_app.toast) return;
    praxis_toast_show(g_app.toast, praxis_locale_str(msg),
                      praxis_toast_color_success(), 0,
                      PRAXIS_TOAST_DEFAULT_DURATION_MS);
}

/* Map a failure result code to its locale string. */
static praxis_string_index_t action_error_str(praxis_action_result_t result) {
    switch (result) {
    case PRAXIS_ACTION_ERR_NO_PROFILE:         return STR_PRAXIS_TOAST_ERR_NO_PROFILE;
    case PRAXIS_ACTION_ERR_SAVE_NOT_FOUND:     return STR_PRAXIS_TOAST_ERR_SAVE_NOT_FOUND;
    case PRAXIS_ACTION_ERR_SLOT_NOT_SUPPORTED: return STR_PRAXIS_TOAST_ERR_SLOT_NOT_SUPPORTED;
    case PRAXIS_ACTION_ERR_SLOT_EMPTY:         return STR_PRAXIS_TOAST_ERR_SLOT_EMPTY;
    case PRAXIS_ACTION_ERR_NO_SELECTION:       return STR_PRAXIS_TOAST_ERR_NO_SELECTION;
    case PRAXIS_ACTION_ERR_FILE_READONLY:      return STR_PRAXIS_TOAST_ERR_FILE_READONLY;
    case PRAXIS_ACTION_ERR_RING_BACKUP:        return STR_PRAXIS_TOAST_ERR_RING_BACKUP;
    case PRAXIS_ACTION_ERR_IO:                 return STR_PRAXIS_TOAST_ERR_IO;
    case PRAXIS_ACTION_ERR_NO_UNDO:            return STR_PRAXIS_TOAST_ERR_NO_UNDO;
    default:                                   return STR_PRAXIS_TOAST_ERR_IO;
    }
}

/* Show an error toast (normal text color, emphasized red border) when the
 * action result indicates failure. */
static void show_error_toast(praxis_action_result_t result) {
    const theme_palette_t *pal;
    COLORREF text_color;

    if (result == PRAXIS_ACTION_OK || !g_app.toast) return;
    pal = theme_core_palette();
    text_color = pal ? pal->text : GetSysColor(COLOR_WINDOWTEXT);
    praxis_toast_show(g_app.toast, praxis_locale_str(action_error_str(result)),
                      text_color, praxis_toast_color_error(),
                      PRAXIS_TOAST_DEFAULT_DURATION_MS);
}

static void run_hotkey_action(HWND hwnd, hotkey_id_t hotkey_id) {
    praxis_action_result_t result;
    switch (hotkey_id) {
    case HOTKEY_BACKUP_FULL:
        result = praxis_hotkey_action_backup_full(hwnd, &g_profile_store, g_app.save_tree, get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_FULL_SUCCESS, result);
        show_error_toast(result);
        break;
    case HOTKEY_BACKUP_SLOT:
        result = praxis_hotkey_action_backup_slot(hwnd, &g_profile_store, g_app.save_tree, get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_SLOT_SUCCESS, result);
        show_error_toast(result);
        break;
    case HOTKEY_RESTORE:
        result = praxis_hotkey_action_restore(hwnd, &g_profile_store, g_app.save_tree);
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_RESTORE_SUCCESS, result);
        show_error_toast(result);
        break;
    case HOTKEY_UNDO_RESTORE:
        result = praxis_hotkey_action_undo(hwnd, &g_profile_store);
        /* Preserve the current selection across the refresh so the tree does
         * not jump back to the root after undoing a restore. */
        if (result == PRAXIS_ACTION_OK && g_app.save_tree) save_tree_refresh_preserve_selection(g_app.save_tree);
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_UNDO_SUCCESS, result);
        show_error_toast(result);
        break;
    case HOTKEY_BACKUP_REPLACE:
        if (!save_tree_selected_file_can_replace(g_app.save_tree)) {
            return;
        }
        result = praxis_hotkey_action_backup_replace_selected(hwnd, &g_profile_store, g_app.save_tree,
            get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_REPLACE_SUCCESS, result);
        show_error_toast(result);
        break;
    case HOTKEY_PREVIOUS_SAVE:
        if (g_app.save_tree) save_tree_select_sibling_file(g_app.save_tree, -1);
        update_toolbar_action_state();
        break;
    case HOTKEY_NEXT_SAVE:
        if (g_app.save_tree) save_tree_select_sibling_file(g_app.save_tree, 1);
        update_toolbar_action_state();
        break;
    }
}

static LRESULT praxis_window_on_command(HWND hwnd, WPARAM wp) {
    if (praxis_main_menu_handle_command(hwnd, wp, &g_profile_store)) {
        if (g_app.toolbar) toolbar_apply_locale_strings(g_app.toolbar);
        populate_toolbar_profiles();
        apply_active_profile_ui(hwnd, WM_WATCHER_NOTIFY);
        return 0;
    }
    if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_PROFILE_COMBO) {
        handle_profile_combo_change(hwnd, WM_WATCHER_NOTIFY);
        return 0;
    }
    if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_SORT_COMBO) {
        if (g_app.toolbar && g_app.save_tree) {
            save_tree_set_sort_mode(g_app.save_tree, toolbar_get_selected_sort_mode(g_app.toolbar));
            update_toolbar_action_state();
        }
        return 0;
    }
    switch (LOWORD(wp)) {
    case IDM_GAME_MANAGE: {
        wchar_t ini[MAX_PATH];
        config_core_get_app_ini_path(ini, MAX_PATH, L"Praxis.ini");
        dialog_game_profile_manager_show(hwnd, &g_profile_store, ini);
        populate_toolbar_profiles();
        apply_active_profile_ui(hwnd, WM_WATCHER_NOTIFY);
        return 0;
    }
    case IDC_BTN_BACKUP_FULL: {
        praxis_action_result_t result = praxis_hotkey_action_backup_full(hwnd, &g_profile_store, g_app.save_tree, get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_FULL_SUCCESS, result);
        show_error_toast(result);
        return 0;
    }
    case IDC_BTN_BACKUP_SLOT: {
        praxis_action_result_t result = praxis_hotkey_action_backup_slot(hwnd, &g_profile_store, g_app.save_tree, get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_SLOT_SUCCESS, result);
        show_error_toast(result);
        return 0;
    }
    case IDC_BTN_BACKUP_REPLACE: {
        praxis_action_result_t result = praxis_hotkey_action_backup_replace_selected(hwnd, &g_profile_store, g_app.save_tree,
            get_active_compression_level());
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_BACKUP_REPLACE_SUCCESS, result);
        show_error_toast(result);
        return 0;
    }
    case IDC_BTN_RESTORE: {
        praxis_action_result_t result = praxis_hotkey_action_restore(hwnd, &g_profile_store, g_app.save_tree);
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_RESTORE_SUCCESS, result);
        show_error_toast(result);
        return 0;
    }
    case IDC_BTN_UNDO: {
        praxis_action_result_t result = praxis_hotkey_action_undo(hwnd, &g_profile_store);
        if (result == PRAXIS_ACTION_OK && g_app.save_tree) save_tree_refresh(g_app.save_tree);
        update_toolbar_action_state();
        show_success_toast(STR_PRAXIS_TOAST_UNDO_SUCCESS, result);
        show_error_toast(result);
        return 0;
    }
    case IDC_BTN_ADD_BACKUP:
        handle_add_backup(hwnd, WM_WATCHER_NOTIFY);
        return 0;
    case IDC_BTN_DEL_BACKUP:
        handle_delete_backup(hwnd, WM_WATCHER_NOTIFY);
        return 0;
    case IDM_FILE_EXIT:
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case IDM_OPTIONS_HOTKEYS:
        dialog_hotkey_settings_show(hwnd);
        return 0;
    }
    return 0;
}

static void praxis_first_launch_setup(const wchar_t *ini_path) {
    profile_store_t *probe_store;
    game_profile_t game_profile;

    if (!ini_path || praxis_config.migration_dismissed) return;
    probe_store = (profile_store_t *)LocalAlloc(LMEM_FIXED, sizeof(*probe_store));
    if (!probe_store) return;
    profile_store_init(probe_store);
    profile_store_io_load(probe_store, ini_path);
    if (probe_store->game_count != 0) {
        LocalFree(probe_store);
        return;
    }
    ZeroMemory(&game_profile, sizeof(game_profile));
    game_profile.game_id = GAME_ID_ELDEN_RING;
    /* Leave game_profile.name empty so the dialog auto-fills it with a unique
     * name derived from the selected backend's display_name (e.g. "Elden Ring"). */
    if (praxis_config.tree_root[0] != L'\0' &&
        !lstrcpynW(game_profile.tree_root, praxis_config.tree_root, MAX_PATH)) {
        LocalFree(probe_store);
        return;
    }
    if (dialog_edit_game_profile_show(NULL, probe_store, &game_profile, true) == IDOK) profile_store_add_game(probe_store, &game_profile);
    else {
        praxis_config.migration_dismissed = 1;
    }
    profile_store_io_save(probe_store, ini_path);
    LocalFree(probe_store);
}

static LRESULT CALLBACK praxis_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        praxis_window_on_create(hwnd, ((CREATESTRUCTW *)lp)->hInstance);
        return (g_app.save_tree && g_app.status_bar) ? 0 : -1;

    /* Theme: dark backgrounds for child controls. */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HBRUSH br = theme_core_on_ctlcolor((HDC)wp, msg);
        if (br) {
            return (LRESULT)br;
        }
        break;
    }

    /* Theme: paint window client background with dark brush in dark mode. */
    case WM_ERASEBKGND:
        if (theme_core_on_erasebkgnd(hwnd, (HDC)wp)) {
            return 1;
        }
        break;

    /* UAH menu bar dark painting (Win10 1809+). We DO NOT intercept
     * WM_UAHMEASUREMENUITEM: DefWindowProcW must run for that message so
     * items get correct sizes. Intercepting without writing valid sizes
     * produces zero-sized items (menu visually disappears). */
    case WM_UAHDRAWMENU:
        if (theme_core_on_uah_drawmenu(hwnd, lp)) {
            return 0;
        }
        break;
    case WM_UAHDRAWMENUITEM:
        if (theme_core_on_uah_drawmenuitem(hwnd, lp)) {
            return 0;
        }
        break;

    /* Paint over the 1px light separator under the menu bar after non-client
     * paint. theme_core_paint_uah_menu_underline is a no-op in light mode. */
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        theme_core_paint_uah_menu_underline(hwnd);
        return r;
    }

    /* React to system theme, high-contrast, and system color changes. */
    case WM_SETTINGCHANGE:
        if (theme_core_on_setting_change(wp, lp)) {
            theme_core_apply_to_window_and_children(hwnd);
        }
        break;

    case WM_SYSCOLORCHANGE:
        if (theme_core_on_syscolor_change()) {
            theme_core_apply_to_window_and_children(hwnd);
        }
        break;

    case WM_THEMECHANGED:
        theme_core_apply_to_window_and_children(hwnd);
        break;

    case WM_SIZE:
        layout_main_window(wp, lp);
        if (g_app.toast) praxis_toast_recenter(g_app.toast);
        return 0;
    case WM_NOTIFY:
        {
            NMHDR *nmhdr = (NMHDR *)lp;

            if (nmhdr && g_app.save_tree && nmhdr->hwndFrom == save_tree_get_hwnd(g_app.save_tree) &&
                (nmhdr->code == TVN_SELCHANGEDW || nmhdr->code == TVN_SELCHANGEDA)) {
                update_toolbar_action_state();
                return 0;
            }
        }
        if (g_app.save_tree) {
            LRESULT notify_result = 0;
            if (save_tree_handle_notify(g_app.save_tree, (LPNMHDR)lp, &notify_result)) {
                NMHDR *nmhdr = (NMHDR *)lp;
                if (nmhdr && nmhdr->code != NM_CUSTOMDRAW) {
                    update_toolbar_action_state();
                }
                return notify_result;
            }
        }
        /* Custom-draw fallback for any tree/listview not consumed above. */
        {
            NMHDR *nmhdr = (NMHDR *)lp;
            if (nmhdr && nmhdr->code == NM_CUSTOMDRAW) {
                wchar_t cls[64];
                if (GetClassNameW(nmhdr->hwndFrom, cls, 64) > 0) {
                    if (lstrcmpiW(cls, WC_TREEVIEWW) == 0) {
                        return theme_core_on_treeview_customdraw((LPNMTVCUSTOMDRAW)lp);
                    }
                    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
                        return theme_core_on_listview_customdraw((LPNMLVCUSTOMDRAW)lp);
                    }
                    if (lstrcmpiW(cls, TOOLBARCLASSNAMEW) == 0) {
                        return theme_core_on_toolbar_customdraw((LPNMTBCUSTOMDRAW)lp);
                    }
                }
            }
        }
        break;
    case WM_WATCHER_NOTIFY:
        if (wp != 2) SetTimer(hwnd, IDT_REFRESH_DEBOUNCE, 200, NULL);
        return 0;
    case WM_TIMER:
        if (wp == IDT_REFRESH_DEBOUNCE) {
            KillTimer(hwnd, IDT_REFRESH_DEBOUNCE);
            if (g_app.save_tree) save_tree_refresh_preserve_selection(g_app.save_tree);
            update_toolbar_action_state();
        }
        return 0;
    case WM_COMMAND:
        return praxis_window_on_command(hwnd, wp);
    case WM_INITMENUPOPUP:
        praxis_main_menu_init_popup(hwnd, (HMENU)wp, &g_profile_store);
        return 0;
    case WM_HOTKEY: {
        wchar_t log_msg[64];
        if (!get_active_backend()) return 0;
        _snwprintf(log_msg, 64, L"HOTKEY_FIRED id=%d\n", (int)wp);
        log_msg[63] = L'\0';
        log_write(log_msg);
        run_hotkey_action(hwnd, (hotkey_id_t)wp);
        return 0;
    }
    case WM_CLOSE:
        persist_window_placement(hwnd);
        save_profile_store();
        hotkey_unregister_all();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_REFRESH_DEBOUNCE);
        destroy_main_children();
        praxis_theme_cleanup();
        if (g_log_file != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_file);
            g_log_file = INVALID_HANDLE_VALUE;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, LPWSTR cmd_line, int cmd_show) {
    HRESULT com_hr;
    bool com_initialized;
    INITCOMMONCONTROLSEX common_controls = { sizeof(common_controls), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES
        | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS };
    int argc = 0;
    LPWSTR *argv;
    wchar_t ini_path[MAX_PATH];
    wchar_t window_title[256];
    WNDCLASSEXW window_class = {0};
    HWND hwnd;
    MSG msg;

    (void)prev_instance;
    (void)cmd_line;
    InitCommonControlsEx(&common_controls);
    SetThemeAppProperties(STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT);
    com_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    com_initialized = SUCCEEDED(com_hr) || com_hr == S_FALSE;
    save_compress_init();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int arg_index = 1; arg_index < argc - 1; arg_index++) {
            if (wcscmp(argv[arg_index], L"--log-file") != 0) continue;
            g_log_file = CreateFileW(argv[arg_index + 1], GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            break;
        }
        LocalFree(argv);
    }

    praxis_load_config();
    praxis_locale_set_current(praxis_config.language);
    /* Initialize theme before any window creation so dark titlebar applies on first paint. */
    praxis_theme_init_from_config();
    if (config_core_get_app_ini_path(ini_path, MAX_PATH, L"Praxis.ini")) praxis_first_launch_setup(ini_path);

    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = praxis_wnd_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszMenuName = MAKEINTRESOURCEW(IDR_MAIN_MENU);
    window_class.lpszClassName = L"PRAXIS_MAIN";
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    window_class.hIconSm = window_class.hIcon;
    if (!RegisterClassExW(&window_class)) return com_initialized ? (CoUninitialize(), 1) : 1;

    praxis_window_format_title(window_title, 256);
    hwnd = CreateWindowExW(0, L"PRAXIS_MAIN", window_title, WS_OVERLAPPEDWINDOW,
        praxis_config.window_x == -1 ? CW_USEDEFAULT : praxis_config.window_x,
        praxis_config.window_y == -1 ? CW_USEDEFAULT : praxis_config.window_y,
        praxis_config.window_width == 0 ? 800 : praxis_config.window_width,
        praxis_config.window_height == 0 ? 600 : praxis_config.window_height,
        NULL, NULL, instance, NULL);
    if (!hwnd) return com_initialized ? (CoUninitialize(), 1) : 1;

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) TranslateMessage(&msg), DispatchMessageW(&msg);
    if (com_initialized) CoUninitialize();
    return (int)msg.wParam;
}
