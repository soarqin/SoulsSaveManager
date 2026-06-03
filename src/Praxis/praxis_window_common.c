/**
 * @file praxis_window_common.c
 * @brief Internal shared helpers for the Praxis main window.
 */

#include "praxis_window_common.h"

#include "config.h"
#include "../common/config_core.h"
#include "backend_registry.h"
#include "hotkey.h"
#include "locale.h"
#include "praxis_hotkey_actions.h"
#include "profile_store_io.h"
#include "version.h"
#include "dialogs/edit_backup_profile.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include <windows.h>
#include <commctrl.h>
#include "ui_layout.h"

void log_write(const wchar_t *msg) {
    DWORD written;
    int utf8_size;
    char *utf8;

    if (g_log_file == INVALID_HANDLE_VALUE) return;
    utf8_size = WideCharToMultiByte(CP_UTF8, 0, msg, -1, NULL, 0, NULL, NULL);
    if (utf8_size <= 0) return;
    utf8 = LocalAlloc(LMEM_FIXED, utf8_size);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, utf8_size, NULL, NULL);
    WriteFile(g_log_file, utf8, (DWORD)(utf8_size - 1), &written, NULL);
    LocalFree(utf8);
}

const game_backend_t *get_active_backend(void) {
    const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
    const game_profile_t *gp = bp ? profile_store_find_game_by_id(&g_profile_store, bp->parent_game_id) : NULL;
    const game_backend_t *backend;

    if (!gp) gp = profile_store_get_active_game(&g_profile_store);
    backend = gp ? backend_registry_get_by_id(gp->game_id) : NULL;
    return backend ? backend : backend_registry_get_default();
}

bool save_profile_store(void) {
    wchar_t ini[MAX_PATH];
    return config_core_get_app_ini_path(ini, MAX_PATH, L"Praxis.ini")
        && profile_store_io_save(&g_profile_store, ini);
}

int get_active_compression_level(void) {
    const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
    return bp ? (int)bp->compression_level : (int)COMP_LEVEL_NONE;
}

void praxis_window_format_title(wchar_t *buffer, size_t buffer_count) {
    if (!buffer || buffer_count == 0) return;
    _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%ls v%ls",
        praxis_locale_str(STR_PRAXIS_APP_TITLE), VERSION_STR_W);
}

void praxis_window_set_title(HWND hwnd) {
    wchar_t title[256];

    if (!hwnd) return;
    praxis_window_format_title(title, 256);
    SetWindowTextW(hwnd, title);
}

void populate_toolbar_profiles(void) {
    if (g_app.toolbar) toolbar_populate_profiles(g_app.toolbar, &g_profile_store);
}

void update_toolbar_action_state(void) {
    const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
    bool actions_enabled = bp != NULL;
    bool replace_enabled = actions_enabled && g_app.save_tree
        && save_tree_selected_file_can_replace(g_app.save_tree);

    if (!g_app.toolbar) {
        return;
    }

    toolbar_set_actions_enabled(g_app.toolbar, actions_enabled);
    toolbar_set_backup_replace_enabled(g_app.toolbar, replace_enabled);
}

static void set_active_status_text(void) {
    const game_profile_t *gp = NULL;
    const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
    wchar_t status[256];

    if (!g_app.status_bar) return;
    if (bp) gp = profile_store_find_game_by_id(&g_profile_store, bp->parent_game_id);
    if (!gp) gp = profile_store_get_active_game(&g_profile_store);
    if (!(gp && bp)) {
        SetWindowTextW(g_app.status_bar, praxis_locale_str(STR_PRAXIS_APP_TITLE));
        return;
    }
    _snwprintf_s(status, 256, _TRUNCATE, praxis_locale_str(STR_PRAXIS_STATUS_ACTIVE), gp->name, bp->name);
    SetWindowTextW(g_app.status_bar, status);
}

void apply_active_profile_ui(HWND hwnd, UINT watcher_notify_msg) {
    const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
    wchar_t backup_root[MAX_PATH];

    if (g_app.toolbar) {
        toolbar_set_selected_backup_id(g_app.toolbar, bp ? bp->id : 0);
        update_toolbar_action_state();
    }
    if (!bp || !profile_store_resolve_backup_root(&g_profile_store, bp->id, backup_root, MAX_PATH)) {
        set_active_status_text();
        return;
    }
    if (g_app.save_tree) {
        save_tree_set_root(g_app.save_tree, backup_root);
        update_toolbar_action_state();
    }
    if (g_app.save_watcher) save_watcher_change_root(g_app.save_watcher, backup_root);
    else g_app.save_watcher = save_watcher_start(hwnd, backup_root, watcher_notify_msg);
    set_active_status_text();
}

void destroy_main_children(void) {
    if (g_app.save_watcher) save_watcher_stop(g_app.save_watcher);
    if (g_app.toolbar) toolbar_destroy(g_app.toolbar);
    if (g_app.save_tree) save_tree_destroy(g_app.save_tree);
    if (g_app.toast) praxis_toast_destroy(g_app.toast);
    g_app.save_watcher = NULL;
    g_app.toolbar = NULL;
    g_app.save_tree = NULL;
    g_app.status_bar = NULL;
    g_app.toast = NULL;
}

