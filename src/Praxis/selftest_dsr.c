/**
 * @file selftest_dsr.c
 * @brief DSR (Dark Souls: Remastered) selftest subcommand handlers and fixture builder.
 * @details Implements 9 DSR-specific selftest subcommands plus a minimal valid
 *          DSR .sl2 fixture builder. Linked only into the PraxisSelftest
 *          executable target (never into the praxis GUI target). The
 *          dispatcher hook into praxis_selftest_run is wired separately by T14.
 */

#include "selftest_dsr.h"
#include "praxis_selftest.h"
#include "dsr_test_format.h"
#include "../common/dsrsave.h"

#include "../../deps/md5/md5.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>

/* On-disk header in front of each BND4 slot: [16-byte MD5][16-byte IV][CT]. */
#define DSR_MD5_HEADER_SIZE 0x10u

/* Formatted wide-char printf for DSR selftest output (mirrors st_printf in
 * praxis_selftest.c, which is static and not accessible from this TU). */
static void dsr_printf(const wchar_t *fmt, ...) {
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
        char *utf8;

        if (utf8_size <= 0) {
            return;
        }

        utf8 = (char *)LocalAlloc(LMEM_FIXED, utf8_size);
        if (!utf8) {
            return;
        }

        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, utf8_size, NULL, NULL);
        WriteFile(hOut, utf8, (DWORD)(utf8_size - 1), &written, NULL);
        LocalFree(utf8);
    }
}

/* Returns true if a regular file exists at the given path. */
static bool dsr_file_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Compute SHA256 of the file at path. out_hash receives 32 bytes on success.
 * Opens the file with GENERIC_READ | FILE_SHARE_READ only — never modifies it. */
