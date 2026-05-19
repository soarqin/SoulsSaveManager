/**
 * @file praxis_selftest.c
 * @brief Praxis selftest dispatcher and helpers.
 * @details Houses the headless `--selftest` command dispatcher plus helper
 *          routines that are only used by selftest subcommands.
 */

#ifdef PRAXIS_ENABLE_SELFTEST

#include "praxis_selftest.h"

#include "config.h"
#include "../common/config_core.h"
#include "backend_registry.h"
#include "hotkey.h"
#include "locale.h"
#include "ring_backup.h"
#include "restore_safe.h"
#include "save_tree.h"
#include "save_watcher.h"
#include "profile_store.h"
#include "profile_store_io.h"
#include "bnd4_test_format.h"
#include "ds3_test_format.h"

#include "../common/ds3save.h"
#include "../common/ersave.h"
#include "../common/save_compress.h"
#include "../common/theme_core.h"

#include "../../deps/md5/md5.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

bool save_tree_select_sibling_file(save_tree_t *t, int direction);
#include "praxis_hotkey_actions.h"

/* Formatted wide-char printf honoring stdout redirect set by the parent process. */
static void st_printf(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    HANDLE hOut;
    DWORD type;
    DWORD written;

    va_start(args, fmt);
    _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    buf[1023] = L'\0';

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        return;
    }

    type = GetFileType(hOut);
    if (type == FILE_TYPE_CHAR) {
        WriteConsoleW(hOut, buf, (DWORD)wcslen(buf), &written, NULL);
    } else {
        int utf8_size = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
        if (utf8_size <= 0) {
            return;
        }

        char *utf8 = LocalAlloc(LMEM_FIXED, utf8_size);
        if (!utf8) {
            return;
        }

        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, utf8_size, NULL, NULL);
        WriteFile(hOut, utf8, (DWORD)(utf8_size - 1), &written, NULL);
        LocalFree(utf8);
    }
}

static bool selftest_locale_allows_english_fallback(praxis_string_index_t idx) {
    switch (idx) {
    case STR_PRAXIS_BACKUP:
    case STR_PRAXIS_OPTIONS:
    case STR_PRAXIS_FILE:
    case STR_PRAXIS_ERROR:
    case STR_PRAXIS_PROFILE_NAME:
    case STR_PRAXIS_PROFILE_COMPRESSION:
    case STR_PRAXIS_BTN_OK:
    case STR_PRAXIS_THEME_SYSTEM:
        return true;
    default:
        return false;
    }
}

/* Creates a minimal valid BND4 save file at path with the given Steam user ID.
 * Mirrors ERSaveManager's make_min_valid_sl2 for headless testing. */
static bool praxis_make_min_valid_sl2(const wchar_t *path, uint64_t user_id) {
    const uint32_t char_slot_size = BND4_TEST_CHAR_SLOT_SIZE;
    const uint32_t summary_slot_size = BND4_TEST_SUMMARY_SLOT_SIZE;
    const uint32_t slot0_offset = BND4_TEST_FILE_HEADER_SIZE;
    const uint32_t summary_data_size = BND4_TEST_SUMMARY_DATA_SIZE;
    const uint32_t face_section_size = BND4_TEST_SUMMARY_FACE_SECTION;
    const uint32_t summary_offset = slot0_offset + 10u * char_slot_size;
    const uint32_t index_offset = summary_offset + summary_slot_size;
    const uint32_t total_size = index_offset + summary_slot_size;
    const uint32_t summary_layout_size = face_section_size + BND4_TEST_SUMMARY_LAYOUT_ADJUSTMENT;
    uint8_t *file_data = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    HANDLE file;
    DWORD written;
    bool ok;

    if (!file_data) {
        return false;
    }

    CopyMemory(file_data, "BND4", 4);
    *(uint32_t *)(file_data + BND4_TEST_SLOT_COUNT_OFFSET) = 12u;

    for (int i = 0; i < 10; i++) {
        *(uint32_t *)(file_data + BND4_TEST_SLOT_SIZE_ARRAY_OFFSET + i * BND4_TEST_SLOT_ENTRY_STRIDE) = char_slot_size;
        *(uint32_t *)(file_data + BND4_TEST_SLOT_OFFSET_ARRAY_OFFSET + i * BND4_TEST_SLOT_ENTRY_STRIDE) = slot0_offset + (uint32_t)i * char_slot_size;
    }

    *(uint32_t *)(file_data + BND4_TEST_SLOT_SIZE_ARRAY_OFFSET + 10 * BND4_TEST_SLOT_ENTRY_STRIDE) = summary_slot_size;
    *(uint32_t *)(file_data + BND4_TEST_SLOT_OFFSET_ARRAY_OFFSET + 10 * BND4_TEST_SLOT_ENTRY_STRIDE) = summary_offset;
    *(uint32_t *)(file_data + BND4_TEST_SLOT_SIZE_ARRAY_OFFSET + 11 * BND4_TEST_SLOT_ENTRY_STRIDE) = summary_slot_size;
    *(uint32_t *)(file_data + BND4_TEST_SLOT_OFFSET_ARRAY_OFFSET + 11 * BND4_TEST_SLOT_ENTRY_STRIDE) = index_offset;

    {
        uint8_t *summary_payload = file_data + summary_offset + BND4_TEST_MD5_HEADER_SIZE;

        *(uint64_t *)(summary_payload + BND4_TEST_SUMMARY_USER_ID_OFFSET) = user_id;
        *(uint32_t *)(summary_payload + BND4_TEST_SUMMARY_SZ_OFFSET) = summary_layout_size;
        *(uint32_t *)(summary_payload + BND4_TEST_SUMMARY_FACE_OFFSET) = face_section_size;
        md5_buffer(summary_payload, summary_data_size, file_data + summary_offset);
    }

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(file_data);
        return false;
    }

    ok = WriteFile(file, file_data, total_size, &written, NULL) && written == total_size;
    CloseHandle(file);
    LocalFree(file_data);
    return ok;
}

/* Creates a minimal valid DS3 BND4 save file at path with the given user ID.
 * Builds 12 slots (10 char + 1 summary + 1 regulation) using REAL DS3 on-disk
 * sizes, encrypting each slot's plaintext with BCrypt AES-128-CBC PKCS7 using
 * the DS3 master key and the fixture's fixed IV. Only char slot 0 and the
 * summary slot carry meaningful data; all other slots have all-zero plaintext.
 *
 * On-disk layout per slot: [16-byte MD5][16-byte IV][ciphertext].
 * MD5 covers IV || ciphertext (DS3 convention; differs from ER which checksums
 * plaintext directly without encryption). */
static bool praxis_make_min_valid_ds3_sl2(const wchar_t *path, uint64_t userid) {
    const uint32_t header_size = DS3_BND4_FILE_HEADER_SIZE;
    const uint32_t char_size = DS3_CHAR_SLOT_ON_DISK_SIZE;
    const uint32_t summary_size = DS3_SUMMARY_SLOT_ON_DISK_SIZE;
    const uint32_t char_pt_size = DS3_CHAR_PLAINTEXT_SIZE;
    const uint32_t summary_pt_size = DS3_SUMMARY_PLAINTEXT_SIZE;
    const uint32_t char_ct_size = char_size - DS3_BND4_MD5_HEADER_SIZE - 16u;
    const uint32_t summary_ct_size = summary_size - DS3_BND4_MD5_HEADER_SIZE - 16u;
    const uint32_t total_size = header_size + 10u * char_size + summary_size + char_size;
    uint8_t *file_data = NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    ULONG result_size = 0;
    NTSTATUS status;
    HANDLE file;
    DWORD written;
    bool ok = false;
    int i;

    file_data = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    if (!file_data) {
        return false;
    }

    /* BND4 header */
    CopyMemory(file_data, "BND4", 4);
    *(uint32_t *)(file_data + DS3_BND4_SLOT_COUNT_OFFSET) = 12u;
    for (i = 0; i < 10; i++) {
        *(uint32_t *)(file_data + DS3_BND4_SLOT_SIZE_ARRAY_OFFSET + (uint32_t)i * DS3_BND4_SLOT_ENTRY_STRIDE) = char_size;
        *(uint32_t *)(file_data + DS3_BND4_SLOT_OFFSET_ARRAY_OFFSET + (uint32_t)i * DS3_BND4_SLOT_ENTRY_STRIDE) = header_size + (uint32_t)i * char_size;
    }
    /* Slot 10: summary */
    *(uint32_t *)(file_data + DS3_BND4_SLOT_SIZE_ARRAY_OFFSET + 10u * DS3_BND4_SLOT_ENTRY_STRIDE) = summary_size;
    *(uint32_t *)(file_data + DS3_BND4_SLOT_OFFSET_ARRAY_OFFSET + 10u * DS3_BND4_SLOT_ENTRY_STRIDE) = header_size + 10u * char_size;
    /* Slot 11: regulation (same on-disk size as char) */
    *(uint32_t *)(file_data + DS3_BND4_SLOT_SIZE_ARRAY_OFFSET + 11u * DS3_BND4_SLOT_ENTRY_STRIDE) = char_size;
    *(uint32_t *)(file_data + DS3_BND4_SLOT_OFFSET_ARRAY_OFFSET + 11u * DS3_BND4_SLOT_ENTRY_STRIDE) = header_size + 10u * char_size + summary_size;

    /* BCrypt setup: AES-128-CBC with DS3 master key */
    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        LocalFree(file_data);
        return false;
    }
    status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(file_data);
        return false;
    }
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&key_obj_size,
                                sizeof(ULONG), &result_size, 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(file_data);
        return false;
    }
    key_obj = (uint8_t *)LocalAlloc(LMEM_FIXED, key_obj_size);
    if (!key_obj) {
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(file_data);
        return false;
    }
    status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size,
                                        (PUCHAR)DS3_AES_KEY_BYTES, 16, 0);
    if (!NT_SUCCESS(status)) {
        LocalFree(key_obj);
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(file_data);
        return false;
    }

    /* Encrypt each of the 12 slots */
    ok = true;
    for (i = 0; i < 12 && ok; i++) {
        uint32_t slot_offset;
        uint32_t pt_size;
        uint32_t ct_size;
        uint8_t *plaintext;
        uint8_t *md5_buf;
        uint8_t iv_scratch[16];
        uint8_t *ct_buf;
        ULONG ct_written = 0;

        if (i < 10) {
            slot_offset = header_size + (uint32_t)i * char_size;
            pt_size = char_pt_size;
            ct_size = char_ct_size;
        } else if (i == 10) {
            slot_offset = header_size + 10u * char_size;
            pt_size = summary_pt_size;
            ct_size = summary_ct_size;
        } else {
            slot_offset = header_size + 10u * char_size + summary_size;
            pt_size = char_pt_size;
            ct_size = char_ct_size;
        }

        plaintext = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, pt_size);
        if (!plaintext) {
            ok = false;
            break;
        }

        /* Populate slot-specific fields in plaintext */
        if (i == 0) {
            /* Char slot 0: N=0x100 at offset 0x58, userid at N+0x6F */
            *(uint32_t *)(plaintext + DS3_CHAR_USERID_LEN_OFFSET) = 0x100u;
            *(uint64_t *)(plaintext + 0x100u + DS3_CHAR_USERID_DELTA) = userid;
        } else if (i == 10) {
            /* Summary slot */
            *(uint64_t *)(plaintext + DS3_SUMMARY_USERID_OFFSET) = userid;
            *(int32_t *)(plaintext + DS3_SUMMARY_ACTIVE_OFFSET) = 0;
            plaintext[DS3_SUMMARY_AVAILABLE_OFFSET] = 1;
        }

        /* Encrypt into file buffer at slot_offset + 32 (after MD5 + IV) */
        ct_buf = file_data + slot_offset + DS3_BND4_MD5_HEADER_SIZE + 16u;
        CopyMemory(iv_scratch, DS3_TEST_IV, 16);
        status = BCryptEncrypt(key, (PUCHAR)plaintext, pt_size, NULL, iv_scratch, 16,
                                ct_buf, ct_size, &ct_written, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(status) || ct_written != ct_size) {
            LocalFree(plaintext);
            ok = false;
            break;
        }

        /* Write IV right after MD5 header in the file buffer */
        CopyMemory(file_data + slot_offset + DS3_BND4_MD5_HEADER_SIZE, DS3_TEST_IV, 16);

        /* MD5(IV || ciphertext) into the slot's MD5 header */
        md5_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, 16u + ct_size);
        if (!md5_buf) {
            LocalFree(plaintext);
            ok = false;
            break;
        }
        CopyMemory(md5_buf, DS3_TEST_IV, 16);
        CopyMemory(md5_buf + 16, ct_buf, ct_size);
        md5_buffer(md5_buf, 16u + ct_size, file_data + slot_offset);
        LocalFree(md5_buf);

        LocalFree(plaintext);
    }

    /* BCrypt cleanup */
    BCryptDestroyKey(key);
    LocalFree(key_obj);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (!ok) {
        LocalFree(file_data);
        return false;
    }

    /* Write file */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(file_data);
        return false;
    }
    ok = WriteFile(file, file_data, total_size, &written, NULL) && written == total_size;
    CloseHandle(file);
    LocalFree(file_data);
    return ok;
}

