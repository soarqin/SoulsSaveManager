/**
 * @file praxis_main_menu.c
 * @brief Dynamic main menu construction and command dispatch.
 * @details Handles WM_INITMENUPOPUP for game profile and language submenus,
 *          applies localized strings to the static menu bar, and dispatches
 *          WM_COMMAND messages for dynamic menu item IDs.
 */

#include "praxis_main_menu.h"
#include "locale.h"
#include "config.h"
#include "resource.h"
#include "praxis_window_common.h"
#include "theme.h"

#include <stdbool.h>

#include <windows.h>

/* Forward declaration for save_profile_store() defined in main.c. */
extern bool save_profile_store(void);

void praxis_main_menu_apply_locale_strings(HWND hwnd) {
    HMENU menu;
    HMENU file_menu;
    HMENU game_menu;
    HMENU options_menu;
    HMENU language_menu;

    if (!hwnd) {
        return;
    }

    menu = GetMenu(hwnd);
    if (!menu) {
        return;
    }

    file_menu    = GetSubMenu(menu, 0);
    game_menu    = GetSubMenu(menu, 1);
    options_menu = GetSubMenu(menu, 2);

    if (file_menu) {
        ModifyMenuW(menu, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)file_menu,
            praxis_locale_str(STR_PRAXIS_FILE));
        ModifyMenuW(file_menu, IDM_FILE_IMPORT_ORIGINAL, MF_BYCOMMAND | MF_STRING, IDM_FILE_IMPORT_ORIGINAL,
            praxis_locale_str(STR_PRAXIS_IMPORT_ORIGINAL));
        ModifyMenuW(file_menu, IDM_FILE_IMPORT_SLOT, MF_BYCOMMAND | MF_STRING, IDM_FILE_IMPORT_SLOT,
            praxis_locale_str(STR_PRAXIS_IMPORT_SINGLE_SLOT));
        ModifyMenuW(file_menu, IDM_FILE_EXIT, MF_BYCOMMAND | MF_STRING, IDM_FILE_EXIT,
            praxis_locale_str(STR_PRAXIS_EXIT));
    }

    if (game_menu) {
        ModifyMenuW(menu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)game_menu,
            praxis_locale_str(STR_PRAXIS_GAME));
        ModifyMenuW(game_menu, IDM_GAME_MANAGE, MF_BYCOMMAND | MF_STRING, IDM_GAME_MANAGE,
            praxis_locale_str(STR_PRAXIS_MANAGE_GAME_PROFILES));
    }

    if (options_menu) {
        ModifyMenuW(menu, 2, MF_BYPOSITION | MF_POPUP, (UINT_PTR)options_menu,
            praxis_locale_str(STR_PRAXIS_OPTIONS));
        ModifyMenuW(options_menu, IDM_OPTIONS_HOTKEYS, MF_BYCOMMAND | MF_STRING, IDM_OPTIONS_HOTKEYS,
            praxis_locale_str(STR_PRAXIS_HOTKEY_SETTINGS));

        language_menu = GetSubMenu(options_menu, 1);
        if (language_menu) {
            ModifyMenuW(options_menu, 1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)language_menu,
                praxis_locale_str(STR_PRAXIS_LANGUAGE));
        }

        /* Theme submenu at index 2 (after Hotkey Settings and Language). */
        praxis_theme_apply_locale_strings(options_menu);
    }

    DrawMenuBar(hwnd);
}

/**
 * @brief Rebuild the Game submenu from the current profile store.
 * @param hmenu  Handle to the Game popup menu.
 * @param store  Current profile store (read-only).
 *
 * Removes all dynamically inserted items, then inserts one entry per game
 * profile at the top, followed by a separator before the static Manage item.
 */
