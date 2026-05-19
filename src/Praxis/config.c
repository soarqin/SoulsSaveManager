/**
 * @file config.c
 * @brief Praxis configuration load/save using config_core toolkit.
 */

#include "config.h"

#include "config_core.h"
#include "locale.h"
#include "locale_core.h"

#include <string.h>
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#define CONFIG_MAX_BYTES (64 * 1024)

praxis_config_t praxis_config;

static void apply_defaults(void) {
    wchar_t docs[MAX_PATH] = {0};

    ZeroMemory(&praxis_config, sizeof(praxis_config));

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) {
        lstrcpyW(praxis_config.tree_root, docs);
        PathAppendW(praxis_config.tree_root, L"Praxis");
    }

    /* Default to system UI language; INI-supplied Language key overrides this. */
    praxis_config.language = praxis_locale_detect_system();
    praxis_config.window_x = -1;
    praxis_config.window_y = -1;
    praxis_config.window_width = 0;
    praxis_config.window_height = 0;
    praxis_config.compression_level = 5;
    praxis_config.ring_size = 5;
    lstrcpyW(praxis_config.hotkey_backup_full, L"Ctrl+Shift+F5");
    lstrcpyW(praxis_config.hotkey_backup_slot, L"Ctrl+Shift+F6");
    lstrcpyW(praxis_config.hotkey_restore, L"Ctrl+Shift+F9");
    lstrcpyW(praxis_config.hotkey_undo_restore, L"Ctrl+Shift+Z");
    lstrcpyW(praxis_config.hotkey_backup_replace, L"Ctrl+Shift+F7");
    lstrcpyW(praxis_config.hotkey_previous_save, L"Ctrl+Shift+Up");
    lstrcpyW(praxis_config.hotkey_next_save, L"Ctrl+Shift+Down");
    praxis_config.migration_dismissed = 0;
    praxis_config.theme = 0;  /* THEME_MODE_SYSTEM */
}

static void kv_callback(const char *key, const char *value, void *user) {
    praxis_config_t *cfg = (praxis_config_t *)user;

    if (strcmp(key, "TreeRoot") == 0) {
        config_core_store_wide_value(cfg->tree_root, MAX_PATH, value);
    } else if (strcmp(key, "Language") == 0) {
        /* Preserve existing value (system-detected default) when value is empty/missing. */
        cfg->language = config_core_parse_int(value, cfg->language);
    } else if (strcmp(key, "WindowX") == 0) {
        cfg->window_x = config_core_parse_int(value, -1);
    } else if (strcmp(key, "WindowY") == 0) {
        cfg->window_y = config_core_parse_int(value, -1);
    } else if (strcmp(key, "WindowWidth") == 0) {
        cfg->window_width = config_core_parse_int(value, 0);
    } else if (strcmp(key, "WindowHeight") == 0) {
        cfg->window_height = config_core_parse_int(value, 0);
    } else if (strcmp(key, "CompressionLevel") == 0) {
        /* [Settings] CompressionLevel is a legacy compat field; the active
         * backup profile's compression_level is the authoritative source.
         * Accept both the new string form (none/low/medium/high) and the
         * legacy integer form (1/5/9) so older INI files keep loading. */
        if (strcmp(value, "none") == 0)        cfg->compression_level = 1;
        else if (strcmp(value, "low") == 0)    cfg->compression_level = 1;
        else if (strcmp(value, "medium") == 0) cfg->compression_level = 5;
        else if (strcmp(value, "high") == 0)   cfg->compression_level = 9;
        else cfg->compression_level = config_core_parse_int(value, 5); /* legacy integer */
    } else if (strcmp(key, "RingSize") == 0) {
        cfg->ring_size = config_core_parse_int(value, 5);
    } else if (strcmp(key, "HotkeyBackupFull") == 0) {
        config_core_store_wide_value(cfg->hotkey_backup_full, 32, value);
    } else if (strcmp(key, "HotkeyBackupSlot") == 0) {
        config_core_store_wide_value(cfg->hotkey_backup_slot, 32, value);
    } else if (strcmp(key, "HotkeyRestore") == 0) {
        config_core_store_wide_value(cfg->hotkey_restore, 32, value);
    } else if (strcmp(key, "HotkeyUndoRestore") == 0) {
        config_core_store_wide_value(cfg->hotkey_undo_restore, 32, value);
    } else if (strcmp(key, "HotkeyBackupReplace") == 0) {
        config_core_store_wide_value(cfg->hotkey_backup_replace, 32, value);
    } else if (strcmp(key, "HotkeyPreviousSave") == 0) {
        config_core_store_wide_value(cfg->hotkey_previous_save, 32, value);
    } else if (strcmp(key, "HotkeyNextSave") == 0) {
        config_core_store_wide_value(cfg->hotkey_next_save, 32, value);
    } else if (strcmp(key, "MigrationDismissed") == 0) {
        cfg->migration_dismissed = config_core_parse_int(value, 0);
    } else if (strcmp(key, "Theme") == 0) {
        int v = config_core_parse_int(value, 0);
        cfg->theme = (v >= 0 && v <= 2) ? v : 0;
    }
}

void praxis_load_config(void) {
    apply_defaults();
    locale_core_set_current(praxis_config.language);

    wchar_t ini_path[MAX_PATH];
    if (!config_core_get_app_ini_path(ini_path, MAX_PATH, L"Praxis.ini")) {
        return;
    }

    HANDLE fh = CreateFileW(ini_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD file_size = GetFileSize(fh, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size == 0 || file_size > CONFIG_MAX_BYTES) {
        CloseHandle(fh);
        return;
    }

    char *buf = LocalAlloc(LMEM_FIXED, file_size + 1);
    if (!buf) {
        CloseHandle(fh);
        return;
    }

    DWORD bytes_read;
    if (!ReadFile(fh, buf, file_size, &bytes_read, NULL)) {
        LocalFree(buf);
        CloseHandle(fh);
        return;
    }
    buf[bytes_read] = '\0';
    CloseHandle(fh);

    config_core_parse_ini(buf, bytes_read, kv_callback, &praxis_config);
    LocalFree(buf);

    locale_core_set_current(praxis_config.language);
}

/*
 * praxis_save_config() removed. It wrote only the [Settings] section,
 * which destructively overwrote any [GameProfile:N]/[BackupProfile:N]
 * sections previously persisted by profile_store_io_save(). All Praxis-side
 * INI writes now go through profile_store_io_save() via save_profile_store().
 */
