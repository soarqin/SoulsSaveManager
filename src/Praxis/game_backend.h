/**
 * @file game_backend.h
 * @brief Compile-time vtable interface for game-specific save operations.
 * @details Each game backend is a static const game_backend_t instance.
 *          No runtime discovery; backends are static compile-time instances.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef enum game_id_e {
    GAME_ID_ELDEN_RING = 1,
    GAME_ID_DARK_SOULS_3 = 2,
    GAME_ID_DARK_SOULS_REMASTERED = 3,
    GAME_ID_DARK_SOULS_2_SOFS = 4,
    GAME_ID_SEKIRO = 5,
    /* Future: GAME_ID_NIGHTREIGN */
} game_id_t;

typedef struct game_backend_s {
    game_id_t id;
    const wchar_t *display_name;        /* e.g. L"Elden Ring" */
    const wchar_t *backup_extension;    /* e.g. L".ersm" (Elden Ring), L".ds3sm" (Dark Souls III) */
    const wchar_t *save_filename;       /* e.g. L"ER0000.sl2" — appended to original_save_dir */
    bool needs_game_restart;            /* false for FromSoft titles */
    bool full_save_skip_compression;    /* true = always raw-copy full saves (e.g. encrypted saves) */

    /* MANDATORY methods */
    bool (*resolve_save_path)(wchar_t *out_path, size_t out_chars);
    bool (*backup_full)(const wchar_t *src_save, const wchar_t *dst_backup, int compression_level);
    bool (*restore_full)(const wchar_t *src_backup, const wchar_t *dst_save);

    /* OPTIONAL methods (NULL if backend does not support slot-level ops) */
    bool (*get_active_slot)(const wchar_t *save_path, int *out_slot);
    bool (*backup_slot)(const wchar_t *src_save, int slot, const wchar_t *dst_backup, int compression_level);
    bool (*restore_slot)(const wchar_t *src_backup, const wchar_t *dst_save, int slot);

    /* OPTIONAL: resolve a sensible default save directory for use as a UI hint
     * (e.g. as the initial folder in the Add Game dialog's folder picker).
     * Unlike resolve_save_path, this returns a directory path, not a file path.
     * NULL if the backend cannot auto-detect a default save directory. */
    bool (*get_default_save_dir)(wchar_t *out_dir, size_t out_chars);
} game_backend_t;

/**
 * @brief Returns true if the backend supports all three slot-level operations.
 */
static inline bool game_backend_supports_slot_ops(const game_backend_t *b) {
    return b && b->get_active_slot && b->backup_slot && b->restore_slot;
}