static void rebuild_game_submenu(HMENU hmenu, const profile_store_t *store) {
    /* Keep ONLY the Manage item (1 item). Remove all dynamically inserted items. */
    while (GetMenuItemCount(hmenu) > 1) {
        DeleteMenu(hmenu, 0, MF_BYPOSITION);
    }
    /* Insert game profiles at top, checking the currently active one. */
    for (int i = 0; i < (int)store->game_count; i++) {
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (store->games[i].id == store->active_game_id) {
            flags |= MF_CHECKED;
        }
        InsertMenuW(hmenu, i, flags,
                    IDM_GAME_PROFILE_FIRST + store->games[i].id,
                    store->games[i].name);
    }
    /* Insert separator BEFORE the Manage item only when there ARE profiles. */
    if (store->game_count > 0) {
        InsertMenuW(hmenu, (int)store->game_count, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }
}

/**
 * @brief Rebuild the Language submenu from the locale catalog.
 * @param hmenu  Handle to the Language popup menu.
 *
 * Wipes the entire submenu (sentinel placeholder + any prior dynamic items)
 * and repopulates it from the locale catalog, checking the active locale.
 */
static void rebuild_lang_submenu(HMENU hmenu) {
    int n   = praxis_locale_count();
    int cur = praxis_locale_get_current();

    while (GetMenuItemCount(hmenu) > 0) {
        DeleteMenu(hmenu, 0, MF_BYPOSITION);
    }
    for (int i = 0; i < n; i++) {
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (i == cur) {
            flags |= MF_CHECKED;
        }
        InsertMenuW(hmenu, i, flags,
                    (UINT_PTR)(IDM_LANG_FIRST + i),
                    praxis_locale_name(i));
    }
}

void praxis_main_menu_init_popup(HWND hwnd, HMENU hmenu, const profile_store_t *store) {
    int count = GetMenuItemCount(hmenu);
    bool is_game_menu = false;
    bool is_lang_menu = false;
    bool is_theme_menu = praxis_theme_is_theme_menu(hmenu);

    (void)hwnd; /* reserved for future use */

    /* Identify the popup by inspecting its current item IDs:
     *   - Game submenu always contains IDM_GAME_MANAGE.
     *   - Language submenu contains IDM_OPTIONS_LANG (the static
     *     "English" placeholder from the .rc) on first open, or any
     *     id in [IDM_LANG_FIRST, IDM_LANG_LAST] after rebuild.
     *   - Theme submenu identified separately via praxis_theme_is_theme_menu(). */
    if (!is_theme_menu) {
        for (int i = 0; i < count; i++) {
            UINT id = GetMenuItemID(hmenu, i);
            if (id == IDM_GAME_MANAGE) {
                is_game_menu = true;
                break;
            }
            if (id == IDM_OPTIONS_LANG ||
                (id >= IDM_LANG_FIRST && id <= IDM_LANG_LAST)) {
                is_lang_menu = true;
                break;
            }
        }
    }

    if (!is_game_menu && !is_lang_menu && !is_theme_menu) {
        return;
    }

    if (is_game_menu) {
        rebuild_game_submenu(hmenu, store);
        return;
    }

    if (is_theme_menu) {
        praxis_theme_init_popup(hmenu);
        return;
    }

    rebuild_lang_submenu(hmenu);
}

bool praxis_main_menu_handle_command(HWND hwnd, WPARAM wparam, profile_store_t *store) {
    WORD id = LOWORD(wparam);

    /* Theme submenu selection */
    if (praxis_theme_is_menu_command(id)) {
        praxis_theme_handle_menu_command(id);
        return true;
    }

    /* Dynamic Game submenu profile selection */
    if (id >= IDM_GAME_PROFILE_FIRST && id <= IDM_GAME_PROFILE_LAST) {
        int game_id = (int)(id - IDM_GAME_PROFILE_FIRST);
        const backup_profile_t *backups[1] = {0};

        profile_store_set_active_game(store, game_id);
        if (profile_store_list_backups_for_game(store, game_id, backups, 1) > 0 && backups[0]) {
            profile_store_set_active_backup(store, backups[0]->id);
        } else {
            store->active_backup_id = 0;
        }
        /* Save updated active game */
        save_profile_store();
        return true;
    }

    /* Dynamic Language submenu selection */
    if (id >= IDM_LANG_FIRST && id <= IDM_LANG_LAST) {
        int idx = (int)(id - IDM_LANG_FIRST);
        if (idx >= 0 && idx < praxis_locale_count() && idx != praxis_locale_get_current()) {
            praxis_locale_set_current(idx);
            praxis_config.language = idx;
            save_profile_store();
            /* Update window title and menu bar strings immediately without a restart.
             * The Language submenu checkmark is updated on next WM_INITMENUPOPUP. */
            praxis_window_set_title(hwnd);
            praxis_main_menu_apply_locale_strings(hwnd);
        }
        return true;
    }

    return false;
}