void layout_main_window(WPARAM wp, LPARAM lp) {
    int client_width = (int)LOWORD(lp), client_height = (int)HIWORD(lp);
    int top_h = g_app.toolbar ? toolbar_get_top_height(g_app.toolbar) : 0;
    int bottom_h = g_app.toolbar ? toolbar_get_bottom_height(g_app.toolbar) : 0;
    int status_height = 0, bottom_y, tree_h;
    HWND tree_hwnd = g_app.save_tree ? save_tree_get_hwnd(g_app.save_tree) : NULL;

    if (g_app.toolbar) toolbar_layout_top(g_app.toolbar, client_width);
    if (g_app.status_bar) {
        RECT status_rect;
        SendMessageW(g_app.status_bar, WM_SIZE, wp, lp);
        GetWindowRect(g_app.status_bar, &status_rect);
        status_height = status_rect.bottom - status_rect.top;
    }
    bottom_y = client_height - status_height - bottom_h;
    if (bottom_y < top_h) bottom_y = top_h;
    if (g_app.toolbar) toolbar_layout_bottom(g_app.toolbar, client_width, bottom_y);
    if (!tree_hwnd) return;
    tree_h = bottom_y - top_h;
    if (tree_h < 0) tree_h = 0;

    /* Add left/right margin to the tree view */
    MoveWindow(tree_hwnd, UI_MARGIN, top_h,
               client_width - UI_MARGIN * 2, tree_h, TRUE);
}

void register_hotkeys(HWND hwnd) {
    hotkey_binding_t binding;

    hotkey_init(hwnd);
    if (hotkey_parse_string(praxis_config.hotkey_backup_full, &binding)) hotkey_register(HOTKEY_BACKUP_FULL, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_backup_slot, &binding)) hotkey_register(HOTKEY_BACKUP_SLOT, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_restore, &binding)) hotkey_register(HOTKEY_RESTORE, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_undo_restore, &binding)) hotkey_register(HOTKEY_UNDO_RESTORE, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_backup_replace, &binding)) hotkey_register(HOTKEY_BACKUP_REPLACE, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_previous_save, &binding)) hotkey_register(HOTKEY_PREVIOUS_SAVE, &binding);
    if (hotkey_parse_string(praxis_config.hotkey_next_save, &binding)) hotkey_register(HOTKEY_NEXT_SAVE, &binding);
}

void handle_profile_combo_change(HWND hwnd, UINT watcher_notify_msg) {
    int selected_id = toolbar_get_selected_backup_id(g_app.toolbar);

    if (selected_id > 0 && profile_store_set_active_backup(&g_profile_store, selected_id)) {
        const backup_profile_t *bp = profile_store_get_active_backup(&g_profile_store);
        if (bp) profile_store_set_active_game(&g_profile_store, bp->parent_game_id);
        save_profile_store();
    }
    apply_active_profile_ui(hwnd, watcher_notify_msg);
}

void handle_add_backup(HWND hwnd, UINT watcher_notify_msg) {
    const game_profile_t *gp = profile_store_get_active_game(&g_profile_store);
    backup_profile_t new_backup;
    int new_backup_id;

    if (!gp) return;
    ZeroMemory(&new_backup, sizeof(new_backup));
    new_backup.parent_game_id = gp->id;
    new_backup.compression_level = COMP_LEVEL_MEDIUM;
    if (dialog_edit_backup_profile_show(hwnd, &new_backup, true) != IDOK) return;
    new_backup_id = profile_store_add_backup(&g_profile_store, &new_backup);
    if (!new_backup_id) return;
    profile_store_set_active_backup(&g_profile_store, new_backup_id);
    save_profile_store();
    populate_toolbar_profiles();
    apply_active_profile_ui(hwnd, watcher_notify_msg);
}

void handle_delete_backup(HWND hwnd, UINT watcher_notify_msg) {
    int backup_id = toolbar_get_selected_backup_id(g_app.toolbar);

    if (!backup_id) return;
    if (MessageBoxW(hwnd, praxis_locale_str(STR_PRAXIS_CONFIRM_DELETE_BACKUP),
        praxis_locale_str(STR_PRAXIS_CONFIRM), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!profile_store_delete_backup(&g_profile_store, backup_id)) return;
    save_profile_store();
    populate_toolbar_profiles();
    apply_active_profile_ui(hwnd, watcher_notify_msg);
}

void persist_window_placement(HWND hwnd) {
    RECT window_rect;

    praxis_config.window_x = -1;
    praxis_config.window_y = -1;
    if (GetWindowRect(hwnd, &window_rect)) {
        praxis_config.window_x = window_rect.left;
        praxis_config.window_y = window_rect.top;
        praxis_config.window_width = window_rect.right - window_rect.left;
        praxis_config.window_height = window_rect.bottom - window_rect.top;
    }
    praxis_config.language = praxis_locale_get_current();
}