static bool dsr_compute_file_sha256(const wchar_t *path, uint8_t out_hash[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    uint8_t *hash_obj = NULL;
    ULONG hash_obj_size = 0;
    ULONG result_size = 0;
    HANDLE fh = INVALID_HANDLE_VALUE;
    uint8_t *buf = NULL;
    DWORD bytes_read = 0;
    NTSTATUS status;
    bool ok = true;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        ok = false;
    }
    if (ok) {
        status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hash_obj_size,
                                    sizeof(ULONG), &result_size, 0);
        if (!NT_SUCCESS(status)) {
            ok = false;
        }
    }
    if (ok) {
        hash_obj = (uint8_t *)LocalAlloc(LMEM_FIXED, hash_obj_size);
        if (!hash_obj) {
            ok = false;
        }
    }
    if (ok) {
        status = BCryptCreateHash(alg, &hash, hash_obj, hash_obj_size, NULL, 0, 0);
        if (!NT_SUCCESS(status)) {
            ok = false;
        }
    }
    if (ok) {
        fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) {
            ok = false;
        }
    }
    if (ok) {
        buf = (uint8_t *)LocalAlloc(LMEM_FIXED, 65536);
        if (!buf) {
            ok = false;
        }
    }
    if (ok) {
        for (;;) {
            if (!ReadFile(fh, buf, 65536, &bytes_read, NULL)) {
                ok = false;
                break;
            }
            if (bytes_read == 0) {
                break;
            }
            status = BCryptHashData(hash, buf, bytes_read, 0);
            if (!NT_SUCCESS(status)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        status = BCryptFinishHash(hash, out_hash, 32, 0);
        if (!NT_SUCCESS(status)) {
            ok = false;
        }
    }

    if (buf) LocalFree(buf);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (hash) BCryptDestroyHash(hash);
    if (hash_obj) LocalFree(hash_obj);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* Creates a minimal valid DSR BND4 .sl2 fixture at path.
 * Builds 11 slots (10 char + 1 summary; NO regulation slot) using the real DSR
 * on-disk size (0x60030), encrypting each slot's plaintext with BCrypt
 * AES-128-CBC + PKCS7 using the DSR master key and the fixture's fixed IV.
 *
 * On-disk layout per slot: [16-byte MD5][16-byte IV][ciphertext].
 * MD5 covers IV || ciphertext (DSR convention, same as DS3).
 *
 * Only summary slot (index 10) carries meaningful fields:
 *   - active_slot byte at DSR_SUMMARY_ACTIVE_OFFSET (0x45)
 *   - availability[0] = 1 at DSR_SUMMARY_AVAILABLE_OFFSET (0xB0)
 * All char slots have all-zero plaintext (still produce valid MD5s).
 *
 * DSR has NO Steam ID embedded in save data — the userid parameter is purely
 * for API parity with praxis_make_min_valid_ds3_sl2 (multi-account fixture
 * filenames) and is NOT patched into the plaintext. */
static bool praxis_make_min_valid_dsr_sl2(const wchar_t *path, uint64_t userid) {
    const uint32_t header_size = DSR_BND4_FILE_HEADER_SIZE;
    const uint32_t slot_size = DSR_CHAR_SLOT_ON_DISK_SIZE;
    const uint32_t pt_size = DSR_CHAR_PLAINTEXT_SIZE;
    const uint32_t ct_size = slot_size - DSR_MD5_HEADER_SIZE - 16u;
    const uint32_t total_size = header_size + (uint32_t)DSR_BND4_ENTRY_COUNT * slot_size;
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

    (void)userid;  /* DSR has no Steam ID in save data; parameter for API parity */

    file_data = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    if (!file_data) {
        return false;
    }

    /* BND4 header */
    CopyMemory(file_data, "BND4", 4);
    *(uint32_t *)(file_data + DSR_BND4_SLOT_COUNT_OFFSET) = (uint32_t)DSR_BND4_ENTRY_COUNT;
    for (i = 0; i < DSR_BND4_ENTRY_COUNT; i++) {
        *(uint32_t *)(file_data + DSR_BND4_SIZE_ARRAY_OFFSET + (uint32_t)i * DSR_BND4_ENTRY_STRIDE) = slot_size;
        *(uint32_t *)(file_data + DSR_BND4_OFFSET_ARRAY_OFFSET + (uint32_t)i * DSR_BND4_ENTRY_STRIDE) = header_size + (uint32_t)i * slot_size;
    }

    /* BCrypt setup: AES-128-CBC with DSR master key */
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
                                        (PUCHAR)DSR_AES_KEY_BYTES, 16, 0);
    if (!NT_SUCCESS(status)) {
        LocalFree(key_obj);
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(file_data);
        return false;
    }

    /* Encrypt each of the 11 slots */
    ok = true;
    for (i = 0; i < DSR_BND4_ENTRY_COUNT && ok; i++) {
        uint32_t slot_offset = header_size + (uint32_t)i * slot_size;
        uint8_t *plaintext;
        uint8_t *md5_buf;
        uint8_t iv_scratch[16];
        uint8_t *ct_buf;
        ULONG ct_written = 0;

        plaintext = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, pt_size);
        if (!plaintext) {
            ok = false;
            break;
        }

        if (i == 10) {
            /* Summary slot: active_slot is a 1-byte field in DSR (not int32). */
            plaintext[DSR_SUMMARY_ACTIVE_OFFSET] = 0;
            plaintext[DSR_SUMMARY_AVAILABLE_OFFSET] = 1;
        }

        /* Encrypt into file buffer at slot_offset + 32 (after MD5 + IV) */
        ct_buf = file_data + slot_offset + DSR_MD5_HEADER_SIZE + 16u;
        CopyMemory(iv_scratch, DSR_TEST_IV, 16);
        status = BCryptEncrypt(key, (PUCHAR)plaintext, pt_size, NULL, iv_scratch, 16,
                                ct_buf, ct_size, &ct_written, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(status) || ct_written != ct_size) {
            LocalFree(plaintext);
            ok = false;
            break;
        }

        /* Write IV right after MD5 header in the file buffer */
        CopyMemory(file_data + slot_offset + DSR_MD5_HEADER_SIZE, DSR_TEST_IV, 16);

        /* MD5(IV || ciphertext) into the slot's MD5 header */
        md5_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, 16u + ct_size);
        if (!md5_buf) {
            LocalFree(plaintext);
            ok = false;
            break;
        }
        CopyMemory(md5_buf, DSR_TEST_IV, 16);
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

/* ==========================================================================
 * Subcommand handlers
 * ========================================================================== */

/* dsr-aes-known-vector
 * Validates BCrypt AES-128-CBC PKCS7 wiring with DSR key + IV against the
 * pre-computed ciphertext in dsr_test_format.h. Independent of the fixture
 * builder. */
static int handle_dsr_aes_known_vector(int argc, wchar_t **argv) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    ULONG result_size = 0;
    NTSTATUS status;
    bool ok = true;

    (void)argc;
    (void)argv;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        dsr_printf(L"FAIL: BCryptOpenAlgorithmProvider\n");
        ok = false;
    }
    if (ok) {
        status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                    (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                    sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
        if (!NT_SUCCESS(status)) {
            dsr_printf(L"FAIL: BCryptSetProperty\n");
            ok = false;
        }
    }
    if (ok) {
        status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                    (PUCHAR)&key_obj_size, sizeof(ULONG),
                                    &result_size, 0);
        if (!NT_SUCCESS(status)) {
            dsr_printf(L"FAIL: BCryptGetProperty\n");
            ok = false;
        }
    }
    if (ok) {
        key_obj = (uint8_t *)LocalAlloc(LMEM_FIXED, key_obj_size);
        if (!key_obj) {
            dsr_printf(L"FAIL: LocalAlloc\n");
            ok = false;
        }
    }
    if (ok) {
        status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size,
                                            (PUCHAR)DSR_AES_KEY_BYTES, 16, 0);
        if (!NT_SUCCESS(status)) {
            dsr_printf(L"FAIL: BCryptGenerateSymmetricKey\n");
            ok = false;
        }
    }

    if (ok) {
        uint8_t iv_copy[16];
        uint8_t ct_buf[64];
        ULONG ct_len = 0;

        CopyMemory(iv_copy, DSR_TEST_IV, 16);
        status = BCryptEncrypt(key, (PUCHAR)DSR_TEST_KNOWN_PLAINTEXT, 16, NULL,
                                iv_copy, 16, ct_buf, sizeof(ct_buf), &ct_len,
                                BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(status)) {
            dsr_printf(L"FAIL: BCryptEncrypt\n");
            ok = false;
        }
        if (ok && (ct_len != DSR_TEST_KNOWN_CIPHERTEXT_SIZE
                   || RtlCompareMemory(ct_buf, DSR_TEST_KNOWN_CIPHERTEXT,
                                       DSR_TEST_KNOWN_CIPHERTEXT_SIZE)
                      != DSR_TEST_KNOWN_CIPHERTEXT_SIZE)) {
            dsr_printf(L"FAIL: ciphertext mismatch\n");
            ok = false;
        }

        if (ok) {
            uint8_t pt_buf[64];
            ULONG pt_len = 0;

            CopyMemory(iv_copy, DSR_TEST_IV, 16);
            status = BCryptDecrypt(key, ct_buf, ct_len, NULL, iv_copy, 16,
                                    pt_buf, sizeof(pt_buf), &pt_len,
                                    BCRYPT_BLOCK_PADDING);
            if (!NT_SUCCESS(status) || pt_len != 16
                || RtlCompareMemory(pt_buf, DSR_TEST_KNOWN_PLAINTEXT, 16) != 16) {
                dsr_printf(L"FAIL: decrypt mismatch\n");
                ok = false;
            }
        }
    }

    if (key) BCryptDestroyKey(key);
    if (key_obj) LocalFree(key_obj);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);

    if (ok) {
        dsr_printf(L"PASS: dsr-aes-known-vector\n");
        return 0;
    }
    return 1;
}