static bool praxis_make_min_valid_sl2_with_slot(const wchar_t *path, uint64_t user_id,
                                                int slot, const wchar_t *name) {
    const uint32_t summary_slot_size = BND4_TEST_SUMMARY_SLOT_SIZE;
    const uint32_t summary_offset = BND4_TEST_FILE_HEADER_SIZE + 10u * BND4_TEST_CHAR_SLOT_SIZE;
    const uint32_t summary_layout_size = BND4_TEST_SUMMARY_FACE_SECTION + BND4_TEST_SUMMARY_LAYOUT_ADJUSTMENT;
    const uint32_t available_offset = BND4_TEST_SUMMARY_SZ_OFFSET + 4u + summary_layout_size;
    const uint32_t profile_offset = available_offset + 10u;
    HANDLE file;
    uint8_t *summary_payload;
    DWORD bytes_read;
    DWORD written;
    uint8_t md5[16];
    bool ok;

    if (slot < 0 || slot >= 10 || !praxis_make_min_valid_sl2(path, user_id)) {
        return false;
    }

    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    summary_payload = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, BND4_TEST_SUMMARY_DATA_SIZE);
    if (!summary_payload) {
        CloseHandle(file);
        return false;
    }

    if (SetFilePointer(file, summary_offset + BND4_TEST_MD5_HEADER_SIZE, NULL, FILE_BEGIN)
            != summary_offset + BND4_TEST_MD5_HEADER_SIZE
        || !ReadFile(file, summary_payload, BND4_TEST_SUMMARY_DATA_SIZE, &bytes_read, NULL)
        || bytes_read != BND4_TEST_SUMMARY_DATA_SIZE) {
        LocalFree(summary_payload);
        CloseHandle(file);
        return false;
    }

    summary_payload[available_offset + (uint32_t)slot] = 1;
    if (name) {
        lstrcpynW((wchar_t *)(summary_payload + profile_offset + BND4_TEST_PROFILE_SIZE * (uint32_t)slot),
                  name, BND4_TEST_CHAR_NAME_SIZE / sizeof(wchar_t));
    }

    md5_buffer(summary_payload, BND4_TEST_SUMMARY_DATA_SIZE, md5);
    ok = SetFilePointer(file, summary_offset, NULL, FILE_BEGIN) == summary_offset
        && WriteFile(file, md5, sizeof(md5), &written, NULL)
        && written == sizeof(md5)
        && SetFilePointer(file, summary_offset + BND4_TEST_MD5_HEADER_SIZE, NULL, FILE_BEGIN)
            == summary_offset + BND4_TEST_MD5_HEADER_SIZE
        && WriteFile(file, summary_payload, summary_slot_size - BND4_TEST_MD5_HEADER_SIZE, &written, NULL)
        && written == summary_slot_size - BND4_TEST_MD5_HEADER_SIZE;

    LocalFree(summary_payload);
    CloseHandle(file);
    return ok;
}

static int selftest_make_valid_ersm(const wchar_t *path) {
    uint8_t buf[16] = {0};

    return ersm_compress_to_file(path, buf, sizeof(buf), ERSM_TYPE_CHAR_SLOT, ERSM_LEVEL_NORMAL) ? 0 : 1;
}

