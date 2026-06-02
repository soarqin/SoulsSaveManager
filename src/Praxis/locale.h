/**
 * @file locale.h
 * @brief Praxis locale string table declarations.
 * @details Each application owns its own string catalog. Praxis uses STR_PRAXIS_* prefix.
 */

#pragma once

#include <wchar.h>

/* Praxis string index enum — MUST use STR_PRAXIS_* prefix to avoid collision with ERSaveManager */
typedef enum praxis_string_index_e {
    STR_PRAXIS_APP_TITLE = 0,
    STR_PRAXIS_BACKUP_FULL,
    STR_PRAXIS_RESTORE_FULL,
    STR_PRAXIS_BACKUP_SLOT,
    STR_PRAXIS_RESTORE_SLOT,
    STR_PRAXIS_UNDO_RESTORE,
    STR_PRAXIS_BACKUP_REPLACE,
    STR_PRAXIS_PREVIOUS_SAVE,
    STR_PRAXIS_NEXT_SAVE,
    STR_PRAXIS_TREE_ROOT,
    STR_PRAXIS_NEW_FOLDER,
    STR_PRAXIS_RENAME,
    STR_PRAXIS_DELETE,
    STR_PRAXIS_HOTKEY_CONFLICT,
    STR_PRAXIS_GAME_RUNNING_WARNING,
    STR_PRAXIS_RING_BACKUP_FAILED,
    STR_PRAXIS_RESTORE_CONFIRMATION,
    STR_PRAXIS_LANGUAGE,
    STR_PRAXIS_OPTIONS,
    STR_PRAXIS_HOTKEY_SETTINGS,
    STR_PRAXIS_GAME,
    STR_PRAXIS_FILE,
    STR_PRAXIS_CONFIRM,
    STR_PRAXIS_CANCEL,
    STR_PRAXIS_ERROR,
    STR_PRAXIS_SUCCESS,
    STR_PRAXIS_SET_TREE_ROOT,
    STR_PRAXIS_REFRESH,
    STR_PRAXIS_EXIT,
    STR_PRAXIS_BACKUP,
    STR_PRAXIS_RESTORE,
    STR_PRAXIS_GAME_PROFILE,
    STR_PRAXIS_BACKUP_PROFILE,
    STR_PRAXIS_MANAGE_GAME_PROFILES,
    STR_PRAXIS_PROFILE_NAME,
    STR_PRAXIS_PROFILE_GAME,
    STR_PRAXIS_PROFILE_SAVE_DIR,
    STR_PRAXIS_PROFILE_TREE_ROOT,
    STR_PRAXIS_PROFILE_COMPRESSION,
    STR_PRAXIS_COMPRESSION_NONE,
    STR_PRAXIS_COMPRESSION_LOW,
    STR_PRAXIS_COMPRESSION_MEDIUM,
    STR_PRAXIS_COMPRESSION_HIGH,
    STR_PRAXIS_BTN_ADD,
    STR_PRAXIS_BTN_EDIT,
    STR_PRAXIS_BTN_DELETE,
    STR_PRAXIS_BTN_CLOSE,
    STR_PRAXIS_BTN_OK,
    STR_PRAXIS_BTN_CANCEL,
    STR_PRAXIS_BTN_RESET_DEFAULTS,
    STR_PRAXIS_PROFILE,
    STR_PRAXIS_UNKNOWN,
    STR_PRAXIS_TIP_BACKUP_FULL,
    STR_PRAXIS_TIP_BACKUP_SLOT,
    STR_PRAXIS_TIP_BACKUP_REPLACE,
    STR_PRAXIS_TIP_RESTORE,
    STR_PRAXIS_TIP_UNDO,
    STR_PRAXIS_TIP_ADD_BACKUP,
    STR_PRAXIS_TIP_DELETE_BACKUP,
    STR_PRAXIS_MIGRATION_TITLE,
    STR_PRAXIS_MIGRATION_WELCOME,
    STR_PRAXIS_MIGRATION_GAME_PAGE,
    STR_PRAXIS_MIGRATION_BACKUP_PAGE,
    STR_PRAXIS_MIGRATION_CONFIRM,
    STR_PRAXIS_CONFIRM_DELETE_GAME,
    STR_PRAXIS_CONFIRM_DELETE_BACKUP,
    STR_PRAXIS_NO_PROFILE_HINT,
    STR_PRAXIS_STATUS_ACTIVE,
    STR_PRAXIS_SHOW_IN_EXPLORER,
    STR_PRAXIS_CONFIRM_DELETE_SAVE,
    STR_PRAXIS_THEME,
    STR_PRAXIS_THEME_SYSTEM,
    STR_PRAXIS_THEME_LIGHT,
    STR_PRAXIS_THEME_DARK,
    STR_PRAXIS_SORT_NAME_ASC,
    STR_PRAXIS_SORT_NAME_DESC,
    STR_PRAXIS_SORT_MODIFIED_ASC,
    STR_PRAXIS_SORT_MODIFIED_DESC,
    STR_PRAXIS_MAKE_READ_ONLY,
    STR_PRAXIS_MAKE_WRITABLE,
    STR_PRAXIS_READ_ONLY_MARK,
    /* Toast panel notifications shown on successful backup/restore actions. */
    STR_PRAXIS_TOAST_BACKUP_FULL_SUCCESS,
    STR_PRAXIS_TOAST_BACKUP_SLOT_SUCCESS,
    STR_PRAXIS_TOAST_BACKUP_REPLACE_SUCCESS,
    STR_PRAXIS_TOAST_RESTORE_SUCCESS,
    STR_PRAXIS_TOAST_UNDO_SUCCESS,
    /* Toast error messages shown on backup/restore failure. */
    STR_PRAXIS_TOAST_ERR_NO_PROFILE,
    STR_PRAXIS_TOAST_ERR_SAVE_NOT_FOUND,
    STR_PRAXIS_TOAST_ERR_SLOT_NOT_SUPPORTED,
    STR_PRAXIS_TOAST_ERR_SLOT_EMPTY,
    STR_PRAXIS_TOAST_ERR_NO_SELECTION,
    STR_PRAXIS_TOAST_ERR_FILE_READONLY,
    STR_PRAXIS_TOAST_ERR_RING_BACKUP,
    STR_PRAXIS_TOAST_ERR_IO,
    STR_PRAXIS_TOAST_ERR_NO_UNDO,
    STR_PRAXIS_IMPORT_ORIGINAL,
    STR_PRAXIS_IMPORT_SINGLE_SLOT,
    STR_PRAXIS_IMPORT_DIALOG_TITLE,
    STR_PRAXIS_IMPORT_NO_SAVES_FOUND,
    STR_PRAXIS_IMPORT_COLUMN_NAME,
    STR_PRAXIS_IMPORT_COLUMN_PATH,
    STR_PRAXIS_IMPORT_COLUMN_TYPE,
    STR_PRAXIS_IMPORT_COLUMN_COMPRESSED,
    STR_PRAXIS_IMPORT_TYPE_FULL,
    STR_PRAXIS_IMPORT_TYPE_SLOT,
    STR_PRAXIS_IMPORT_YES,
    STR_PRAXIS_IMPORT_NO,
    STR_PRAXIS_IMPORT_BTN_IMPORT,
    STR_PRAXIS_IMPORT_SELECT_FOLDER,
    STR_PRAXIS_MAX
} praxis_string_index_t;

/**
 * @brief Gets a localized string by index.
 * @param idx Index of the string to retrieve.
 * @return Pointer to the localized wide string.
 */
const wchar_t *praxis_locale_str(praxis_string_index_t idx);

/**
 * @brief Gets the total number of available locales.
 * @return Number of available locales.
 */
int praxis_locale_count(void);

/**
 * @brief Gets the name of a locale by index.
 * @param idx Index of the locale.
 * @return Pointer to the locale name string.
 */
const wchar_t *praxis_locale_name(int idx);

/**
 * @brief Gets the current locale index.
 * @return Current locale index.
 */
int praxis_locale_get_current(void);

/**
 * @brief Sets the current locale index.
 * @param idx The locale index to set as current.
 */
void praxis_locale_set_current(int idx);

/**
 * @brief Detects the system language and returns the best matching language index.
 * @return Best matching language index.
 */
int praxis_locale_detect_system(void);
