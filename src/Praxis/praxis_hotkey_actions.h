/**
 * @file praxis_hotkey_actions.h
 * @brief Hotkey-triggered backup/restore action handlers.
 * @details Implements the core backup/restore operations invoked from the
 *          WM_HOTKEY message and the toolbar action buttons. Each function
 *          operates on the active game/backup profile from the provided store
 *          and refreshes the save tree widget on success.
 */

#pragma once

#include <stdbool.h>
#include <windows.h>
#include "profile_store.h"
#include "save_tree.h"

/**
 * @brief Result codes for hotkey backup/restore actions.
 * @details Returned by all praxis_hotkey_action_* functions. PRAXIS_ACTION_OK
 *          means the operation completed successfully. All other values indicate
 *          a specific failure reason that should be shown to the user.
 */
typedef enum praxis_action_result_e {
    PRAXIS_ACTION_OK                    = 0,
    PRAXIS_ACTION_ERR_NO_PROFILE        = 1,  /* No active backup profile configured */
    PRAXIS_ACTION_ERR_SAVE_NOT_FOUND    = 2,  /* Game save file could not be located */
    PRAXIS_ACTION_ERR_SLOT_NOT_SUPPORTED = 3, /* Backend does not support slot-level ops */
    PRAXIS_ACTION_ERR_SLOT_EMPTY        = 4,  /* Active slot is empty or unreadable */
    PRAXIS_ACTION_ERR_NO_SELECTION      = 5,  /* No backup file selected in the tree */
    PRAXIS_ACTION_ERR_FILE_READONLY     = 6,  /* Selected backup file is read-only */
    PRAXIS_ACTION_ERR_RING_BACKUP       = 7,  /* Pre-restore ring snapshot failed */
    PRAXIS_ACTION_ERR_IO                = 8,  /* Backup/restore I/O operation failed */
    PRAXIS_ACTION_ERR_NO_UNDO           = 9,  /* No ring backup available to undo */
} praxis_action_result_t;

/** Returns true when the result indicates success. */
static inline bool praxis_action_succeeded(praxis_action_result_t r) {
    return r == PRAXIS_ACTION_OK;
}

/**
 * @brief Perform a full-save backup using the active backup profile.
 * @details Creates a timestamped backup of the active save file using the
 *          backend-defined extension (e.g. `.ersm`, `.ds3sm`). When compression_level
 *          is COMP_LEVEL_NONE the file is a byte-identical raw BND4 copy of
 *          the source save; otherwise it is an LZMA-compressed ERSM
 *          container. The format is identified by magic bytes at restore
 *          time, not by the extension.
 *          Refreshes save_tree and selects the new file on success.
 * @param hwnd Main window handle (reserved for future MessageBox feedback).
 * @param store Profile store containing the active game/backup profile.
 * @param save_tree Save tree widget to refresh and select after backup.
 * @param compression_level compression_level_t value cast to int.
 * @return PRAXIS_ACTION_OK on success, error code on failure.
 */
praxis_action_result_t praxis_hotkey_action_backup_full(HWND hwnd, profile_store_t *store,
                                                        save_tree_t *save_tree, int compression_level);

/**
 * @brief Perform an active-slot backup using the active backup profile.
 * @details Queries the backend for the currently active character slot, then
 *          creates a timestamped backup of that slot using the
 *          backend-defined extension (e.g. `.ersm`, `.ds3sm`).
 *          Refreshes save_tree and selects the new file on success.
 * @param hwnd Main window handle (reserved for future MessageBox feedback).
 * @param store Profile store containing the active game/backup profile.
 * @param save_tree Save tree widget to refresh and select after backup.
 * @param compression_level compression_level_t value cast to int.
 * @return PRAXIS_ACTION_OK on success, error code on failure.
 */
praxis_action_result_t praxis_hotkey_action_backup_slot(HWND hwnd, profile_store_t *store,
                                                        save_tree_t *save_tree, int compression_level);

/**
 * @brief Replace the selected backup with a fresh backup of the same kind.
 * @details Classifies the currently selected backup by file magic. Full-save
 *          backups are replaced with a fresh full-save backup; slot backups
 *          are replaced with a fresh backup of the currently active slot.
 *          The replacement is written to a temporary file first, then moved
 *          over the selected backup only after the new backup succeeds.
 *          Refreshes save_tree and keeps the same selected path on success.
 * @param hwnd Main window handle (reserved for future MessageBox feedback).
 * @param store Profile store containing the active game/backup profile.
 * @param save_tree Save tree widget providing the selected backup path.
 * @param compression_level compression_level_t value cast to int.
 * @return PRAXIS_ACTION_OK on success, error code on failure.
 */
praxis_action_result_t praxis_hotkey_action_backup_replace_selected(HWND hwnd, profile_store_t *store,
                                                                    save_tree_t *save_tree, int compression_level);

/**
 * @brief Restore the currently selected backup to the active save.
 * @details Takes a pre-restore ring snapshot, then auto-detects whether the
 *          selected backup is a full save or a slot save and restores
 *          accordingly. Refreshes save_tree on success.
 * @param hwnd Main window handle (reserved for future MessageBox feedback).
 * @param store Profile store containing the active game/backup profile.
 * @param save_tree Save tree widget providing the selected path; refreshed on success.
 * @return PRAXIS_ACTION_OK on success, error code on failure.
 */
praxis_action_result_t praxis_hotkey_action_restore(HWND hwnd, profile_store_t *store, save_tree_t *save_tree);

/**
 * @brief Undo the last restore by re-applying the pre-restore ring snapshot.
 * @details Reads last-restore metadata from the ring directory and calls
 *          restore_safe_undo. The caller is responsible for refreshing any
 *          UI tree widgets after a successful undo.
 * @param hwnd Main window handle (reserved for future MessageBox feedback).
 * @param store Profile store containing the active game/backup profile.
 * @return PRAXIS_ACTION_OK on success, error code on failure.
 */
praxis_action_result_t praxis_hotkey_action_undo(HWND hwnd, profile_store_t *store);