/* dsr-null-guards
 * Verify each public dsr_* function rejects NULL / out-of-range inputs cleanly
 * without crashing or returning success. */
static int handle_dsr_null_guards(int argc, wchar_t **argv) {
    bool ok = true;
    int slot_out = -1;
    dsr_save_data_t *probe = NULL;
    uint8_t *dummy_buf;
    wchar_t tmp_path[MAX_PATH];
    dsr_save_data_t *save = NULL;
    dsr_char_data_t *c0 = NULL;

    (void)argc;
    (void)argv;

    dummy_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DSR_CHAR_DATA_SERIALIZED_SIZE);
    if (!dummy_buf) {
        dsr_printf(L"FAIL: dummy_buf alloc failed\n");
        return 1;
    }

    /* dsr_save_data_load(NULL, &out) returns false */
    if (dsr_save_data_load(NULL, &probe)) {
        dsr_printf(L"FAIL: load(NULL, &out) should return false\n");
        ok = false;
    }
    /* dsr_save_data_load(L"...", NULL) returns false */
    if (dsr_save_data_load(L"dummy-path.sl2", NULL)) {
        dsr_printf(L"FAIL: load(path, NULL) should return false\n");
        ok = false;
    }
    /* dsr_save_data_free(NULL) does not crash */
    dsr_save_data_free(NULL);
    /* dsr_save_get_active_slot(NULL, &slot) returns false */
    if (dsr_save_get_active_slot(NULL, &slot_out)) {
        dsr_printf(L"FAIL: get_active_slot(NULL, ...) should return false\n");
        ok = false;
    }

    /* Build a fixture for the rest of the checks */
    tmp_path[0] = L'\0';
    GetTempPathW(MAX_PATH, tmp_path);
    {
        size_t len = wcslen(tmp_path);
        const wchar_t *suffix = L"dsr-null-guards-test.sl2";
        if (len + wcslen(suffix) + 1 < MAX_PATH) {
            wcscat_s(tmp_path, MAX_PATH, suffix);
        }
    }

    if (!praxis_make_min_valid_dsr_sl2(tmp_path, DSR_TEST_USERID_A)) {
        dsr_printf(L"FAIL: fixture builder failed\n");
        LocalFree(dummy_buf);
        return 1;
    }
    if (!dsr_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        LocalFree(dummy_buf);
        dsr_printf(L"FAIL: load fixture\n");
        return 1;
    }

    /* dsr_save_get_active_slot(valid_save, NULL) returns false */
    if (dsr_save_get_active_slot(save, NULL)) {
        dsr_printf(L"FAIL: get_active_slot(..., NULL) should return false\n");
        ok = false;
    }
    /* dsr_char_data_ref(NULL, 0) returns NULL */
    if (dsr_char_data_ref(NULL, 0) != NULL) {
        dsr_printf(L"FAIL: ref(NULL, 0) should return NULL\n");
        ok = false;
    }
    /* dsr_char_data_ref(valid_save, -1) returns NULL */
    if (dsr_char_data_ref(save, -1) != NULL) {
        dsr_printf(L"FAIL: ref(save, -1) should return NULL\n");
        ok = false;
    }
    /* dsr_char_data_ref(valid_save, 10) returns NULL (valid range 0-9) */
    if (dsr_char_data_ref(save, 10) != NULL) {
        dsr_printf(L"FAIL: ref(save, 10) should return NULL\n");
        ok = false;
    }

    c0 = dsr_char_data_ref(save, 0);

    /* dsr_char_data_serialize(NULL, buf, size) returns false */
    if (dsr_char_data_serialize(NULL, dummy_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: serialize(NULL, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_serialize(valid_char, NULL, size) returns false */
    if (c0 && dsr_char_data_serialize(c0, NULL, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: serialize(..., NULL, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_serialize(valid_char, buf, 0) returns false */
    if (c0 && dsr_char_data_serialize(c0, dummy_buf, 0)) {
        dsr_printf(L"FAIL: serialize(..., ..., 0) should return false\n");
        ok = false;
    }
    /* dsr_char_data_import_raw(NULL, 0, buf, size) returns false */
    if (dsr_char_data_import_raw(NULL, 0, dummy_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: import_raw(NULL, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_import_raw(valid_save, 0, NULL, size) returns false */
    if (dsr_char_data_import_raw(save, 0, NULL, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: import_raw(..., ..., NULL, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_import_raw(valid_save, -1, buf, size) returns false */
    if (dsr_char_data_import_raw(save, -1, dummy_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: import_raw(..., -1, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_import_raw(valid_save, 10, buf, size) returns false */
    if (dsr_char_data_import_raw(save, 10, dummy_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_printf(L"FAIL: import_raw(..., 10, ...) should return false\n");
        ok = false;
    }
    /* dsr_char_data_import_raw(valid_save, 0, buf, 0) returns false */
    if (dsr_char_data_import_raw(save, 0, dummy_buf, 0)) {
        dsr_printf(L"FAIL: import_raw(..., ..., ..., 0) should return false\n");
        ok = false;
    }

    dsr_save_data_free(save);
    DeleteFileW(tmp_path);
    LocalFree(dummy_buf);

    if (!ok) {
        return 1;
    }
    dsr_printf(L"PASS: dsr-null-guards\n");
    return 0;
}

/* dsr-load-min-fixture <tmp>
 * Build a minimal valid DSR fixture, load it, verify active slot is 0,
 * verify slot 0 is non-NULL, and verify slot 1 is NULL. Destructive: deletes
 * <tmp> after assertion. */
static int handle_dsr_load_min_fixture(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    dsr_save_data_t *save = NULL;
    int slot = -1;
    dsr_char_data_t *c0;
    dsr_char_data_t *c1;

    if (argc < 4) {
        dsr_printf(L"Usage: dsr-load-min-fixture <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_dsr_sl2(tmp_path, DSR_TEST_USERID_A)) {
        dsr_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!dsr_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: dsr_save_data_load returned false/NULL\n");
        return 1;
    }
    if (!dsr_save_get_active_slot(save, &slot) || slot != 0) {
        dsr_save_data_free(save);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: active slot expected 0, got %d\n", slot);
        return 1;
    }
    c0 = dsr_char_data_ref(save, 0);
    if (c0 == NULL) {
        dsr_save_data_free(save);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: slot 0 should be non-NULL\n");
        return 1;
    }
    c1 = dsr_char_data_ref(save, 1);
    if (c1 != NULL) {
        dsr_save_data_free(save);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: slot 1 should be NULL\n");
        return 1;
    }

    dsr_save_data_free(save);
    DeleteFileW(tmp_path);
    dsr_printf(L"PASS: dsr-load-min-fixture\n");
    return 0;
}

/* dsr-roundtrip-byte-stable <tmp>
 * Build a fixture, read file bytes A, load + serialize slot 0 + import_raw
 * the same bytes back, write file, read file bytes B, assert A == B. A no-op
 * roundtrip must be byte-stable. Destructive: deletes <tmp>. */
static int handle_dsr_roundtrip_byte_stable(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint8_t *buf_a = NULL;
    uint8_t *buf_b = NULL;
    uint8_t *ser_buf = NULL;
    dsr_save_data_t *save = NULL;
    dsr_char_data_t *c0 = NULL;
    bool bytes_equal = false;

    if (argc < 4) {
        dsr_printf(L"Usage: dsr-roundtrip-byte-stable <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_dsr_sl2(tmp_path, DSR_TEST_USERID_A)) {
        dsr_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: open A\n");
        return 1;
    }
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
        dsr_printf(L"FAIL: read A\n");
        return 1;
    }
    CloseHandle(fh);

    if (!dsr_save_data_load(tmp_path, &save) || !save) {
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: load\n");
        return 1;
    }
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DSR_CHAR_DATA_SERIALIZED_SIZE);
    c0 = dsr_char_data_ref(save, 0);
    if (!ser_buf || c0 == NULL) {
        if (ser_buf) LocalFree(ser_buf);
        dsr_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: ref\n");
        return 1;
    }
    if (!dsr_char_data_serialize(c0, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        dsr_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!dsr_char_data_import_raw(save, 0, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        dsr_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    dsr_save_data_free(save);

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE
        || !ReadFile(fh, buf_b, file_size, &bytes_read, NULL)
        || bytes_read != file_size) {
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: read B\n");
        return 1;
    }
    CloseHandle(fh);

    bytes_equal = (RtlCompareMemory(buf_a, buf_b, file_size) == file_size);
    LocalFree(buf_a);
    LocalFree(buf_b);
    DeleteFileW(tmp_path);

    if (!bytes_equal) {
        dsr_printf(L"FAIL: bytes differ after no-op roundtrip\n");
        return 1;
    }
    dsr_printf(L"PASS: dsr-roundtrip-byte-stable\n");
    return 0;
}

/* dsr-active-slot <tmp> <expected_int>
 * Build a fixture, load it, verify the active slot equals the expected value.
 * Destructive: deletes <tmp>. */
static int handle_dsr_active_slot(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    int expected;
    dsr_save_data_t *save = NULL;
    int slot = -1;
    bool got;

    if (argc < 5) {
        dsr_printf(L"Usage: dsr-active-slot <tmp_path> <expected_int>\n");
        return 1;
    }
    tmp_path = argv[3];
    expected = _wtoi(argv[4]);

    if (!praxis_make_min_valid_dsr_sl2(tmp_path, DSR_TEST_USERID_A)) {
        dsr_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!dsr_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        dsr_printf(L"FAIL: load\n");
        return 1;
    }

    got = dsr_save_get_active_slot(save, &slot);
    dsr_save_data_free(save);
    DeleteFileW(tmp_path);

    if (!got || slot != expected) {
        dsr_printf(L"FAIL: active slot %d, expected %d\n", slot, expected);
        return 1;
    }
    dsr_printf(L"PASS: dsr-active-slot (slot=%d)\n", slot);
    return 0;
}

/* dsr-cross-account-import <srcA> <dstB>
 * Build fixture A with USERID_A and fixture B with USERID_B, serialize A's
 * slot 0, import into B's slot 0, reload B, and verify clean load. DSR has
 * NO Steam ID in save data so this is a no-op verification — the test
 * exercises the import path end-to-end without a re-signing check.
 * Destructive: deletes both <srcA> and <dstB>. */
static int handle_dsr_cross_account_import(int argc, wchar_t **argv) {
    const wchar_t *path_a;
    const wchar_t *path_b;
    dsr_save_data_t *save_a = NULL;
    dsr_save_data_t *save_b = NULL;
    dsr_char_data_t *c0_a = NULL;
    uint8_t *ser_buf = NULL;
    bool ok = true;

    if (argc < 5) {
        dsr_printf(L"Usage: dsr-cross-account-import <tmp_path_A> <tmp_path_B>\n");
        return 1;
    }
    path_a = argv[3];
    path_b = argv[4];

    if (!praxis_make_min_valid_dsr_sl2(path_a, DSR_TEST_USERID_A)) {
        dsr_printf(L"FAIL: fixture A builder failed\n");
        return 1;
    }
    if (!praxis_make_min_valid_dsr_sl2(path_b, DSR_TEST_USERID_B)) {
        DeleteFileW(path_a);
        dsr_printf(L"FAIL: fixture B builder failed\n");
        return 1;
    }

    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DSR_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf) {
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: ser_buf alloc\n");
        return 1;
    }

    if (!dsr_save_data_load(path_a, &save_a) || !save_a) {
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: load A\n");
        return 1;
    }
    c0_a = dsr_char_data_ref(save_a, 0);
    if (c0_a == NULL) {
        dsr_save_data_free(save_a);
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: ref A slot 0\n");
        return 1;
    }
    if (!dsr_char_data_serialize(c0_a, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_save_data_free(save_a);
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: serialize A\n");
        return 1;
    }
    dsr_save_data_free(save_a);
    save_a = NULL;

    if (!dsr_save_data_load(path_b, &save_b) || !save_b) {
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: load B\n");
        return 1;
    }
    if (!dsr_char_data_import_raw(save_b, 0, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        dsr_save_data_free(save_b);
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: import_raw into B\n");
        return 1;
    }
    LocalFree(ser_buf);
    ser_buf = NULL;
    dsr_save_data_free(save_b);
    save_b = NULL;

    /* Reload B to verify it still loads cleanly after the cross-account import */
    if (!dsr_save_data_load(path_b, &save_b) || !save_b) {
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        dsr_printf(L"FAIL: reload B\n");
        return 1;
    }
    if (dsr_char_data_ref(save_b, 0) == NULL) {
        ok = false;
    }
    dsr_save_data_free(save_b);
    DeleteFileW(path_a);
    DeleteFileW(path_b);

    if (!ok) {
        dsr_printf(L"FAIL: reload B slot 0 ref NULL\n");
        return 1;
    }
    dsr_printf(L"PASS: dsr-cross-account-import (DSR has no Steam ID; no-op verified)\n");
    return 0;
}

/* dsr-real-save-load <path>
 * Load a real DSR save file and print the active slot plus availability of
 * all 10 char slots. Read-only: opens via dsr_save_data_load (which uses
 * read-only file access internally). Never modifies the file. If the file
 * does not exist, prints SKIP and exits 0 (CI-safe). */
static int handle_dsr_real_save_load(int argc, wchar_t **argv) {
    const wchar_t *path;
    dsr_save_data_t *save = NULL;
    int slot = -1;
    int i;

    if (argc < 4) {
        dsr_printf(L"Usage: dsr-real-save-load <path>\n");
        return 1;
    }
    path = argv[3];

    if (!dsr_file_exists(path)) {
        dsr_printf(L"SKIP: dsr-real-save-load \u2014 real save not available at %ls\n", path);
        return 0;
    }

    if (!dsr_save_data_load(path, &save) || !save) {
        dsr_printf(L"FAIL: dsr_save_data_load returned false/NULL for real save\n");
        return 1;
    }

    if (dsr_save_get_active_slot(save, &slot)) {
        dsr_printf(L"Active slot: %d\n", slot);
    } else {
        dsr_printf(L"Active slot: (not available)\n");
    }

    for (i = 0; i < 10; i++) {
        dsr_char_data_t *c = dsr_char_data_ref(save, i);
        dsr_printf(L"Slot %d: %ls\n", i, c ? L"available" : L"empty");
    }

    dsr_save_data_free(save);
    dsr_printf(L"PASS: dsr-real-save-load\n");
    return 0;
}

/* dsr-real-save-classify <path>
 * Open a real DSR save read-only (GENERIC_READ | FILE_SHARE_READ), verify
 * the BND4 magic at offset 0 and that slot count is >= DSR_BND4_ENTRY_COUNT
 * (11) at offset DSR_BND4_SLOT_COUNT_OFFSET. Never modifies the file.
 * If the file does not exist, prints SKIP and exits 0. */
static int handle_dsr_real_save_classify(int argc, wchar_t **argv) {
    const wchar_t *path;
    HANDLE fh;
    DWORD file_size = 0;
    uint8_t header[0x10];
    uint8_t slot_count_buf[4];
    DWORD bytes_read = 0;
    int slot_count;

    if (argc < 4) {
        dsr_printf(L"Usage: dsr-real-save-classify <path>\n");
        return 1;
    }
    path = argv[3];

    if (!dsr_file_exists(path)) {
        dsr_printf(L"SKIP: dsr-real-save-classify \u2014 real save not available at %ls\n", path);
        return 0;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        dsr_printf(L"FAIL: cannot open file\n");
        return 1;
    }
    file_size = GetFileSize(fh, NULL);

    if (!ReadFile(fh, header, sizeof(header), &bytes_read, NULL)
        || bytes_read != sizeof(header)) {
        CloseHandle(fh);
        dsr_printf(L"FAIL: cannot read header\n");
        return 1;
    }
    if (header[0] != 'B' || header[1] != 'N' || header[2] != 'D' || header[3] != '4') {
        CloseHandle(fh);
        dsr_printf(L"FAIL: not a BND4 file\n");
        return 1;
    }

    SetFilePointer(fh, DSR_BND4_SLOT_COUNT_OFFSET, NULL, FILE_BEGIN);
    if (!ReadFile(fh, slot_count_buf, 4, &bytes_read, NULL) || bytes_read != 4) {
        CloseHandle(fh);
        dsr_printf(L"FAIL: cannot read slot count\n");
        return 1;
    }
    CloseHandle(fh);
    slot_count = *(int *)slot_count_buf;

    if (slot_count < DSR_BND4_ENTRY_COUNT) {
        dsr_printf(L"FAIL: slot count %d < %d\n", slot_count, DSR_BND4_ENTRY_COUNT);
        return 1;
    }

    dsr_printf(L"File size: %u bytes\n", file_size);
    dsr_printf(L"Slot count: %d\n", slot_count);
    dsr_printf(L"PASS: dsr-real-save-classify\n");
    return 0;
}

/* dsr-real-save-roundtrip-readonly <path> <tmp_copy>
 * Verify the read-only contract on a real DSR save:
 *   1. Compute SHA256 of <path> (BEFORE).
 *   2. CopyFileW <path> to <tmp_copy>.
 *   3. Load <tmp_copy>, get active slot, serialize first available char data,
 *      import_raw the same bytes back into the same slot (no-op semantically).
 *   4. Reload <tmp_copy>, verify active slot is unchanged.
 *   5. DeleteFileW <tmp_copy>.
 *   6. Compute SHA256 of <path> (AFTER), assert BEFORE == AFTER.
 * If the file does not exist, prints SKIP and exits 0. */
static int handle_dsr_real_save_roundtrip_readonly(int argc, wchar_t **argv) {
    const wchar_t *path;
    const wchar_t *tmp_copy;
    uint8_t hash_before[32];
    uint8_t hash_after[32];
    dsr_save_data_t *save = NULL;
    int active_slot = -1;
    int active_slot_after = -1;
    int first_slot = -1;
    int i;
    dsr_char_data_t *c = NULL;
    uint8_t *ser_buf = NULL;

    if (argc < 5) {
        dsr_printf(L"Usage: dsr-real-save-roundtrip-readonly <path> <tmp_copy>\n");
        return 1;
    }
    path = argv[3];
    tmp_copy = argv[4];

    if (!dsr_file_exists(path)) {
        dsr_printf(L"SKIP: dsr-real-save-roundtrip-readonly \u2014 real save not available at %ls\n", path);
        return 0;
    }

    if (!dsr_compute_file_sha256(path, hash_before)) {
        dsr_printf(L"FAIL: sha256 BEFORE\n");
        return 1;
    }

    if (!CopyFileW(path, tmp_copy, FALSE)) {
        dsr_printf(L"FAIL: CopyFileW to tmp_copy (error 0x%08X)\n", (unsigned)GetLastError());
        return 1;
    }

    if (!dsr_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        dsr_printf(L"FAIL: load tmp_copy\n");
        return 1;
    }
    dsr_save_get_active_slot(save, &active_slot);

    for (i = 0; i < 10; i++) {
        if (dsr_char_data_ref(save, i) != NULL) {
            first_slot = i;
            break;
        }
    }

    if (first_slot < 0) {
        dsr_save_data_free(save);
        DeleteFileW(tmp_copy);
        if (!dsr_compute_file_sha256(path, hash_after)) {
            dsr_printf(L"FAIL: sha256 AFTER (no slots case)\n");
            return 1;
        }
        if (RtlCompareMemory(hash_before, hash_after, 32) != 32) {
            dsr_printf(L"FAIL: <path> hash changed during no-slots run\n");
            return 1;
        }
        dsr_printf(L"SKIP: no available char slots in save\n");
        return 0;
    }

    c = dsr_char_data_ref(save, first_slot);
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DSR_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf) {
        dsr_save_data_free(save);
        DeleteFileW(tmp_copy);
        dsr_printf(L"FAIL: alloc ser_buf\n");
        return 1;
    }
    if (!dsr_char_data_serialize(c, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        dsr_save_data_free(save);
        DeleteFileW(tmp_copy);
        dsr_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!dsr_char_data_import_raw(save, first_slot, ser_buf, DSR_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        dsr_save_data_free(save);
        DeleteFileW(tmp_copy);
        dsr_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    dsr_save_data_free(save);
    save = NULL;

    if (!dsr_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        dsr_printf(L"FAIL: reload after roundtrip\n");
        return 1;
    }
    dsr_save_get_active_slot(save, &active_slot_after);
    dsr_save_data_free(save);
    DeleteFileW(tmp_copy);

    if (active_slot != active_slot_after) {
        dsr_printf(L"FAIL: active slot changed from %d to %d\n",
            active_slot, active_slot_after);
        return 1;
    }

    if (!dsr_compute_file_sha256(path, hash_after)) {
        dsr_printf(L"FAIL: sha256 AFTER\n");
        return 1;
    }
    if (RtlCompareMemory(hash_before, hash_after, 32) != 32) {
        int j;
        dsr_printf(L"FAIL: <path> hash changed during read-only roundtrip\n");
        dsr_printf(L"  before: ");
        for (j = 0; j < 32; j++) dsr_printf(L"%02X", hash_before[j]);
        dsr_printf(L"\n  after:  ");
        for (j = 0; j < 32; j++) dsr_printf(L"%02X", hash_after[j]);
        dsr_printf(L"\n");
        return 1;
    }

    dsr_printf(L"PASS: dsr-real-save-roundtrip-readonly (slot %d, active=%d, source hash unchanged)\n",
        first_slot, active_slot);
    return 0;
}

/* ==========================================================================
 * Dispatcher
 * ========================================================================== */

int praxis_selftest_dsr_dispatch(int argc, wchar_t **argv, const wchar_t *sub) {
    if (wcscmp(sub, L"dsr-aes-known-vector") == 0) return handle_dsr_aes_known_vector(argc, argv);
    if (wcscmp(sub, L"dsr-null-guards") == 0) return handle_dsr_null_guards(argc, argv);
    if (wcscmp(sub, L"dsr-load-min-fixture") == 0) return handle_dsr_load_min_fixture(argc, argv);
    if (wcscmp(sub, L"dsr-roundtrip-byte-stable") == 0) return handle_dsr_roundtrip_byte_stable(argc, argv);
    if (wcscmp(sub, L"dsr-active-slot") == 0) return handle_dsr_active_slot(argc, argv);
    if (wcscmp(sub, L"dsr-cross-account-import") == 0) return handle_dsr_cross_account_import(argc, argv);
    if (wcscmp(sub, L"dsr-real-save-load") == 0) return handle_dsr_real_save_load(argc, argv);
    if (wcscmp(sub, L"dsr-real-save-classify") == 0) return handle_dsr_real_save_classify(argc, argv);
    if (wcscmp(sub, L"dsr-real-save-roundtrip-readonly") == 0) return handle_dsr_real_save_roundtrip_readonly(argc, argv);
    return -1;
}