static bool selftest_delete_tree_path(const wchar_t *full_path) {
    wchar_t delete_buf[MAX_PATH + 2];
    SHFILEOPSTRUCTW op = {0};

    if (!full_path || full_path[0] == L'\0' || lstrlenW(full_path) >= MAX_PATH + 1) {
        return false;
    }

    lstrcpyW(delete_buf, full_path);
    delete_buf[lstrlenW(delete_buf) + 1] = L'\0';

    op.wFunc = FO_DELETE;
    op.pFrom = delete_buf;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static HTREEITEM selftest_find_child_by_text(HWND hwnd, HTREEITEM parent, const wchar_t *text) {
    HTREEITEM tree_item;
    wchar_t label[MAX_PATH];

    if (!hwnd || !text) {
        return NULL;
    }

    tree_item = parent ? TreeView_GetChild(hwnd, parent) : TreeView_GetRoot(hwnd);
    while (tree_item) {
        TVITEMW tvi = {0};

        label[0] = L'\0';
        tvi.hItem = tree_item;
        tvi.mask = TVIF_TEXT;
        tvi.pszText = label;
        tvi.cchTextMax = MAX_PATH;
        if (TreeView_GetItem(hwnd, &tvi) && lstrcmpW(label, text) == 0) {
            return tree_item;
        }

        tree_item = TreeView_GetNextSibling(hwnd, tree_item);
    }

    return NULL;
}

static HTREEITEM selftest_find_tree_item_by_relpath(HWND hwnd, const wchar_t *relpath) {
    HTREEITEM tree_item;
    wchar_t path_copy[MAX_PATH];
    wchar_t *context = NULL;
    wchar_t *part;

    if (!hwnd) {
        return NULL;
    }

    tree_item = TreeView_GetRoot(hwnd);
    if (!tree_item || !relpath || relpath[0] == L'\0') {
        return tree_item;
    }

    lstrcpynW(path_copy, relpath, MAX_PATH);
    part = wcstok_s(path_copy, L"\\", &context);
    while (part) {
        tree_item = selftest_find_child_by_text(hwnd, tree_item, part);
        if (!tree_item) {
            return NULL;
        }

        part = wcstok_s(NULL, L"\\", &context);
    }

    return tree_item;
}

static bool selftest_build_tree_path(const wchar_t *root, const wchar_t *relpath, wchar_t *out, size_t out_chars) {
    if (!root || !out || out_chars == 0) {
        return false;
    }

    if ((size_t)lstrlenW(root) >= out_chars) {
        return false;
    }

    lstrcpyW(out, root);
    if (relpath && relpath[0] != L'\0' && !PathAppendW(out, relpath)) {
        return false;
    }

    return true;
}

static void selftest_walk_up_existing_relpath(const wchar_t *root, const wchar_t *start_relpath,
    wchar_t *out_relpath, size_t out_chars) {
    wchar_t try_relpath[MAX_PATH];
    wchar_t full_path[MAX_PATH];

    if (!out_relpath || out_chars == 0) {
        return;
    }

    out_relpath[0] = L'\0';
    if (!root || !start_relpath || start_relpath[0] == L'\0') {
        return;
    }

    lstrcpynW(try_relpath, start_relpath, MAX_PATH);
    while (true) {
        if (selftest_build_tree_path(root, try_relpath, full_path, MAX_PATH) && PathFileExistsW(full_path)) {
            lstrcpynW(out_relpath, try_relpath, out_chars);
            return;
        }

        {
            wchar_t *last_sep = wcsrchr(try_relpath, L'\\');
            if (last_sep) {
                *last_sep = L'\0';
            } else {
                break;
            }
        }
    }
}

/* Selftest-only INI callback: parses config keys for the config-load subcommand. */
static void selftest_config_kv_cb(const char *key, const char *value, void *user) {
    praxis_config_t *cfg = (praxis_config_t *)user;

    if (strcmp(key, "TreeRoot") == 0) {
        config_core_store_wide_value(cfg->tree_root, MAX_PATH, value);
    } else if (strcmp(key, "Language") == 0) {
        cfg->language = config_core_parse_int(value, 0);
    } else if (strcmp(key, "HotkeyBackupFull") == 0) {
        config_core_store_wide_value(cfg->hotkey_backup_full, 32, value);
    } else if (strcmp(key, "HotkeyBackupReplace") == 0) {
        config_core_store_wide_value(cfg->hotkey_backup_replace, 32, value);
    } else if (strcmp(key, "HotkeyPreviousSave") == 0) {
        config_core_store_wide_value(cfg->hotkey_previous_save, 32, value);
    } else if (strcmp(key, "HotkeyNextSave") == 0) {
        config_core_store_wide_value(cfg->hotkey_next_save, 32, value);
    } else if (strcmp(key, "RingSize") == 0) {
        cfg->ring_size = config_core_parse_int(value, 5);
    } else if (strcmp(key, "CompressionLevel") == 0) {
        cfg->compression_level = config_core_parse_int(value, 5);
    }
}

static bool selftest_parse_tree_sort_mode(const wchar_t *value, save_tree_sort_mode_t *out_mode) {
    if (!value || !out_mode) {
        return false;
    }

    if (wcscmp(value, L"name-asc") == 0) {
        *out_mode = SAVE_TREE_SORT_NAME_ASC;
        return true;
    }
    if (wcscmp(value, L"name-desc") == 0) {
        *out_mode = SAVE_TREE_SORT_NAME_DESC;
        return true;
    }
    if (wcscmp(value, L"modified-asc") == 0) {
        *out_mode = SAVE_TREE_SORT_MODIFIED_ASC;
        return true;
    }
    if (wcscmp(value, L"modified-desc") == 0) {
        *out_mode = SAVE_TREE_SORT_MODIFIED_DESC;
        return true;
    }

    return false;
}

int praxis_selftest_run(int argc, wchar_t **argv) {
    int exit_code;

    if (!argv || argc < 3) {
        st_printf(L"usage: --selftest <subcommand> [args...]\n");
        return 2;
    }

    {
        const wchar_t *sub = argv[2];

        if (wcscmp(sub, L"smoke") == 0) {
            praxis_load_config();
            save_compress_init();
            st_printf(L"praxis_smoke_ok\n");
            exit_code = 0;
        } else if (wcscmp(sub, L"dump-default-backend") == 0) {
            const game_backend_t *b = backend_registry_get_default();
            if (!b) {
                st_printf(L"no default backend\n");
                exit_code = 1;
            } else {
                st_printf(L"%ls\n", b->display_name);
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"provision-sl2") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest provision-sl2 <output_path>\n");
                exit_code = 2;
            } else {
                exit_code = praxis_make_min_valid_sl2(argv[3], 76561199999999999ULL) ? 0 : 1;
            }
        } else if (wcscmp(sub, L"provision-ds3-sl2") == 0) {
            /* Create a minimal valid DS3 fixture at <output_path> and leave it on disk. */
            if (argc < 4) {
                st_printf(L"usage: --selftest provision-ds3-sl2 <output_path>\n");
                exit_code = 2;
            } else {
                exit_code = praxis_make_min_valid_ds3_sl2(argv[3], DS3_TEST_USERID_A) ? 0 : 1;
            }
        } else if (wcscmp(sub, L"ds3-backup-slot") == 0) {
            /* Diagnose DS3 slot backup step by step.
             * Usage: ds3-backup-slot <src_save> <slot> <dst_backup> */
            if (argc < 6) {
                st_printf(L"usage: --selftest ds3-backup-slot <src_save> <slot> <dst_backup>\n");
                exit_code = 2;
            } else {
                int slot = _wtoi(argv[4]);
                ds3_save_data_t *save = ds3_save_data_load(argv[3]);
                if (!save) {
                    st_printf(L"FAIL: step 1 ds3_save_data_load returned NULL\n");
                    exit_code = 1;
                } else {
                    const ds3_char_data_t *cd = ds3_char_data_ref(save, slot);
                    if (!cd) {
                        st_printf(L"FAIL: step 2 ds3_char_data_ref(slot=%d) returned NULL\n", slot);
                        ds3_save_data_free(save);
                        exit_code = 1;
                    } else {
                        uint8_t *buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
                        if (!buf) {
                            st_printf(L"FAIL: step 3 LocalAlloc failed\n");
                            ds3_save_data_free(save);
                            exit_code = 1;
                        } else if (!ds3_char_data_serialize(cd, buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                            st_printf(L"FAIL: step 4 ds3_char_data_serialize failed\n");
                            LocalFree(buf);
                            ds3_save_data_free(save);
                            exit_code = 1;
                        } else {
                            ds3_save_data_free(save);
                            st_printf(L"OK: steps 1-4 passed, serialized %u bytes\n",
                                (unsigned)DS3_CHAR_DATA_SERIALIZED_SIZE);
                            bool ok = ersm_compress_to_file(argv[5], buf,
                                DS3_CHAR_DATA_SERIALIZED_SIZE, ERSM_TYPE_CHAR_SLOT, 5);
                            LocalFree(buf);
                            if (!ok) {
                                st_printf(L"FAIL: step 5 ersm_compress_to_file failed\n");
                                exit_code = 1;
                            } else {
                                st_printf(L"PASS: ds3-backup-slot (slot=%d)\n", slot);
                                exit_code = 0;
                            }
                        }
                    }
                }
            }
        } else if (wcscmp(sub, L"char-set-name-profile") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest char-set-name-profile <save_path>\n");
                exit_code = 2;
            } else {
                er_save_data_t *save;
                const er_char_data_t *slot_data;
                uint8_t *flat;

                exit_code = 1;
                if (!praxis_make_min_valid_sl2_with_slot(argv[3], 76561199999999999ULL, 0, L"Initial")) {
                    st_printf(L"char-set-name-profile: fixture failed\n");
                } else {
                    save = er_save_data_load(argv[3]);
                    if (!save) {
                        st_printf(L"char-set-name-profile: load failed\n");
                    } else if (!er_char_data_set_name(save, 0, L"Renamed")) {
                        st_printf(L"char-set-name-profile: set-name failed\n");
                    } else {
                        slot_data = er_char_data_ref(save, 0);
                        flat = (uint8_t *)LocalAlloc(LMEM_FIXED, BND4_TEST_CHAR_DATA_SIZE + BND4_TEST_PROFILE_SIZE);
                        if (!slot_data || !flat) {
                            st_printf(L"char-set-name-profile: export setup failed\n");
                        } else if (!er_char_data_serialize(slot_data, flat,
                                   BND4_TEST_CHAR_DATA_SIZE + BND4_TEST_PROFILE_SIZE)) {
                            st_printf(L"char-set-name-profile: serialize failed\n");
                        } else if (lstrcmpW((const wchar_t *)(flat + BND4_TEST_CHAR_DATA_SIZE),
                                            L"Renamed") != 0) {
                            st_printf(L"char-set-name-profile: profile name mismatch\n");
                        } else {
                            st_printf(L"char-set-name-profile: ok\n");
                            exit_code = 0;
                        }
                        if (flat) {
                            LocalFree(flat);
                        }
                    }
                    if (save) {
                        er_save_data_free(save);
                    }
                }
            }
        } else if (wcscmp(sub, L"ersave-null-guards") == 0) {
            uint8_t dummy_face[BND4_TEST_CHAR_NAME_SIZE] = {0};

            exit_code = 0;
            if (er_save_simple_data_get_char_name(NULL, 0) != NULL) {
                st_printf(L"ersave-null-guards: simple name should be NULL\n");
                exit_code = 1;
            } else if (er_save_simple_data_slot_export(NULL, 0) != NULL) {
                st_printf(L"ersave-null-guards: simple export should be NULL\n");
                exit_code = 1;
            } else if (er_char_data_import(NULL, 0, NULL)) {
                st_printf(L"ersave-null-guards: char import should fail\n");
                exit_code = 1;
            } else if (er_char_data_set_name(NULL, 0, L"Name")) {
                st_printf(L"ersave-null-guards: set name should fail\n");
                exit_code = 1;
            } else if (er_face_data_ref(NULL, 0) != NULL) {
                st_printf(L"ersave-null-guards: face ref should be NULL\n");
                exit_code = 1;
            } else if (er_face_data_import(NULL, 0, NULL)) {
                st_printf(L"ersave-null-guards: face import NULL should fail\n");
                exit_code = 1;
            } else if (er_face_data_import(NULL, 0, dummy_face)) {
                st_printf(L"ersave-null-guards: face import save NULL should fail\n");
                exit_code = 1;
            } else if (er_face_data_to_file(NULL, L"NUL")) {
                st_printf(L"ersave-null-guards: face write NULL should fail\n");
                exit_code = 1;
            } else {
                st_printf(L"ersave-null-guards: ok\n");
            }
        } else if (wcscmp(sub, L"backup-full-headless") == 0) {
            if (argc < 5) {
                st_printf(L"usage: --selftest backup-full-headless <src_sl2> <dst_ersm>\n");
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                if (!b) {
                    st_printf(L"no backend\n");
                    exit_code = 1;
                } else {
                    exit_code = b->backup_full(argv[3], argv[4], 5) ? 0 : 1;
                }
            }
        } else if (wcscmp(sub, L"restore-full-headless") == 0) {
            if (argc < 5) {
                st_printf(L"usage: --selftest restore-full-headless <src_backup> <dst_sl2>\n");
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                if (!b) {
                    st_printf(L"no backend\n");
                    exit_code = 1;
                } else {
                    exit_code = b->restore_full(argv[3], argv[4]) ? 0 : 1;
                }
            }
        } else if (wcscmp(sub, L"backup-slot-headless") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                int slot = _wtoi(argv[4]);
                exit_code = (b && b->backup_slot) ? (b->backup_slot(argv[3], slot, argv[5], 5) ? 0 : 1) : 1;
            }
        } else if (wcscmp(sub, L"restore-slot-headless") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                int slot = _wtoi(argv[5]);
                exit_code = (b && b->restore_slot) ? (b->restore_slot(argv[3], argv[4], slot) ? 0 : 1) : 1;
            }
        } else if (wcscmp(sub, L"dump-active-slot-praxis") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                int slot = -1;
                if (b && b->get_active_slot && b->get_active_slot(argv[3], &slot)) {
                    st_printf(L"active_slot=%d\n", slot);
                    exit_code = 0;
                } else {
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"hotkey-validate") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                hotkey_binding_t b;

                exit_code = hotkey_parse_string(argv[3], &b) ? 0 : 1;
                if (exit_code == 0) {
                    wchar_t str[64];
                    hotkey_to_string(&b, str, 64);
                    st_printf(L"parsed: %ls\n", str);
                }
            }
        } else if (wcscmp(sub, L"make-valid-ersm") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                exit_code = selftest_make_valid_ersm(argv[3]);
            }
        } else if (wcscmp(sub, L"tree-populate") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    st_printf(L"items=%d\n", save_tree_item_count(t));
                    save_tree_destroy(t);
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"tree-preserve-selection-walkup") == 0) {
            if (argc < 6) {
                st_printf(L"usage: --selftest tree-preserve-selection-walkup <root> <select_path> <delete_path>\n");
                exit_code = 2;
            } else {
                HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
                save_tree_t *t;

                if (!host) {
                    exit_code = 1;
                } else {
                    t = save_tree_create(host, GetModuleHandleW(NULL), 0);
                    if (!t) {
                        DestroyWindow(host);
                        exit_code = 1;
                    } else {
                        HWND tree_hwnd = save_tree_get_hwnd(t);
                        HTREEITEM select_item;
                        wchar_t delete_full[MAX_PATH];
                        wchar_t expected_relpath[MAX_PATH];
                        wchar_t expected_full[MAX_PATH];
                        wchar_t selected_full[MAX_PATH];

                        save_tree_set_root(t, argv[3]);
                        select_item = selftest_find_tree_item_by_relpath(tree_hwnd, argv[4]);
                        if (!select_item) {
                            st_printf(L"tree-preserve-selection-walkup: selection not found\n");
                            exit_code = 1;
                        } else if (!TreeView_SelectItem(tree_hwnd, select_item)) {
                            st_printf(L"tree-preserve-selection-walkup: selection failed\n");
                            exit_code = 1;
                        } else if (!selftest_build_tree_path(argv[3], argv[5], delete_full, MAX_PATH)) {
                            exit_code = 1;
                        } else if (!selftest_delete_tree_path(delete_full)) {
                            st_printf(L"tree-preserve-selection-walkup: delete failed\n");
                            exit_code = 1;
                        } else {
                            selftest_walk_up_existing_relpath(argv[3], argv[4], expected_relpath, MAX_PATH);
                            if (!selftest_build_tree_path(argv[3], expected_relpath, expected_full, MAX_PATH)) {
                                exit_code = 1;
                            } else {
                                save_tree_refresh_preserve_selection(t);
                                if (!save_tree_get_selected_path(t, selected_full, MAX_PATH)) {
                                    st_printf(L"tree-preserve-selection-walkup: no selection after refresh\n");
                                    exit_code = 1;
                                } else if (lstrcmpW(selected_full, expected_full) != 0) {
                                    st_printf(L"expected=%ls\nselected=%ls\n", expected_full, selected_full);
                                    exit_code = 1;
                                } else {
                                    st_printf(L"selected=%ls\n", selected_full);
                                    exit_code = 0;
                                }
                            }
                        }

                        save_tree_destroy(t);
                        DestroyWindow(host);
                    }
                }
            }
        } else if (wcscmp(sub, L"tree-cycle-sibling-files") == 0) {
            if (argc < 7) {
                st_printf(L"usage: --selftest tree-cycle-sibling-files <root> <select_relpath> <direction> <expected_relpath>\n");
                exit_code = 2;
            } else {
                HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
                save_tree_t *t;

                if (!host) {
                    exit_code = 1;
                } else {
                    t = save_tree_create(host, GetModuleHandleW(NULL), 0);
                    if (!t) {
                        DestroyWindow(host);
                        exit_code = 1;
                    } else {
                        wchar_t select_full[MAX_PATH];
                        wchar_t expected_full[MAX_PATH];
                        wchar_t selected_full[MAX_PATH];
                        int direction = _wtoi(argv[5]);

                        save_tree_set_root(t, argv[3]);
                        if (!selftest_build_tree_path(argv[3], argv[4], select_full, MAX_PATH) ||
                            !save_tree_select_full_path(t, select_full)) {
                            st_printf(L"tree-cycle-sibling-files: selection failed\n");
                            exit_code = 1;
                        } else if (!save_tree_select_sibling_file(t, direction)) {
                            st_printf(L"tree-cycle-sibling-files: cycle failed\n");
                            exit_code = 1;
                        } else if (!selftest_build_tree_path(argv[3], argv[6], expected_full, MAX_PATH)) {
                            exit_code = 1;
                        } else if (!save_tree_get_selected_path(t, selected_full, MAX_PATH)) {
                            st_printf(L"tree-cycle-sibling-files: no selected path\n");
                            exit_code = 1;
                        } else if (lstrcmpW(selected_full, expected_full) != 0) {
                            st_printf(L"expected=%ls\nselected=%ls\n", expected_full, selected_full);
                            exit_code = 1;
                        } else {
                            st_printf(L"selected=%ls\n", selected_full);
                            exit_code = 0;
                        }

                        save_tree_destroy(t);
                        DestroyWindow(host);
                    }
                }
            }
        } else if (wcscmp(sub, L"tree-cycle-sibling-files-sorted") == 0) {
            if (argc < 8) {
                st_printf(L"usage: --selftest tree-cycle-sibling-files-sorted <root> <sort_mode> <select_relpath> <direction> <expected_relpath>\n");
                exit_code = 2;
            } else {
                HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
                save_tree_t *t;
                save_tree_sort_mode_t sort_mode;

                if (!selftest_parse_tree_sort_mode(argv[4], &sort_mode)) {
                    st_printf(L"tree-cycle-sibling-files-sorted: invalid sort mode\n");
                    exit_code = 2;
                } else if (!host) {
                    exit_code = 1;
                } else {
                    t = save_tree_create(host, GetModuleHandleW(NULL), 0);
                    if (!t) {
                        DestroyWindow(host);
                        exit_code = 1;
                    } else {
                        wchar_t select_full[MAX_PATH];
                        wchar_t expected_full[MAX_PATH];
                        wchar_t selected_full[MAX_PATH];
                        int direction = _wtoi(argv[6]);

                        save_tree_set_sort_mode(t, sort_mode);
                        save_tree_set_root(t, argv[3]);
                        if (!selftest_build_tree_path(argv[3], argv[5], select_full, MAX_PATH) ||
                            !save_tree_select_full_path(t, select_full)) {
                            st_printf(L"tree-cycle-sibling-files-sorted: selection failed\n");
                            exit_code = 1;
                        } else if (!save_tree_select_sibling_file(t, direction)) {
                            st_printf(L"tree-cycle-sibling-files-sorted: cycle failed\n");
                            exit_code = 1;
                        } else if (!selftest_build_tree_path(argv[3], argv[7], expected_full, MAX_PATH)) {
                            exit_code = 1;
                        } else if (!save_tree_get_selected_path(t, selected_full, MAX_PATH)) {
                            st_printf(L"tree-cycle-sibling-files-sorted: no selected path\n");
                            exit_code = 1;
                        } else if (lstrcmpW(selected_full, expected_full) != 0) {
                            st_printf(L"expected=%ls\nselected=%ls\n", expected_full, selected_full);
                            exit_code = 1;
                        } else {
                            st_printf(L"selected=%ls\n", selected_full);
                            exit_code = 0;
                        }

                        save_tree_destroy(t);
                        DestroyWindow(host);
                    }
                }
            }
        } else if (wcscmp(sub, L"tree-readonly-toggle") == 0) {
            if (argc < 5) {
                st_printf(L"usage: --selftest tree-readonly-toggle <root> <relpath>\n");
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                wchar_t full[MAX_PATH];

                if (!t || !selftest_build_tree_path(argv[3], argv[4], full, MAX_PATH)) {
                    if (t) {
                        save_tree_destroy(t);
                    }
                    exit_code = 1;
                } else {
                    DWORD attrs;

                    save_tree_set_root(t, argv[3]);
                    SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                    if (!save_tree_set_file_readonly(t, argv[4], true)) {
                        st_printf(L"tree-readonly-toggle: set failed\n");
                        exit_code = 1;
                    } else {
                        attrs = GetFileAttributesW(full);
                        if (attrs == INVALID_FILE_ATTRIBUTES ||
                            (attrs & FILE_ATTRIBUTE_READONLY) == 0) {
                            st_printf(L"tree-readonly-toggle: readonly bit missing\n");
                            exit_code = 1;
                        } else if (!save_tree_set_file_readonly(t, argv[4], false)) {
                            st_printf(L"tree-readonly-toggle: clear failed\n");
                            exit_code = 1;
                        } else {
                            attrs = GetFileAttributesW(full);
                            if (attrs == INVALID_FILE_ATTRIBUTES ||
                                (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
                                st_printf(L"tree-readonly-toggle: readonly bit still set\n");
                                exit_code = 1;
                            } else {
                                st_printf(L"tree-readonly-toggle: ok\n");
                                exit_code = 0;
                            }
                        }
                    }

                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"tree-readonly-reject-folder") == 0) {
            if (argc < 5) {
                st_printf(L"usage: --selftest tree-readonly-reject-folder <root> <relpath>\n");
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);

                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    if (save_tree_set_file_readonly(t, argv[4], true)) {
                        st_printf(L"tree-readonly-reject-folder: folder was changed\n");
                        exit_code = 1;
                    } else {
                        st_printf(L"tree-readonly-reject-folder: ok\n");
                        exit_code = 0;
                    }

                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"tree-rename") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    exit_code = save_tree_rename(t, argv[4], argv[5]) ? 0 : 1;
                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"tree-delete") == 0) {
            if (argc < 5) {
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    exit_code = save_tree_delete(t, argv[4]) ? 0 : 1;
                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"tree-new-folder") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    exit_code = save_tree_new_folder(t, argv[4], argv[5]) ? 0 : 1;
                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"tree-move") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                save_tree_t *t = save_tree_create(NULL, NULL, 0);
                if (!t) {
                    exit_code = 1;
                } else {
                    save_tree_set_root(t, argv[3]);
                    exit_code = save_tree_move(t, argv[4], argv[5]) ? 0 : 1;
                    save_tree_destroy(t);
                }
            }
        } else if (wcscmp(sub, L"ring-snapshot") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                wchar_t out[MAX_PATH];

                ring_backup_init(argv[3], 5);
                exit_code = ring_backup_snapshot(b, argv[4], argv[5], 5, out, MAX_PATH) ? 0 : 1;
                if (exit_code == 0) {
                    st_printf(L"ring_path=%ls\n", out);
                }
            }
        } else if (wcscmp(sub, L"restore-with-safety") == 0) {
            if (argc < 6) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                ring_backup_init(argv[3], 5);
                restore_safe_request_t req = {
                    .backend = b,
                    .backup_src = argv[4],
                    .save_dst = argv[5],
                    .tree_root = argv[3],
                    .compression_level = 5,
                    .slot_mode = false,
                    .slot_index = 0,
                };
                exit_code = restore_safe_full(&req) ? 0 : 1;
            }
        } else if (wcscmp(sub, L"restore-auto-detect") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                save_kind_t kind = save_compress_classify_backup(argv[3]);
                if (kind == SAVE_KIND_FULL) {
                    st_printf(L"FULL\n");
                    exit_code = 0;
                } else if (kind == SAVE_KIND_SLOT) {
                    st_printf(L"SLOT\n");
                    exit_code = 0;
                } else {
                    st_printf(L"UNKNOWN\n");
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"undo-last-restore") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                ring_backup_init(argv[3], 5);
                exit_code = restore_safe_undo(b, argv[3], 5) ? 0 : 1;
            }
        } else if (wcscmp(sub, L"write-raw-bnd4") == 0) {
            if (argc < 5) {
                st_printf(L"usage: --selftest write-raw-bnd4 <src> <dst>\n");
                exit_code = 2;
            } else {
                HANDLE fh = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (fh == INVALID_HANDLE_VALUE) {
                    st_printf(L"write-raw-bnd4: cannot open source\n");
                    exit_code = 1;
                } else {
                    DWORD fsz = GetFileSize(fh, NULL);
                    uint8_t *buf = (uint8_t *)LocalAlloc(LMEM_FIXED, fsz);
                    if (!buf) {
                        CloseHandle(fh);
                        exit_code = 1;
                    } else {
                        DWORD rd = 0;

                        ReadFile(fh, buf, fsz, &rd, NULL);
                        CloseHandle(fh);
                        if (!ersm_write_raw_bnd4_to_file(argv[4], buf, (size_t)rd)) {
                            st_printf(L"write-raw-bnd4: write failed (bad magic or I/O)\n");
                            exit_code = 1;
                        } else {
                            st_printf(L"write-raw-bnd4: ok\n");
                            exit_code = 0;
                        }

                        LocalFree(buf);
                    }
                }
            }
        } else if (wcscmp(sub, L"classify") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest classify <file>\n");
                exit_code = 2;
            } else {
                save_kind_t kind = save_compress_classify_backup(argv[3]);
                if (kind == SAVE_KIND_FULL) {
                    wprintf(L"FULL\n");
                } else if (kind == SAVE_KIND_SLOT) {
                    wprintf(L"SLOT\n");
                } else {
                    wprintf(L"UNKNOWN\n");
                }
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"profile-roundtrip") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                profile_store_t store;

                profile_store_init(&store);
                store.games[0].id = 1;
                lstrcpyW(store.games[0].name, L"TestGame");
                store.games[0].game_id = GAME_ID_ELDEN_RING;
                lstrcpyW(store.games[0].tree_root, L"C:\\Test\\Root");
                store.game_count = 1;
                store.backups[0].id = 1;
                store.backups[0].parent_game_id = 1;
                lstrcpyW(store.backups[0].name, L"Main");
                store.backups[0].compression_level = COMP_LEVEL_LOW;
                store.backup_count = 1;
                store.active_game_id = 1;
                store.active_backup_id = 1;
                store.next_game_id = 2;
                store.next_backup_id = 2;

                if (!profile_store_io_save(&store, argv[3])) {
                    st_printf(L"profile-roundtrip: FAIL (save)\n");
                    exit_code = 1;
                } else {
                    profile_store_t store2;
                    int ok;

                    profile_store_init(&store2);
                    if (!profile_store_io_load(&store2, argv[3])) {
                        st_printf(L"profile-roundtrip: FAIL (load)\n");
                        exit_code = 1;
                    } else {
                        ok = (int)(store2.game_count == 1)
                           & (int)(store2.backup_count == 1)
                           & (lstrcmpW(store2.games[0].name, L"TestGame") == 0 ? 1 : 0)
                           & (lstrcmpW(store2.backups[0].name, L"Main") == 0 ? 1 : 0)
                           & (store2.active_game_id == 1 ? 1 : 0)
                           & (store2.active_backup_id == 1 ? 1 : 0);
                        exit_code = ok ? 0 : 1;
                        if (exit_code == 0) {
                            st_printf(L"profile-roundtrip: ok\n");
                        } else {
                            st_printf(L"profile-roundtrip: FAIL (compare)\n");
                        }
                    }
                }
            }
        } else if (wcscmp(sub, L"profile-load") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                profile_store_t store;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[3]);
                st_printf(L"games=%u backups=%u active_game=%d active_backup=%d\n",
                    (unsigned)store.game_count, (unsigned)store.backup_count,
                    store.active_game_id, store.active_backup_id);
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"profile-add-game") == 0) {
            if (argc < 8) {
                exit_code = 2;
            } else {
                profile_store_t store;
                game_profile_t gp;
                int new_id;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[7]);
                ZeroMemory(&gp, sizeof(gp));
                lstrcpynW(gp.name, argv[3], 64);
                lstrcpynW(gp.original_save_dir, argv[4], MAX_PATH);
                lstrcpynW(gp.tree_root, argv[5], MAX_PATH);
                gp.game_id = (game_id_t)_wtoi(argv[6]);
                new_id = profile_store_add_game(&store, &gp);
                if (new_id == 0) {
                    st_printf(L"profile-add-game: FAIL\n");
                    exit_code = 1;
                } else {
                    profile_store_io_save(&store, argv[7]);
                    st_printf(L"profile-add-game: ok id=%d\n", new_id);
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"profile-add-backup") == 0) {
            if (argc < 7) {
                exit_code = 2;
            } else {
                profile_store_t store;
                backup_profile_t bp;
                const wchar_t *cl = argv[5];
                int new_id;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[6]);
                ZeroMemory(&bp, sizeof(bp));
                bp.parent_game_id = _wtoi(argv[3]);
                lstrcpynW(bp.name, argv[4], 64);
                if (wcscmp(cl, L"none") == 0) {
                    bp.compression_level = COMP_LEVEL_NONE;
                } else if (wcscmp(cl, L"medium") == 0) {
                    bp.compression_level = COMP_LEVEL_MEDIUM;
                } else if (wcscmp(cl, L"high") == 0) {
                    bp.compression_level = COMP_LEVEL_HIGH;
                } else {
                    bp.compression_level = COMP_LEVEL_LOW;
                }
                new_id = profile_store_add_backup(&store, &bp);
                if (new_id == 0) {
                    st_printf(L"profile-add-backup: FAIL\n");
                    exit_code = 1;
                } else {
                    profile_store_io_save(&store, argv[6]);
                    st_printf(L"profile-add-backup: ok id=%d\n", new_id);
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"profile-list") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                profile_store_t store;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[3]);
                st_printf(L"games=%zu backups=%zu active_game=%d active_backup=%d\n",
                    store.game_count, store.backup_count,
                    store.active_game_id, store.active_backup_id);
                for (size_t i = 0; i < store.game_count; i++) {
                    st_printf(L"  game[%d] name=%ls game_id=%d tree_root=%ls\n",
                        store.games[i].id, store.games[i].name,
                        (int)store.games[i].game_id, store.games[i].tree_root);
                }
                for (size_t i = 0; i < store.backup_count; i++) {
                    wchar_t backup_root[MAX_PATH] = {0};
                    bool has_root = profile_store_resolve_backup_root(&store, store.backups[i].id,
                        backup_root, MAX_PATH);

                    st_printf(L"  backup[%d] parent=%d name=%ls computed_root=%ls comp=%d\n",
                        store.backups[i].id, store.backups[i].parent_game_id,
                        store.backups[i].name, has_root ? backup_root : L"",
                        (int)store.backups[i].compression_level);
                }
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"profile-delete-game") == 0) {
            if (argc < 5) {
                exit_code = 2;
            } else {
                profile_store_t store;
                bool ok;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[4]);
                ok = profile_store_delete_game(&store, _wtoi(argv[3]));
                if (ok) {
                    profile_store_io_save(&store, argv[4]);
                    st_printf(L"profile-delete-game: ok\n");
                    exit_code = 0;
                } else {
                    st_printf(L"profile-delete-game: not found\n");
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"profile-delete-backup") == 0) {
            if (argc < 5) {
                exit_code = 2;
            } else {
                profile_store_t store;
                bool ok;

                profile_store_init(&store);
                profile_store_io_load(&store, argv[4]);
                ok = profile_store_delete_backup(&store, _wtoi(argv[3]));
                if (ok) {
                    profile_store_io_save(&store, argv[4]);
                    st_printf(L"profile-delete-backup: ok\n");
                    exit_code = 0;
                } else {
                    st_printf(L"profile-delete-backup: not found\n");
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"locale-dump") == 0) {
            for (int i = 0; i < (int)STR_PRAXIS_MAX; i++) {
                st_printf(L"%d: %ls\n", i, praxis_locale_str((praxis_string_index_t)i));
            }
            exit_code = 0;
        } else if (wcscmp(sub, L"locale-audit") == 0) {
            int previous_locale = praxis_locale_get_current();
            bool ok = true;

            for (int idx = 0; idx < (int)STR_PRAXIS_MAX; idx++) {
                praxis_string_index_t str_idx = (praxis_string_index_t)idx;
                const wchar_t *english;

                if (selftest_locale_allows_english_fallback(str_idx)) {
                    continue;
                }
                praxis_locale_set_current(0);
                english = praxis_locale_str(str_idx);
                for (int locale_idx = 1; locale_idx < praxis_locale_count(); locale_idx++) {
                    const wchar_t *translated;

                    praxis_locale_set_current(locale_idx);
                    translated = praxis_locale_str(str_idx);
                    if (translated[0] == L'\0' || wcscmp(translated, english) == 0) {
                        st_printf(L"locale-audit: locale=%d idx=%d text=%ls\n",
                            locale_idx, idx, translated);
                        ok = false;
                    }
                }
            }

            praxis_locale_set_current(previous_locale);
            exit_code = ok ? 0 : 1;
        } else if (wcscmp(sub, L"watcher-state") == 0) {
            if (argc < 4) {
                exit_code = 2;
            } else {
                /* watcher-state: create a host window so save_watcher_start gets a valid HWND */
                HWND ws_host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
                if (!ws_host) {
                    st_printf(L"watcher-state: failed to create host window\n");
                    exit_code = 1;
                } else {
                    save_watcher_t *watcher = save_watcher_start(ws_host, argv[3], WM_APP + 1);
                    if (!watcher) {
                        st_printf(L"watcher-state: failed to start\n");
                        DestroyWindow(ws_host);
                        exit_code = 1;
                    } else {
                        Sleep(500);
                        save_watcher_stop(watcher);
                        DestroyWindow(ws_host);
                        st_printf(L"watcher-state: ok\n");
                        exit_code = 0;
                    }
                }
            }
        } else if (wcscmp(sub, L"backup-full-with-active") == 0) {
            if (argc < 5) {
                exit_code = 2;
            } else {
                const game_backend_t *b = backend_registry_get_default();
                if (!b || !b->backup_full) {
                    st_printf(L"backup-full-with-active: no backend\n");
                    exit_code = 1;
                } else if (!b->backup_full(argv[3], argv[4], 5)) {
                    st_printf(L"backup-full-with-active: backup failed\n");
                    exit_code = 1;
                } else {
                    st_printf(L"backup-full-with-active: ok\n");
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"backup-replace-selected") == 0) {
            if (argc < 6) {
                st_printf(L"usage: --selftest backup-replace-selected <save_dir> <game_tree_root> <selected_relpath>\n");
                exit_code = 2;
            } else {
                HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
                save_tree_t *t;

                if (!host) {
                    exit_code = 1;
                } else {
                    t = save_tree_create(host, GetModuleHandleW(NULL), 0);
                    if (!t) {
                        DestroyWindow(host);
                        exit_code = 1;
                    } else {
                        profile_store_t store;
                        game_profile_t gp;
                        wchar_t backup_root[MAX_PATH];
                        wchar_t selected_full_before[MAX_PATH];
                        wchar_t selected_full_after[MAX_PATH];
                        save_kind_t before_kind;
                        save_kind_t after_kind;

                        profile_store_init(&store);
                        ZeroMemory(&gp, sizeof(gp));
                        lstrcpyW(gp.name, L"Selftest");
                        gp.game_id = GAME_ID_ELDEN_RING;
                        lstrcpynW(gp.original_save_dir, argv[3], MAX_PATH);
                        lstrcpynW(gp.tree_root, argv[4], MAX_PATH);

                        if (!profile_store_add_game(&store, &gp) ||
                            !profile_store_resolve_backup_root(&store, store.active_backup_id,
                                backup_root, MAX_PATH)) {
                            st_printf(L"backup-replace-selected: profile setup failed\n");
                            exit_code = 1;
                        } else {
                            wchar_t select_full[MAX_PATH];

                            save_tree_set_root(t, backup_root);
                            if (!selftest_build_tree_path(backup_root, argv[5], select_full, MAX_PATH) ||
                                !save_tree_select_full_path(t, select_full)) {
                                st_printf(L"backup-replace-selected: selection failed\n");
                                exit_code = 1;
                            } else if (!save_tree_get_selected_path(t, selected_full_before, MAX_PATH)) {
                                st_printf(L"backup-replace-selected: selected path failed\n");
                                exit_code = 1;
                            } else {
                                before_kind = save_compress_classify_backup(selected_full_before);
                                if (before_kind == SAVE_KIND_UNKNOWN) {
                                    st_printf(L"backup-replace-selected: unknown kind before replace\n");
                                    exit_code = 1;
                                } else if (praxis_hotkey_action_backup_replace_selected(host, &store, t, COMP_LEVEL_MEDIUM) != PRAXIS_ACTION_OK) {
                                    st_printf(L"backup-replace-selected: action failed\n");
                                    exit_code = 1;
                                } else if (!save_tree_get_selected_path(t, selected_full_after, MAX_PATH)) {
                                    st_printf(L"backup-replace-selected: selected path lost\n");
                                    exit_code = 1;
                                } else if (lstrcmpW(selected_full_before, selected_full_after) != 0) {
                                    st_printf(L"before=%ls\nafter=%ls\n", selected_full_before, selected_full_after);
                                    exit_code = 1;
                                } else {
                                    after_kind = save_compress_classify_backup(selected_full_after);
                                    if (after_kind != before_kind) {
                                        st_printf(L"before_kind=%d after_kind=%d\n",
                                            (int)before_kind, (int)after_kind);
                                        exit_code = 1;
                                    } else {
                                        st_printf(L"backup-replace-selected: ok kind=%d\n", (int)after_kind);
                                        exit_code = 0;
                                    }
                                }
                            }
                        }

                        save_tree_destroy(t);
                        DestroyWindow(host);
                    }
                }
            }
        } else if (wcscmp(sub, L"backup-replace-selected-readonly") == 0) {
            if (argc < 6) {
                st_printf(L"usage: --selftest backup-replace-selected-readonly <save_dir> <game_tree_root> <selected_relpath>\n");
                exit_code = 2;
            } else {
                HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
                save_tree_t *t;

                if (!host) {
                    exit_code = 1;
                } else {
                    t = save_tree_create(host, GetModuleHandleW(NULL), 0);
                    if (!t) {
                        DestroyWindow(host);
                        exit_code = 1;
                    } else {
                        profile_store_t store;
                        game_profile_t gp;
                        wchar_t backup_root[MAX_PATH];
                        wchar_t selected_full[MAX_PATH];
                        wchar_t temp_path[MAX_PATH];

                        profile_store_init(&store);
                        ZeroMemory(&gp, sizeof(gp));
                        lstrcpyW(gp.name, L"Selftest");
                        gp.game_id = GAME_ID_ELDEN_RING;
                        lstrcpynW(gp.original_save_dir, argv[3], MAX_PATH);
                        lstrcpynW(gp.tree_root, argv[4], MAX_PATH);

                        if (!profile_store_add_game(&store, &gp) ||
                            !profile_store_resolve_backup_root(&store, store.active_backup_id,
                                backup_root, MAX_PATH)) {
                            st_printf(L"backup-replace-selected-readonly: profile setup failed\n");
                            exit_code = 1;
                        } else {
                            wchar_t select_full[MAX_PATH];

                            save_tree_set_root(t, backup_root);
                            if (!selftest_build_tree_path(backup_root, argv[5], select_full, MAX_PATH) ||
                                !save_tree_select_full_path(t, select_full) ||
                                !save_tree_get_selected_path(t, selected_full, MAX_PATH)) {
                                st_printf(L"backup-replace-selected-readonly: selection failed\n");
                                exit_code = 1;
                            } else {
                                _snwprintf_s(temp_path, MAX_PATH, _TRUNCATE, L"%ls.replace.tmp", selected_full);
                                DeleteFileW(temp_path);
                                SetFileAttributesW(selected_full, FILE_ATTRIBUTE_READONLY);
                                if (praxis_hotkey_action_backup_replace_selected(host, &store, t, COMP_LEVEL_MEDIUM) == PRAXIS_ACTION_OK) {
                                    st_printf(L"backup-replace-selected-readonly: action should fail\n");
                                    exit_code = 1;
                                } else if (PathFileExistsW(temp_path)) {
                                    st_printf(L"backup-replace-selected-readonly: temp file left behind\n");
                                    DeleteFileW(temp_path);
                                    exit_code = 1;
                                } else {
                                    st_printf(L"backup-replace-selected-readonly: ok\n");
                                    exit_code = 0;
                                }
                                SetFileAttributesW(selected_full, FILE_ATTRIBUTE_NORMAL);
                            }
                        }

                        save_tree_destroy(t);
                        DestroyWindow(host);
                    }
                }
            }
        } else if (wcscmp(sub, L"hotkey-defaults") == 0) {
            praxis_load_config();
            st_printf(L"backup_full=%ls\n", praxis_config.hotkey_backup_full);
            st_printf(L"restore=%ls\n", praxis_config.hotkey_restore);
            st_printf(L"undo=%ls\n", praxis_config.hotkey_undo_restore);
            st_printf(L"backup_slot=%ls\n", praxis_config.hotkey_backup_slot);
            st_printf(L"backup_replace=%ls\n", praxis_config.hotkey_backup_replace);
            st_printf(L"previous_save=%ls\n", praxis_config.hotkey_previous_save);
            st_printf(L"next_save=%ls\n", praxis_config.hotkey_next_save);
            exit_code = 0;
        } else if (wcscmp(sub, L"config-load") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest config-load <ini>\n");
                exit_code = 2;
            } else {
                HANDLE cfg_fh;

                praxis_load_config();
                cfg_fh = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (cfg_fh != INVALID_HANDLE_VALUE) {
                    DWORD cfg_size = GetFileSize(cfg_fh, NULL);
                    if (cfg_size != INVALID_FILE_SIZE && cfg_size > 0 && cfg_size <= (64u * 1024u)) {
                        char *cfg_buf = LocalAlloc(LMEM_FIXED, cfg_size + 1u);
                        if (cfg_buf) {
                            DWORD cfg_read = 0;
                            if (ReadFile(cfg_fh, cfg_buf, cfg_size, &cfg_read, NULL)) {
                                cfg_buf[cfg_read] = '\0';
                                config_core_parse_ini(cfg_buf, (size_t)cfg_read,
                                    selftest_config_kv_cb, &praxis_config);
                            }
                            LocalFree(cfg_buf);
                        }
                    }
                    CloseHandle(cfg_fh);
                }
                st_printf(L"tree_root=%ls\n", praxis_config.tree_root);
                st_printf(L"language=%d\n", praxis_config.language);
                st_printf(L"hotkeys_backup_full=%ls\n", praxis_config.hotkey_backup_full);
                st_printf(L"hotkeys_backup_replace=%ls\n", praxis_config.hotkey_backup_replace);
                st_printf(L"hotkeys_previous_save=%ls\n", praxis_config.hotkey_previous_save);
                st_printf(L"hotkeys_next_save=%ls\n", praxis_config.hotkey_next_save);
                st_printf(L"ring_size=%d\n", praxis_config.ring_size);
                st_printf(L"compression_level=%d\n", praxis_config.compression_level);
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"backend-vtable-shape") == 0) {
            const game_backend_t *bvt = backend_registry_get_default();
            if (!bvt) {
                st_printf(L"no backend\n");
                exit_code = 1;
            } else {
                st_printf(L"id=%d\n", (int)bvt->id);
                st_printf(L"display_name=%ls\n", bvt->display_name);
                st_printf(L"has_get_active_slot=%d\n", bvt->get_active_slot ? 1 : 0);
                st_printf(L"has_backup_slot=%d\n", bvt->backup_slot ? 1 : 0);
                st_printf(L"has_restore_slot=%d\n", bvt->restore_slot ? 1 : 0);
                exit_code = 0;
            }
        } else if (wcscmp(sub, L"profile-resolve-active") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest profile-resolve-active <ini>\n");
                exit_code = 2;
            } else {
                profile_store_t pra_store;
                const game_profile_t *pra_gp;

                profile_store_init(&pra_store);
                profile_store_io_load(&pra_store, argv[3]);
                pra_gp = profile_store_get_active_game(&pra_store);
                if (!pra_gp) {
                    st_printf(L"profile-resolve-active: no active profile\n");
                    exit_code = 1;
                } else {
                    wchar_t pra_save_path[MAX_PATH] = {0};
                    const game_backend_t *pra_b = backend_registry_get_by_id(pra_gp->game_id);
                    if (pra_gp->original_save_dir[0] != L'\0') {
                        lstrcpynW(pra_save_path, pra_gp->original_save_dir, MAX_PATH);
                    } else if (pra_b && pra_b->resolve_save_path) {
                        pra_b->resolve_save_path(pra_save_path, MAX_PATH);
                    }
                    st_printf(L"active_game_id=%d\n", pra_gp->id);
                    st_printf(L"save_path=%ls\n", pra_save_path);
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"detect-system-language-debug") == 0) {
            /* Print intermediate state of locale_core_detect_system_language. */
            wchar_t dsl_locale[LOCALE_NAME_MAX_LENGTH] = {0};
            wchar_t dsl_scripts[64] = {0};
            int dsl_ret = GetUserDefaultLocaleName(dsl_locale, LOCALE_NAME_MAX_LENGTH);
            st_printf(L"GetUserDefaultLocaleName ret=%d locale=\"%ls\"\n", dsl_ret, dsl_locale);
            int dsl_scripts_ret = GetLocaleInfoEx(dsl_locale, LOCALE_SSCRIPTS, dsl_scripts,
                (int)(sizeof(dsl_scripts) / sizeof(dsl_scripts[0])));
            st_printf(L"GetLocaleInfoEx LOCALE_SSCRIPTS ret=%d scripts=\"%ls\"\n",
                dsl_scripts_ret, dsl_scripts);
            int detected = praxis_locale_detect_system();
            st_printf(L"praxis_locale_detect_system=%d\n", detected);
            for (int i = 0; i < praxis_locale_count(); i++) {
                st_printf(L"  [%d] name=%ls\n", i, praxis_locale_name(i));
            }
            exit_code = 0;
        } else if (wcscmp(sub, L"unique-game-name") == 0) {
            /* unique-game-name <ini> <base_name>
             * Loads the profile store from <ini> and prints the unique name that
             * profile_store_find_unique_game_name returns for <base_name>. */
            if (argc < 5) {
                st_printf(L"usage: --selftest unique-game-name <ini> <base_name>\n");
                exit_code = 2;
            } else {
                profile_store_t ugn_store;
                wchar_t ugn_out[64];

                profile_store_init(&ugn_store);
                profile_store_io_load(&ugn_store, argv[3]);
                if (profile_store_find_unique_game_name(&ugn_store, argv[4], ugn_out, 64)) {
                    st_printf(L"unique-game-name: %ls\n", ugn_out);
                    exit_code = 0;
                } else {
                    st_printf(L"unique-game-name: FAIL\n");
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"backend-default-save-dir") == 0) {
            /* backend-default-save-dir <game_id>
             * Calls the backend's get_default_save_dir method (if any) and prints
             * the result. Exit 0 even when no save directory is found, since this
             * depends on the test machine. Exit 1 only when the backend or method
             * is missing. */
            if (argc < 4) {
                st_printf(L"usage: --selftest backend-default-save-dir <game_id>\n");
                exit_code = 2;
            } else {
                game_id_t gid = (game_id_t)_wtoi(argv[3]);
                const game_backend_t *bds = backend_registry_get_by_id(gid);
                if (!bds) {
                    st_printf(L"backend-default-save-dir: no backend for id=%d\n", (int)gid);
                    exit_code = 1;
                } else if (!bds->get_default_save_dir) {
                    st_printf(L"backend-default-save-dir: not supported by id=%d\n", (int)gid);
                    exit_code = 1;
                } else {
                    wchar_t bds_out[MAX_PATH] = {0};
                    if (bds->get_default_save_dir(bds_out, MAX_PATH)) {
                        st_printf(L"backend-default-save-dir: %ls\n", bds_out);
                    } else {
                        st_printf(L"backend-default-save-dir: not found\n");
                    }
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"watcher-debounce-timing") == 0) {
            if (argc < 4) {
                st_printf(L"usage: --selftest watcher-debounce-timing <root>\n");
                exit_code = 2;
            } else {
                HWND wdt_host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                    0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
                if (!wdt_host) {
                    st_printf(L"watcher-debounce-timing: failed to create host window\n");
                    exit_code = 1;
                } else {
                    save_watcher_t *wdt_w = save_watcher_start(wdt_host, argv[3], WM_APP + 1);
                    if (!wdt_w) {
                        st_printf(L"watcher-debounce-timing: failed to start watcher\n");
                        DestroyWindow(wdt_host);
                        exit_code = 1;
                    } else {
                        st_printf(L"watcher_start_ok\n");
                        save_watcher_stop(wdt_w);
                        DestroyWindow(wdt_host);
                        st_printf(L"watcher_stop_ok\n");
                        exit_code = 0;
                    }
                }
            }
        } else if (wcscmp(sub, L"theme-change-classify") == 0) {
            exit_code = 0;
            if (!theme_core_is_relevant_setting_change(0, (LPARAM)L"ImmersiveColorSet")) {
                st_printf(L"theme-change-classify: ImmersiveColorSet should refresh\n");
                exit_code = 1;
            }
            if (!theme_core_is_relevant_setting_change(SPI_SETHIGHCONTRAST, 0)) {
                st_printf(L"theme-change-classify: SPI_SETHIGHCONTRAST should refresh\n");
                exit_code = 1;
            }
            if (theme_core_is_relevant_setting_change(0, 0)) {
                st_printf(L"theme-change-classify: NULL should not refresh\n");
                exit_code = 1;
            }
            if (theme_core_is_relevant_setting_change(0, (LPARAM)L"NotAThemeChange")) {
                st_printf(L"theme-change-classify: unrelated setting should not refresh\n");
                exit_code = 1;
            }
            if (exit_code == 0) {
                st_printf(L"theme-change-classify: ok\n");
            }
        } else if (wcscmp(sub, L"ds3-aes-known-vector") == 0) {
            /* Encrypts DS3_TEST_KNOWN_PLAINTEXT with DS3_AES_KEY_BYTES and DS3_TEST_IV,
             * asserts byte-exact equality with DS3_TEST_KNOWN_CIPHERTEXT, then decrypts
             * back and asserts equality with original plaintext. Validates BCrypt wiring
             * independent of the DS3 fixture builder. */
            BCRYPT_ALG_HANDLE alg = NULL;
            BCRYPT_KEY_HANDLE key = NULL;
            uint8_t *key_obj = NULL;
            ULONG key_obj_size = 0;
            ULONG result_size = 0;
            NTSTATUS status;
            bool ok = true;

            status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
            if (!NT_SUCCESS(status)) {
                st_printf(L"FAIL: BCryptOpenAlgorithmProvider\n");
                ok = false;
            }
            if (ok) {
                status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                            (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                            sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
                if (!NT_SUCCESS(status)) {
                    st_printf(L"FAIL: BCryptSetProperty\n");
                    ok = false;
                }
            }
            if (ok) {
                status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                            (PUCHAR)&key_obj_size, sizeof(ULONG),
                                            &result_size, 0);
                if (!NT_SUCCESS(status)) {
                    st_printf(L"FAIL: BCryptGetProperty\n");
                    ok = false;
                }
            }
            if (ok) {
                key_obj = (uint8_t *)LocalAlloc(LMEM_FIXED, key_obj_size);
                if (!key_obj) {
                    st_printf(L"FAIL: LocalAlloc\n");
                    ok = false;
                }
            }
            if (ok) {
                status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size,
                                                    (PUCHAR)DS3_AES_KEY_BYTES, 16, 0);
                if (!NT_SUCCESS(status)) {
                    st_printf(L"FAIL: BCryptGenerateSymmetricKey\n");
                    ok = false;
                }
            }

            if (ok) {
                uint8_t iv_copy[16];
                uint8_t ct_buf[64];
                ULONG ct_len = 0;

                CopyMemory(iv_copy, DS3_TEST_IV, 16);
                status = BCryptEncrypt(key, (PUCHAR)DS3_TEST_KNOWN_PLAINTEXT, 16, NULL,
                                        iv_copy, 16, ct_buf, sizeof(ct_buf), &ct_len,
                                        BCRYPT_BLOCK_PADDING);
                if (!NT_SUCCESS(status)) {
                    st_printf(L"FAIL: BCryptEncrypt\n");
                    ok = false;
                }
                if (ok && (ct_len != DS3_TEST_KNOWN_CIPHERTEXT_SIZE
                           || RtlCompareMemory(ct_buf, DS3_TEST_KNOWN_CIPHERTEXT,
                                               DS3_TEST_KNOWN_CIPHERTEXT_SIZE)
                              != DS3_TEST_KNOWN_CIPHERTEXT_SIZE)) {
                    st_printf(L"FAIL: ciphertext mismatch\n");
                    ok = false;
                }

                if (ok) {
                    uint8_t pt_buf[64];
                    ULONG pt_len = 0;

                    CopyMemory(iv_copy, DS3_TEST_IV, 16);
                    status = BCryptDecrypt(key, ct_buf, ct_len, NULL, iv_copy, 16,
                                            pt_buf, sizeof(pt_buf), &pt_len,
                                            BCRYPT_BLOCK_PADDING);
                    if (!NT_SUCCESS(status) || pt_len != 16
                        || RtlCompareMemory(pt_buf, DS3_TEST_KNOWN_PLAINTEXT, 16) != 16) {
                        st_printf(L"FAIL: decrypt mismatch\n");
                        ok = false;
                    }
                }
            }

            if (key) {
                BCryptDestroyKey(key);
            }
            if (key_obj) {
                LocalFree(key_obj);
            }
            if (alg) {
                BCryptCloseAlgorithmProvider(alg, 0);
            }

            if (ok) {
                st_printf(L"PASS: ds3-aes-known-vector\n");
                exit_code = 0;
            } else {
                exit_code = 1;
            }
        } else if (wcscmp(sub, L"ds3-load-min-fixture") == 0) {
            /* Build a minimal valid DS3 fixture, load it, verify active slot is 0,
             * verify slot 0 is non-NULL, and verify slot 1 is NULL. */
            if (argc < 4) {
                st_printf(L"Usage: ds3-load-min-fixture <tmp_path>\n");
                exit_code = 1;
            } else {
                const wchar_t *tmp_path = argv[3];
                ds3_save_data_t *save;
                int slot = -1;
                const ds3_char_data_t *c0;
                const ds3_char_data_t *c1;

                if (!praxis_make_min_valid_ds3_sl2(tmp_path, DS3_TEST_USERID_A)) {
                    st_printf(L"FAIL: fixture builder failed\n");
                    exit_code = 1;
                } else if ((save = ds3_save_data_load(tmp_path)) == NULL) {
                    DeleteFileW(tmp_path);
                    st_printf(L"FAIL: ds3_save_data_load returned NULL\n");
                    exit_code = 1;
                } else if (!ds3_save_get_active_slot(save, &slot) || slot != 0) {
                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    st_printf(L"FAIL: active slot expected 0, got %d\n", slot);
                    exit_code = 1;
                } else if ((c0 = ds3_char_data_ref(save, 0)) == NULL) {
                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    st_printf(L"FAIL: slot 0 should be non-NULL\n");
                    exit_code = 1;
                } else if ((c1 = ds3_char_data_ref(save, 1)) != NULL) {
                    (void)c1;
                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    st_printf(L"FAIL: slot 1 should be NULL\n");
                    exit_code = 1;
                } else {
                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    st_printf(L"PASS: ds3-load-min-fixture\n");
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"ds3-roundtrip-byte-stable") == 0) {
            /* Build a fixture, read file bytes A, load + serialize slot 0 +
             * import_raw the same bytes back, write file, read file bytes B,
             * assert A == B. A no-op roundtrip must be byte-stable. */
            if (argc < 4) {
                st_printf(L"Usage: ds3-roundtrip-byte-stable <tmp_path>\n");
                exit_code = 1;
            } else {
                const wchar_t *tmp_path = argv[3];

                if (!praxis_make_min_valid_ds3_sl2(tmp_path, DS3_TEST_USERID_A)) {
                    st_printf(L"FAIL: fixture builder failed\n");
                    exit_code = 1;
                } else {
                    HANDLE fh;
                    DWORD file_size = 0;
                    DWORD bytes_read = 0;
                    uint8_t *buf_a = NULL;
                    uint8_t *buf_b = NULL;
                    uint8_t *ser_buf = NULL;
                    ds3_save_data_t *save = NULL;
                    const ds3_char_data_t *c0 = NULL;
                    bool ok = false;

                    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (fh == INVALID_HANDLE_VALUE) {
                        DeleteFileW(tmp_path);
                        st_printf(L"FAIL: open A\n");
                        exit_code = 1;
                    } else {
                        file_size = GetFileSize(fh, NULL);
                        buf_a = (uint8_t *)LocalAlloc(LMEM_FIXED, file_size);
                        buf_b = (uint8_t *)LocalAlloc(LMEM_FIXED, file_size);
                        if (!buf_a || !buf_b
                            || !ReadFile(fh, buf_a, file_size, &bytes_read, NULL)
                            || bytes_read != file_size) {
                            CloseHandle(fh);
                            if (buf_a) LocalFree(buf_a);
                            if (buf_b) LocalFree(buf_b);
                            DeleteFileW(tmp_path);
                            st_printf(L"FAIL: read A\n");
                            exit_code = 1;
                        } else {
                            CloseHandle(fh);

                            save = ds3_save_data_load(tmp_path);
                            ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
                            if (!save) {
                                if (ser_buf) LocalFree(ser_buf);
                                LocalFree(buf_a);
                                LocalFree(buf_b);
                                DeleteFileW(tmp_path);
                                st_printf(L"FAIL: load\n");
                                exit_code = 1;
                            } else if (!ser_buf || (c0 = ds3_char_data_ref(save, 0)) == NULL) {
                                if (ser_buf) LocalFree(ser_buf);
                                ds3_save_data_free(save);
                                LocalFree(buf_a);
                                LocalFree(buf_b);
                                DeleteFileW(tmp_path);
                                st_printf(L"FAIL: ref\n");
                                exit_code = 1;
                            } else if (!ds3_char_data_serialize(c0, ser_buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                                LocalFree(ser_buf);
                                ds3_save_data_free(save);
                                LocalFree(buf_a);
                                LocalFree(buf_b);
                                DeleteFileW(tmp_path);
                                st_printf(L"FAIL: serialize\n");
                                exit_code = 1;
                            } else if (!ds3_char_data_import_raw(save, 0, ser_buf)) {
                                LocalFree(ser_buf);
                                ds3_save_data_free(save);
                                LocalFree(buf_a);
                                LocalFree(buf_b);
                                DeleteFileW(tmp_path);
                                st_printf(L"FAIL: import_raw\n");
                                exit_code = 1;
                            } else {
                                LocalFree(ser_buf);
                                ds3_save_data_free(save);

                                fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                                if (fh == INVALID_HANDLE_VALUE
                                    || !ReadFile(fh, buf_b, file_size, &bytes_read, NULL)
                                    || bytes_read != file_size) {
                                    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
                                    LocalFree(buf_a);
                                    LocalFree(buf_b);
                                    DeleteFileW(tmp_path);
                                    st_printf(L"FAIL: read B\n");
                                    exit_code = 1;
                                } else {
                                    CloseHandle(fh);
                                    ok = (RtlCompareMemory(buf_a, buf_b, file_size) == file_size);
                                    LocalFree(buf_a);
                                    LocalFree(buf_b);
                                    DeleteFileW(tmp_path);
                                    if (!ok) {
                                        st_printf(L"FAIL: bytes differ after no-op roundtrip\n");
                                        exit_code = 1;
                                    } else {
                                        st_printf(L"PASS: ds3-roundtrip-byte-stable\n");
                                        exit_code = 0;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (wcscmp(sub, L"ds3-active-slot") == 0) {
            /* Build a fixture, load it, verify the active slot equals the expected
             * value passed on the command line. */
            if (argc < 5) {
                st_printf(L"Usage: ds3-active-slot <tmp_path> <expected_int>\n");
                exit_code = 1;
            } else {
                const wchar_t *tmp_path = argv[3];
                int expected = _wtoi(argv[4]);
                ds3_save_data_t *save;
                int slot = -1;
                bool ok;

                if (!praxis_make_min_valid_ds3_sl2(tmp_path, DS3_TEST_USERID_A)) {
                    st_printf(L"FAIL: fixture builder failed\n");
                    exit_code = 1;
                } else if ((save = ds3_save_data_load(tmp_path)) == NULL) {
                    DeleteFileW(tmp_path);
                    st_printf(L"FAIL: load\n");
                    exit_code = 1;
                } else {
                    ok = ds3_save_get_active_slot(save, &slot);
                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    if (!ok || slot != expected) {
                        st_printf(L"FAIL: active slot %d, expected %d\n", slot, expected);
                        exit_code = 1;
                    } else {
                        st_printf(L"PASS: ds3-active-slot (slot=%d)\n", slot);
                        exit_code = 0;
                    }
                }
            }
        } else if (wcscmp(sub, L"ds3-null-guards") == 0) {
            /* Verify each of the 6 public DS3 functions rejects NULL / out-of-range
             * inputs cleanly without crashing or returning success. */
            bool ok = true;
            int slot_out = -1;
            uint8_t *dummy_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
            wchar_t tmp_path[MAX_PATH];
            ds3_save_data_t *save = NULL;
            const ds3_char_data_t *c0 = NULL;

            if (!dummy_buf) {
                st_printf(L"FAIL: dummy_buf alloc failed\n");
                exit_code = 1;
            } else {
                /* ds3_save_data_load(NULL) returns NULL */
                if (ds3_save_data_load(NULL) != NULL) {
                    st_printf(L"FAIL: load(NULL) should return NULL\n");
                    ok = false;
                }
                /* ds3_save_data_free(NULL) does not crash */
                ds3_save_data_free(NULL);
                /* ds3_save_get_active_slot(NULL, &slot) returns false */
                if (ds3_save_get_active_slot(NULL, &slot_out)) {
                    st_printf(L"FAIL: get_active_slot(NULL, ...) should return false\n");
                    ok = false;
                }

                /* Build a fixture for the rest of the checks */
                tmp_path[0] = L'\0';
                GetTempPathW(MAX_PATH, tmp_path);
                PathAppendW(tmp_path, L"ds3-null-guards-test.sl2");

                if (!praxis_make_min_valid_ds3_sl2(tmp_path, DS3_TEST_USERID_A)) {
                    st_printf(L"FAIL: fixture builder failed\n");
                    LocalFree(dummy_buf);
                    exit_code = 1;
                } else if ((save = ds3_save_data_load(tmp_path)) == NULL) {
                    DeleteFileW(tmp_path);
                    LocalFree(dummy_buf);
                    st_printf(L"FAIL: load fixture\n");
                    exit_code = 1;
                } else {
                    /* ds3_save_get_active_slot(valid_save, NULL) returns false */
                    if (ds3_save_get_active_slot(save, NULL)) {
                        st_printf(L"FAIL: get_active_slot(..., NULL) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_ref(NULL, 0) returns NULL */
                    if (ds3_char_data_ref(NULL, 0) != NULL) {
                        st_printf(L"FAIL: ref(NULL, 0) should return NULL\n");
                        ok = false;
                    }
                    /* ds3_char_data_ref(valid_save, -1) returns NULL */
                    if (ds3_char_data_ref(save, -1) != NULL) {
                        st_printf(L"FAIL: ref(save, -1) should return NULL\n");
                        ok = false;
                    }
                    /* ds3_char_data_ref(valid_save, 10) returns NULL */
                    if (ds3_char_data_ref(save, 10) != NULL) {
                        st_printf(L"FAIL: ref(save, 10) should return NULL\n");
                        ok = false;
                    }

                    c0 = ds3_char_data_ref(save, 0);

                    /* ds3_char_data_serialize(NULL, buf, size) returns false */
                    if (ds3_char_data_serialize(NULL, dummy_buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                        st_printf(L"FAIL: serialize(NULL, ...) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_serialize(valid_char, NULL, size) returns false */
                    if (c0 && ds3_char_data_serialize(c0, NULL, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                        st_printf(L"FAIL: serialize(..., NULL, ...) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_serialize(valid_char, buf, 0) returns false */
                    if (c0 && ds3_char_data_serialize(c0, dummy_buf, 0)) {
                        st_printf(L"FAIL: serialize(..., ..., 0) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_import_raw(NULL, 0, buf) returns false */
                    if (ds3_char_data_import_raw(NULL, 0, dummy_buf)) {
                        st_printf(L"FAIL: import_raw(NULL, ...) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_import_raw(valid_save, 0, NULL) returns false */
                    if (ds3_char_data_import_raw(save, 0, NULL)) {
                        st_printf(L"FAIL: import_raw(..., ..., NULL) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_import_raw(valid_save, -1, buf) returns false */
                    if (ds3_char_data_import_raw(save, -1, dummy_buf)) {
                        st_printf(L"FAIL: import_raw(..., -1, ...) should return false\n");
                        ok = false;
                    }
                    /* ds3_char_data_import_raw(valid_save, 10, buf) returns false */
                    if (ds3_char_data_import_raw(save, 10, dummy_buf)) {
                        st_printf(L"FAIL: import_raw(..., 10, ...) should return false\n");
                        ok = false;
                    }

                    ds3_save_data_free(save);
                    DeleteFileW(tmp_path);
                    LocalFree(dummy_buf);

                    if (!ok) {
                        exit_code = 1;
                    } else {
                        st_printf(L"PASS: ds3-null-guards\n");
                        exit_code = 0;
                    }
                }
            }
        } else if (wcscmp(sub, L"ds3-import-resigns-userid") == 0) {
            /* Build fixture A with USERID_A and fixture B with USERID_B.
             * Serialize A's slot 0, import into B's slot 0, reload B, and verify
             * that B's slot 0 userid is USERID_B (re-signed) and not USERID_A.
             * Since ds3_char_data_t is opaque, the userid is verified by
             * serializing B's slot 0 after import and reading the userid out of
             * the plaintext buffer at offset N + DS3_CHAR_USERID_DELTA, where
             * N is the uint32 at DS3_CHAR_USERID_LEN_OFFSET. */
            if (argc < 5) {
                st_printf(L"Usage: ds3-import-resigns-userid <tmp_path_A> <tmp_path_B>\n");
                exit_code = 1;
            } else {
                const wchar_t *path_a = argv[3];
                const wchar_t *path_b = argv[4];
                ds3_save_data_t *save_a = NULL;
                ds3_save_data_t *save_b = NULL;
                const ds3_char_data_t *c0_a = NULL;
                const ds3_char_data_t *c0_b = NULL;
                uint8_t *ser_buf = NULL;
                uint8_t *verify_buf = NULL;
                bool flow_ok = true;

                if (!praxis_make_min_valid_ds3_sl2(path_a, DS3_TEST_USERID_A)) {
                    st_printf(L"FAIL: fixture A builder failed\n");
                    flow_ok = false;
                } else if (!praxis_make_min_valid_ds3_sl2(path_b, DS3_TEST_USERID_B)) {
                    DeleteFileW(path_a);
                    st_printf(L"FAIL: fixture B builder failed\n");
                    flow_ok = false;
                }

                if (flow_ok) {
                    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
                    if (!ser_buf) {
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: ser_buf alloc\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok) {
                    save_a = ds3_save_data_load(path_a);
                    if (!save_a) {
                        LocalFree(ser_buf);
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: load A\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok) {
                    c0_a = ds3_char_data_ref(save_a, 0);
                    if (!c0_a) {
                        ds3_save_data_free(save_a);
                        LocalFree(ser_buf);
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: ref A slot 0\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok && !ds3_char_data_serialize(c0_a, ser_buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                    ds3_save_data_free(save_a);
                    LocalFree(ser_buf);
                    DeleteFileW(path_a);
                    DeleteFileW(path_b);
                    st_printf(L"FAIL: serialize A\n");
                    flow_ok = false;
                }

                if (flow_ok) {
                    ds3_save_data_free(save_a);
                    save_a = NULL;
                    save_b = ds3_save_data_load(path_b);
                    if (!save_b) {
                        LocalFree(ser_buf);
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: load B\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok && !ds3_char_data_import_raw(save_b, 0, ser_buf)) {
                    ds3_save_data_free(save_b);
                    LocalFree(ser_buf);
                    DeleteFileW(path_a);
                    DeleteFileW(path_b);
                    st_printf(L"FAIL: import_raw into B\n");
                    flow_ok = false;
                }

                if (flow_ok) {
                    LocalFree(ser_buf);
                    ser_buf = NULL;
                    ds3_save_data_free(save_b);
                    save_b = ds3_save_data_load(path_b);
                    if (!save_b) {
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: reload B\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok) {
                    c0_b = ds3_char_data_ref(save_b, 0);
                    if (!c0_b) {
                        ds3_save_data_free(save_b);
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: ref B slot 0 after import\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok) {
                    verify_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
                    if (!verify_buf || !ds3_char_data_serialize(c0_b, verify_buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                        if (verify_buf) LocalFree(verify_buf);
                        ds3_save_data_free(save_b);
                        DeleteFileW(path_a);
                        DeleteFileW(path_b);
                        st_printf(L"FAIL: serialize B for verification\n");
                        flow_ok = false;
                    }
                }

                if (flow_ok) {
                    uint32_t n = *(const uint32_t *)(verify_buf + DS3_CHAR_USERID_LEN_OFFSET);
                    uint64_t userid_in_b = *(const uint64_t *)(verify_buf + n + DS3_CHAR_USERID_DELTA);

                    LocalFree(verify_buf);
                    ds3_save_data_free(save_b);
                    DeleteFileW(path_a);
                    DeleteFileW(path_b);

                    if (userid_in_b != DS3_TEST_USERID_B) {
                        st_printf(L"FAIL: userid in B after import is 0x%llX, expected 0x%llX (USERID_B)\n",
                            (unsigned long long)userid_in_b, (unsigned long long)DS3_TEST_USERID_B);
                        exit_code = 1;
                    } else {
                        st_printf(L"PASS: ds3-import-resigns-userid (userid correctly re-signed to B's userid)\n");
                        exit_code = 0;
                    }
                } else {
                    exit_code = 1;
                }
            }
        } else if (wcscmp(sub, L"ds3-real-save-load") == 0) {
            /* Load a real DS3 save file (passed as argv[3]) and print the
             * active slot plus availability of all 10 char slots. Always
             * frees the save_data; does not modify the file. */
            if (argc < 4) {
                st_printf(L"Usage: ds3-real-save-load <path>\n");
                exit_code = 1;
            } else {
                const wchar_t *path = argv[3];
                ds3_save_data_t *save = ds3_save_data_load(path);

                if (!save) {
                    st_printf(L"FAIL: ds3_save_data_load returned NULL for real save\n");
                    exit_code = 1;
                } else {
                    int slot = -1;
                    int i;

                    if (ds3_save_get_active_slot(save, &slot)) {
                        st_printf(L"Active slot: %d\n", slot);
                    } else {
                        st_printf(L"Active slot: (not available)\n");
                    }

                    for (i = 0; i < 10; i++) {
                        const ds3_char_data_t *c = ds3_char_data_ref(save, i);
                        st_printf(L"Slot %d: %ls\n", i, c ? L"available" : L"empty");
                    }

                    ds3_save_data_free(save);
                    st_printf(L"PASS: ds3-real-save-load\n");
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"ds3-dump-summary") == 0) {
            /* Load a real DS3 save and dump summary plaintext offsets for diagnosis. */
            if (argc < 4) {
                st_printf(L"Usage: ds3-dump-summary <path>\n");
                exit_code = 1;
            } else {
                ds3_save_data_t *save = ds3_save_data_load(argv[3]);
                if (!save) {
                    st_printf(L"FAIL: ds3_save_data_load returned NULL\n");
                    exit_code = 1;
                } else {
                    uint8_t active[4] = {0};
                    uint8_t available[10] = {0};
                    int first_nz = -1;
                    uint8_t first_nz_val = 0;
                    ds3_save_dump_summary_offsets(save, active, available, &first_nz, &first_nz_val);
                    ds3_save_data_free(save);
                    st_printf(L"ACTIVE_OFFSET(0x0FE8): %02X %02X %02X %02X  (int32=%d)\n",
                        active[0], active[1], active[2], active[3],
                        (int)(active[0] | (active[1]<<8) | (active[2]<<16) | (active[3]<<24)));
                    st_printf(L"AVAILABLE_OFFSET(0x1098): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        available[0], available[1], available[2], available[3], available[4],
                        available[5], available[6], available[7], available[8], available[9]);
                    st_printf(L"First non-zero in summary[0..0x1200]: offset=0x%X value=0x%02X\n",
                        first_nz, first_nz_val);
                    exit_code = 0;
                }
            }
        } else if (wcscmp(sub, L"ds3-real-save-classify") == 0) {
            /* Open a real DS3 save file (argv[3]) read-only, verify BND4 magic
             * at offset 0 and slot count >= 12 at offset 0x0C, and print the
             * file size. Never modifies the file. */
            if (argc < 4) {
                st_printf(L"Usage: ds3-real-save-classify <path>\n");
                exit_code = 1;
            } else {
                const wchar_t *path = argv[3];
                HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

                if (fh == INVALID_HANDLE_VALUE) {
                    st_printf(L"FAIL: cannot open file\n");
                    exit_code = 1;
                } else {
                    DWORD file_size = GetFileSize(fh, NULL);
                    uint8_t header[0x10];
                    uint8_t slot_count_buf[4];
                    DWORD bytes_read = 0;

                    if (!ReadFile(fh, header, sizeof(header), &bytes_read, NULL)
                        || bytes_read != sizeof(header)) {
                        CloseHandle(fh);
                        st_printf(L"FAIL: cannot read header\n");
                        exit_code = 1;
                    } else if (header[0] != 'B' || header[1] != 'N'
                               || header[2] != 'D' || header[3] != '4') {
                        CloseHandle(fh);
                        st_printf(L"FAIL: not a BND4 file\n");
                        exit_code = 1;
                    } else {
                        int slot_count;

                        SetFilePointer(fh, 0x0C, NULL, FILE_BEGIN);
                        ReadFile(fh, slot_count_buf, 4, &bytes_read, NULL);
                        slot_count = *(int *)slot_count_buf;
                        CloseHandle(fh);

                        if (slot_count < 12) {
                            st_printf(L"FAIL: slot count %d < 12\n", slot_count);
                            exit_code = 1;
                        } else {
                            st_printf(L"File size: %u bytes\n", file_size);
                            st_printf(L"Slot count: %d\n", slot_count);
                            st_printf(L"PASS: ds3-real-save-classify\n");
                            exit_code = 0;
                        }
                    }
                }
            }
        } else if (wcscmp(sub, L"ds3-real-save-roundtrip-readonly") == 0) {
            /* Load a copy of a real DS3 save (argv[4]), serialize and re-import
             * the first available char slot's data (no-op semantically), reload,
             * and verify the active slot is unchanged. The real save path
             * (argv[3]) is not touched here; the caller must copy it to tmp_copy
             * before invoking this selftest. */
            if (argc < 5) {
                st_printf(L"Usage: ds3-real-save-roundtrip-readonly <path> <tmp_copy>\n");
                exit_code = 1;
            } else {
                const wchar_t *tmp_path = argv[4];
                ds3_save_data_t *save;

                (void)argv[3]; /* path retained for arg symmetry; only tmp_copy is touched */

                save = ds3_save_data_load(tmp_path);
                if (!save) {
                    st_printf(L"FAIL: load tmp copy\n");
                    exit_code = 1;
                } else {
                    int active_slot = -1;
                    int first_slot = -1;
                    int i;

                    ds3_save_get_active_slot(save, &active_slot);

                    for (i = 0; i < 10; i++) {
                        if (ds3_char_data_ref(save, i)) {
                            first_slot = i;
                            break;
                        }
                    }

                    if (first_slot < 0) {
                        ds3_save_data_free(save);
                        st_printf(L"SKIP: no available char slots in save\n");
                        exit_code = 0;
                    } else {
                        const ds3_char_data_t *c = ds3_char_data_ref(save, first_slot);
                        uint8_t *ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);

                        if (!ser_buf) {
                            ds3_save_data_free(save);
                            st_printf(L"FAIL: alloc\n");
                            exit_code = 1;
                        } else if (!ds3_char_data_serialize(c, ser_buf, DS3_CHAR_DATA_SERIALIZED_SIZE)) {
                            LocalFree(ser_buf);
                            ds3_save_data_free(save);
                            st_printf(L"FAIL: serialize\n");
                            exit_code = 1;
                        } else if (!ds3_char_data_import_raw(save, first_slot, ser_buf)) {
                            LocalFree(ser_buf);
                            ds3_save_data_free(save);
                            st_printf(L"FAIL: import_raw\n");
                            exit_code = 1;
                        } else {
                            int active_slot_after = -1;

                            LocalFree(ser_buf);
                            ds3_save_data_free(save);

                            save = ds3_save_data_load(tmp_path);
                            if (!save) {
                                st_printf(L"FAIL: reload after roundtrip\n");
                                exit_code = 1;
                            } else {
                                ds3_save_get_active_slot(save, &active_slot_after);
                                ds3_save_data_free(save);

                                if (active_slot != active_slot_after) {
                                    st_printf(L"FAIL: active slot changed from %d to %d\n",
                                        active_slot, active_slot_after);
                                    exit_code = 1;
                                } else {
                                    st_printf(L"PASS: ds3-real-save-roundtrip-readonly (slot %d, active=%d)\n",
                                        first_slot, active_slot);
                                    exit_code = 0;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            st_printf(L"unknown selftest subcommand: %ls\n", sub);
            exit_code = 2;
        }
    }

    /* FreeConsole not called: selftest exits the process immediately after returning. */
    return exit_code;
}

#endif /* PRAXIS_ENABLE_SELFTEST */
