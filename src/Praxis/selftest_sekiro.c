/**
 * @file selftest_sekiro.c
 * @brief Sekiro selftest subcommand handlers and fixture builder.
 * @details Implements 8 Sekiro-specific selftest subcommands plus a minimal
 *          valid Sekiro .sl2 fixture builder. Linked only into the
 *          PraxisSelftest executable target (never into the praxis GUI
 *          target). The dispatcher hook into praxis_selftest_run is wired
 *          separately by a later task.
 *
 *          Sekiro saves are UNENCRYPTED. Slot layout on disk is
 *          [16-byte MD5][raw plaintext] — there is NO AES, NO IV, and NO
 *          PKCS7 padding. As a result this file contains NO AES code and
 *          there is intentionally NO `sekiro-aes-known-vector` subcommand.
 *          BCrypt is used only for SHA256 fingerprinting of real save
 *          files (BCRYPT_SHA256_ALGORITHM, never BCRYPT_AES_ALGORITHM).
 */

#include "selftest_sekiro.h"
#include "sekiro_test_format.h"
#include "../common/sekirosave.h"

#include "../../deps/md5/md5.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
/* NOTE: <bcrypt.h> is included for BCRYPT_SHA256_ALGORITHM only.
 * Sekiro is unencrypted — this TU never opens BCRYPT_AES_ALGORITHM. */

/* On-disk header in front of each Sekiro BND4 slot: [16-byte MD5][plaintext].
 * Unlike DS3/DSR, there is NO IV — Sekiro is unencrypted. */
#define SEKIRO_MD5_HEADER_SIZE 0x10u

/* Formatted wide-char printf for Sekiro selftest output (mirrors st_printf in
 * praxis_selftest.c, which is static and not accessible from this TU). */
static void sekiro_printf(const wchar_t *fmt, ...) {
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
static bool sekiro_file_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* Compute SHA256 of the file at path. out_hash receives 32 bytes on success.
 * Opens the file with GENERIC_READ | FILE_SHARE_READ only — never modifies it.
 * Uses BCRYPT_SHA256_ALGORITHM only — no AES is involved. */
static bool sekiro_sha256_file(const wchar_t *path, uint8_t out_hash[32]) {
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

/* Creates a minimal valid Sekiro BND4 .sl2 fixture at path.
 *
 * Builds 12 BND4 entries (10 char + 1 summary + 1 regulation) using empirical
 * Sekiro on-disk sizes. Sekiro is UNENCRYPTED — slot layout per slot is
 * [16-byte MD5][raw plaintext]. MD5 is computed over the plaintext only.
 *
 * Slot layout:
 *   - Slot 0       (char):       N = 0x10 @ char[0x0C],
 *                                userid (uint64 LE) @ N + 0x44 = 0x54
 *                                MD5(plaintext) at slot start
 *   - Slots 1..9   (char):       all-zero on disk; availability byte will be
 *                                0 in the summary so the loader skips MD5
 *                                verification for these slots
 *   - Slot 10      (summary):    userid (uint64 LE) @ 0x24,
 *                                active = 0 (int32 LE) @ 0x2508,
 *                                availability[0] = 1 @ 0xD4
 *                                MD5(plaintext) at slot start
 *   - Slot 11      (regulation): all-zero placeholder of char-slot size;
 *                                never accessed as character data
 *
 * The userid parameter is stamped into both the summary and slot 0 char
 * plaintexts so the cross-account import test can build mismatched fixtures.
 *
 * Returns true on success. */
static bool praxis_make_min_valid_sekiro_sl2(const wchar_t *path, uint64_t userid) {
    const uint32_t header_size = SEKIRO_BND4_FILE_HEADER_SIZE;
    const uint32_t char_slot_size = SEKIRO_CHAR_SLOT_ON_DISK_SIZE;
    const uint32_t summary_slot_size = SEKIRO_SUMMARY_SLOT_ON_DISK_SIZE;
    /* Per task spec: regulation slot is a char-slot-sized zero placeholder. */
    const uint32_t regulation_slot_size = SEKIRO_CHAR_SLOT_ON_DISK_SIZE;
    const uint32_t total_size = header_size
        + 10u * char_slot_size
        + summary_slot_size
        + regulation_slot_size;
    uint32_t slot_offsets[SEKIRO_BND4_ENTRY_COUNT];
    uint32_t slot_sizes[SEKIRO_BND4_ENTRY_COUNT];
    uint32_t running_offset;
    uint8_t *file_data = NULL;
    HANDLE file;
    DWORD written = 0;
    bool ok;
    int i;

    file_data = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    if (!file_data) {
        return false;
    }

    /* BND4 header */
    CopyMemory(file_data, "BND4", 4);
    *(uint32_t *)(file_data + SEKIRO_BND4_SLOT_COUNT_OFFSET) = (uint32_t)SEKIRO_BND4_ENTRY_COUNT;

    /* Slot offsets/sizes: 0..9 = char, 10 = summary, 11 = regulation. */
    running_offset = header_size;
    for (i = 0; i < 10; i++) {
        slot_sizes[i] = char_slot_size;
        slot_offsets[i] = running_offset;
        running_offset += char_slot_size;
    }
    slot_sizes[10] = summary_slot_size;
    slot_offsets[10] = running_offset;
    running_offset += summary_slot_size;
    slot_sizes[SEKIRO_REGULATION_SLOT_INDEX] = regulation_slot_size;
    slot_offsets[SEKIRO_REGULATION_SLOT_INDEX] = running_offset;

    /* Write slot table */
    for (i = 0; i < SEKIRO_BND4_ENTRY_COUNT; i++) {
        *(uint32_t *)(file_data + SEKIRO_BND4_SIZE_ARRAY_OFFSET
                      + (uint32_t)i * SEKIRO_BND4_ENTRY_STRIDE) = slot_sizes[i];
        *(uint32_t *)(file_data + SEKIRO_BND4_OFFSET_ARRAY_OFFSET
                      + (uint32_t)i * SEKIRO_BND4_ENTRY_STRIDE) = slot_offsets[i];
    }

    /* Slot 0 (char): N=0x10 @ char[0x0C], userid @ N+0x44 = 0x54.
     * Bounds-check N + 0x44 + 8 against SEKIRO_CHAR_PLAINTEXT_SIZE before
     * writing the userid (the loader applies the same check). */
    {
        uint8_t *slot0_md5 = file_data + slot_offsets[0];
        uint8_t *slot0_pt = slot0_md5 + SEKIRO_MD5_HEADER_SIZE;
        uint32_t n_value = 0x10u;
        uint32_t userid_offset = n_value + SEKIRO_CHAR_USERID_DELTA;  /* 0x54 */

        if (n_value > SEKIRO_CHAR_PLAINTEXT_SIZE
            || SEKIRO_CHAR_USERID_DELTA > SEKIRO_CHAR_PLAINTEXT_SIZE - n_value
            || userid_offset > SEKIRO_CHAR_PLAINTEXT_SIZE - sizeof(uint64_t)) {
            LocalFree(file_data);
            return false;
        }

        /* Plaintext is already zero-initialized; patch the two fields. */
        CopyMemory(slot0_pt + SEKIRO_CHAR_USERID_LEN_OFFSET, &n_value, sizeof(uint32_t));
        CopyMemory(slot0_pt + userid_offset, &userid, sizeof(uint64_t));

        md5_buffer(slot0_pt, SEKIRO_CHAR_PLAINTEXT_SIZE, slot0_md5);
    }

    /* Slots 1..9 (char): leave entire on-disk slot zero (already zero-init).
     * Availability bytes for these slots stay 0 in the summary so the loader
     * skips MD5 verification and marks them unavailable. */

    /* Slot 10 (summary): userid @ 0x24, active=0 @ 0x2508, availability[0]=1 @ 0xD4. */
    {
        uint8_t *summary_md5 = file_data + slot_offsets[10];
        uint8_t *summary_pt = summary_md5 + SEKIRO_MD5_HEADER_SIZE;
        int32_t active_zero = 0;

        CopyMemory(summary_pt + SEKIRO_SUMMARY_USERID_OFFSET, &userid, sizeof(uint64_t));
        CopyMemory(summary_pt + SEKIRO_SUMMARY_ACTIVE_OFFSET, &active_zero, sizeof(int32_t));
        summary_pt[SEKIRO_SUMMARY_AVAILABLE_OFFSET] = 1;  /* slot 0 available */

        md5_buffer(summary_pt, SEKIRO_SUMMARY_PLAINTEXT_SIZE, summary_md5);
    }

    /* Slot 11 (regulation): leave all-zero placeholder. The loader only
     * range-validates this slot and never reads its content as character data. */

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

/* sekiro-null-guards
 * Verify each public sekiro_* function rejects NULL / out-of-range inputs
 * cleanly without crashing or returning success. */
static int handle_sekiro_null_guards(int argc, wchar_t **argv) {
    bool ok = true;
    int slot_out = -1;
    sekiro_save_data_t *probe = NULL;
    uint8_t *dummy_buf;
    wchar_t tmp_path[MAX_PATH];
    sekiro_save_data_t *save = NULL;
    sekiro_char_data_t *c0 = NULL;

    (void)argc;
    (void)argv;

    dummy_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    if (!dummy_buf) {
        sekiro_printf(L"FAIL: dummy_buf alloc failed\n");
        return 1;
    }

    /* sekiro_save_data_load(NULL, &out) returns false */
    if (sekiro_save_data_load(NULL, &probe)) {
        sekiro_printf(L"FAIL: load(NULL, &out) should return false\n");
        ok = false;
    }
    /* sekiro_save_data_load(L"...", NULL) returns false */
    if (sekiro_save_data_load(L"dummy-path.sl2", NULL)) {
        sekiro_printf(L"FAIL: load(path, NULL) should return false\n");
        ok = false;
    }
    /* sekiro_save_data_free(NULL) does not crash */
    sekiro_save_data_free(NULL);
    /* sekiro_save_get_active_slot(NULL, &slot) returns false */
    if (sekiro_save_get_active_slot(NULL, &slot_out)) {
        sekiro_printf(L"FAIL: get_active_slot(NULL, ...) should return false\n");
        ok = false;
    }

    /* Build a fixture for the rest of the checks */
    tmp_path[0] = L'\0';
    GetTempPathW(MAX_PATH, tmp_path);
    {
        const wchar_t *suffix = L"sekiro-null-guards-test.sl2";
        size_t len = wcslen(tmp_path);
        if (len + wcslen(suffix) + 1 < MAX_PATH) {
            wcscat_s(tmp_path, MAX_PATH, suffix);
        }
    }

    if (!praxis_make_min_valid_sekiro_sl2(tmp_path, SEKIRO_TEST_USERID_A)) {
        sekiro_printf(L"FAIL: fixture builder failed\n");
        LocalFree(dummy_buf);
        return 1;
    }
    if (!sekiro_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        LocalFree(dummy_buf);
        sekiro_printf(L"FAIL: load fixture\n");
        return 1;
    }

    /* sekiro_save_get_active_slot(valid_save, NULL) returns false */
    if (sekiro_save_get_active_slot(save, NULL)) {
        sekiro_printf(L"FAIL: get_active_slot(..., NULL) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_ref(NULL, 0) returns NULL */
    if (sekiro_char_data_ref(NULL, 0) != NULL) {
        sekiro_printf(L"FAIL: ref(NULL, 0) should return NULL\n");
        ok = false;
    }
    /* sekiro_char_data_ref(valid_save, -1) returns NULL */
    if (sekiro_char_data_ref(save, -1) != NULL) {
        sekiro_printf(L"FAIL: ref(save, -1) should return NULL\n");
        ok = false;
    }
    /* sekiro_char_data_ref(valid_save, 10) returns NULL (valid range 0-9) */
    if (sekiro_char_data_ref(save, 10) != NULL) {
        sekiro_printf(L"FAIL: ref(save, 10) should return NULL\n");
        ok = false;
    }

    c0 = sekiro_char_data_ref(save, 0);

    /* sekiro_char_data_serialize(NULL, buf, size) returns false */
    if (sekiro_char_data_serialize(NULL, dummy_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: serialize(NULL, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_serialize(valid_char, NULL, size) returns false */
    if (c0 && sekiro_char_data_serialize(c0, NULL, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: serialize(..., NULL, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_serialize(valid_char, buf, 0) returns false */
    if (c0 && sekiro_char_data_serialize(c0, dummy_buf, 0)) {
        sekiro_printf(L"FAIL: serialize(..., ..., 0) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_import_raw(NULL, 0, buf, size) returns false */
    if (sekiro_char_data_import_raw(NULL, 0, dummy_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: import_raw(NULL, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_import_raw(valid_save, 0, NULL, size) returns false */
    if (sekiro_char_data_import_raw(save, 0, NULL, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: import_raw(..., ..., NULL, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_import_raw(valid_save, -1, buf, size) returns false */
    if (sekiro_char_data_import_raw(save, -1, dummy_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: import_raw(..., -1, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_import_raw(valid_save, 10, buf, size) returns false */
    if (sekiro_char_data_import_raw(save, 10, dummy_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_printf(L"FAIL: import_raw(..., 10, ...) should return false\n");
        ok = false;
    }
    /* sekiro_char_data_import_raw(valid_save, 0, buf, 0) returns false */
    if (sekiro_char_data_import_raw(save, 0, dummy_buf, 0)) {
        sekiro_printf(L"FAIL: import_raw(..., ..., ..., 0) should return false\n");
        ok = false;
    }

    sekiro_save_data_free(save);
    DeleteFileW(tmp_path);
    LocalFree(dummy_buf);

    if (!ok) {
        return 1;
    }
    sekiro_printf(L"PASS: sekiro-null-guards\n");
    return 0;
}

/* sekiro-load-min-fixture <tmp>
 * Build a minimal valid Sekiro fixture, load it, verify active slot is 0,
 * verify slot 0 is non-NULL, and verify slot 1 is NULL. Destructive: deletes
 * <tmp> after assertion. */
static int handle_sekiro_load_min_fixture(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    sekiro_save_data_t *save = NULL;
    int slot = -1;
    sekiro_char_data_t *c0;
    sekiro_char_data_t *c1;

    if (argc < 4) {
        sekiro_printf(L"Usage: sekiro-load-min-fixture <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_sekiro_sl2(tmp_path, SEKIRO_TEST_USERID_A)) {
        sekiro_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!sekiro_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: sekiro_save_data_load returned false/NULL\n");
        return 1;
    }
    if (!sekiro_save_get_active_slot(save, &slot) || slot != 0) {
        sekiro_save_data_free(save);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: active slot expected 0, got %d\n", slot);
        return 1;
    }
    c0 = sekiro_char_data_ref(save, 0);
    if (c0 == NULL) {
        sekiro_save_data_free(save);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: slot 0 should be non-NULL\n");
        return 1;
    }
    c1 = sekiro_char_data_ref(save, 1);
    if (c1 != NULL) {
        sekiro_save_data_free(save);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: slot 1 should be NULL\n");
        return 1;
    }

    sekiro_save_data_free(save);
    DeleteFileW(tmp_path);
    sekiro_printf(L"PASS: sekiro-load-min-fixture\n");
    return 0;
}

/* sekiro-roundtrip-byte-stable <tmp>
 * Build a fixture, read file bytes A, load + serialize slot 0 + import_raw
 * the same bytes back, read file bytes B, assert A == B. A no-op roundtrip
 * must be byte-stable. Destructive: deletes <tmp>. */
static int handle_sekiro_roundtrip_byte_stable(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint8_t *buf_a = NULL;
    uint8_t *buf_b = NULL;
    uint8_t *ser_buf = NULL;
    sekiro_save_data_t *save = NULL;
    sekiro_char_data_t *c0 = NULL;
    bool bytes_equal = false;

    if (argc < 4) {
        sekiro_printf(L"Usage: sekiro-roundtrip-byte-stable <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_sekiro_sl2(tmp_path, SEKIRO_TEST_USERID_A)) {
        sekiro_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: open A\n");
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
        sekiro_printf(L"FAIL: read A\n");
        return 1;
    }
    CloseHandle(fh);

    if (!sekiro_save_data_load(tmp_path, &save) || !save) {
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: load\n");
        return 1;
    }
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    c0 = sekiro_char_data_ref(save, 0);
    if (!ser_buf || c0 == NULL) {
        if (ser_buf) LocalFree(ser_buf);
        sekiro_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: ref\n");
        return 1;
    }
    if (!sekiro_char_data_serialize(c0, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        sekiro_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!sekiro_char_data_import_raw(save, 0, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        sekiro_save_data_free(save);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    sekiro_save_data_free(save);

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE
        || !ReadFile(fh, buf_b, file_size, &bytes_read, NULL)
        || bytes_read != file_size) {
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        LocalFree(buf_a);
        LocalFree(buf_b);
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: read B\n");
        return 1;
    }
    CloseHandle(fh);

    bytes_equal = (RtlCompareMemory(buf_a, buf_b, file_size) == file_size);
    LocalFree(buf_a);
    LocalFree(buf_b);
    DeleteFileW(tmp_path);

    if (!bytes_equal) {
        sekiro_printf(L"FAIL: bytes differ after no-op roundtrip\n");
        return 1;
    }
    sekiro_printf(L"PASS: sekiro-roundtrip-byte-stable\n");
    return 0;
}

/* sekiro-active-slot <tmp> <expected_int>
 * Build a fixture, load it, verify the active slot equals the expected value.
 * Destructive: deletes <tmp>. */
static int handle_sekiro_active_slot(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    int expected;
    sekiro_save_data_t *save = NULL;
    int slot = -1;
    bool got;

    if (argc < 5) {
        sekiro_printf(L"Usage: sekiro-active-slot <tmp_path> <expected_int>\n");
        return 1;
    }
    tmp_path = argv[3];
    expected = _wtoi(argv[4]);

    if (!praxis_make_min_valid_sekiro_sl2(tmp_path, SEKIRO_TEST_USERID_A)) {
        sekiro_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!sekiro_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        sekiro_printf(L"FAIL: load\n");
        return 1;
    }

    got = sekiro_save_get_active_slot(save, &slot);
    sekiro_save_data_free(save);
    DeleteFileW(tmp_path);

    if (!got || slot != expected) {
        sekiro_printf(L"FAIL: active slot %d, expected %d\n", slot, expected);
        return 1;
    }
    sekiro_printf(L"PASS: sekiro-active-slot (slot=%d)\n", slot);
    return 0;
}

/* sekiro-import-resigns-userid <srcA> <dstB>
 * Build fixture A with USERID_A and fixture B with USERID_B, serialize A's
 * slot 0, import into B's slot 0, then read B's file directly and verify:
 *   - summary userid @ 0x24 still equals USERID_B (destination summary
 *     userid is never touched by import_raw)
 *   - char slot 0 userid @ (N + 0x44) equals USERID_B (import_raw re-signs
 *     the imported char's Steam ID to match the destination summary userid)
 * Destructive: deletes both <srcA> and <dstB>. */
static int handle_sekiro_import_resigns_userid(int argc, wchar_t **argv) {
    const wchar_t *path_a;
    const wchar_t *path_b;
    sekiro_save_data_t *save_a = NULL;
    sekiro_save_data_t *save_b = NULL;
    sekiro_char_data_t *c0_a = NULL;
    uint8_t *ser_buf = NULL;
    uint8_t *file_buf = NULL;
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint32_t slot0_off = 0;
    uint32_t summary_off = 0;
    uint32_t n_value = 0;
    uint64_t summary_userid = 0;
    uint64_t char_userid = 0;
    bool ok = true;

    if (argc < 5) {
        sekiro_printf(L"Usage: sekiro-import-resigns-userid <tmp_path_A> <tmp_path_B>\n");
        return 1;
    }
    path_a = argv[3];
    path_b = argv[4];

    if (!praxis_make_min_valid_sekiro_sl2(path_a, SEKIRO_TEST_USERID_A)) {
        sekiro_printf(L"FAIL: fixture A builder failed\n");
        return 1;
    }
    if (!praxis_make_min_valid_sekiro_sl2(path_b, SEKIRO_TEST_USERID_B)) {
        DeleteFileW(path_a);
        sekiro_printf(L"FAIL: fixture B builder failed\n");
        return 1;
    }

    /* Load A, serialize slot 0 */
    if (!sekiro_save_data_load(path_a, &save_a) || !save_a) {
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: load A\n");
        return 1;
    }
    c0_a = sekiro_char_data_ref(save_a, 0);
    if (c0_a == NULL) {
        sekiro_save_data_free(save_a);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: ref A slot 0\n");
        return 1;
    }
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf
        || !sekiro_char_data_serialize(c0_a, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        if (ser_buf) LocalFree(ser_buf);
        sekiro_save_data_free(save_a);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: serialize A slot 0\n");
        return 1;
    }
    sekiro_save_data_free(save_a);
    save_a = NULL;

    /* Load B, import slot 0 from A (this should re-sign to B's userid) */
    if (!sekiro_save_data_load(path_b, &save_b) || !save_b) {
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: load B\n");
        return 1;
    }
    if (!sekiro_char_data_import_raw(save_b, 0, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        sekiro_save_data_free(save_b);
        LocalFree(ser_buf);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: import_raw into B\n");
        return 1;
    }
    LocalFree(ser_buf);
    ser_buf = NULL;
    sekiro_save_data_free(save_b);
    save_b = NULL;

    /* Read B's file directly to verify the userids on disk. */
    fh = CreateFileW(path_b, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: open B for verification\n");
        return 1;
    }
    file_size = GetFileSize(fh, NULL);
    file_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, file_size);
    if (!file_buf
        || !ReadFile(fh, file_buf, file_size, &bytes_read, NULL)
        || bytes_read != file_size) {
        if (file_buf) LocalFree(file_buf);
        CloseHandle(fh);
        DeleteFileW(path_a);
        DeleteFileW(path_b);
        sekiro_printf(L"FAIL: read B for verification\n");
        return 1;
    }
    CloseHandle(fh);

    /* BND4 slot table: slot 0 offset at SEKIRO_BND4_OFFSET_ARRAY_OFFSET,
     * slot 10 offset at +10*stride. */
    CopyMemory(&slot0_off, file_buf + SEKIRO_BND4_OFFSET_ARRAY_OFFSET, sizeof(uint32_t));
    CopyMemory(&summary_off,
               file_buf + SEKIRO_BND4_OFFSET_ARRAY_OFFSET + 10u * SEKIRO_BND4_ENTRY_STRIDE,
               sizeof(uint32_t));

    /* Summary userid: skip 16-byte MD5 then read uint64 at offset 0x24. */
    CopyMemory(&summary_userid,
               file_buf + summary_off + SEKIRO_MD5_HEADER_SIZE + SEKIRO_SUMMARY_USERID_OFFSET,
               sizeof(uint64_t));
    if (summary_userid != SEKIRO_TEST_USERID_B) {
        sekiro_printf(L"FAIL: summary userid expected %llu, got %llu\n",
            (unsigned long long)SEKIRO_TEST_USERID_B,
            (unsigned long long)summary_userid);
        ok = false;
    }

    /* Char slot 0 userid: skip 16-byte MD5 then read N at 0x0C, then read
     * uint64 at N + 0x44. Bounds-check N first. */
    CopyMemory(&n_value,
               file_buf + slot0_off + SEKIRO_MD5_HEADER_SIZE + SEKIRO_CHAR_USERID_LEN_OFFSET,
               sizeof(uint32_t));
    if (n_value > SEKIRO_CHAR_PLAINTEXT_SIZE
        || SEKIRO_CHAR_USERID_DELTA > SEKIRO_CHAR_PLAINTEXT_SIZE - n_value
        || n_value + SEKIRO_CHAR_USERID_DELTA
           > SEKIRO_CHAR_PLAINTEXT_SIZE - sizeof(uint64_t)) {
        sekiro_printf(L"FAIL: invalid N value %u in char slot 0\n",
            (unsigned)n_value);
        ok = false;
    } else {
        CopyMemory(&char_userid,
                   file_buf + slot0_off + SEKIRO_MD5_HEADER_SIZE
                            + n_value + SEKIRO_CHAR_USERID_DELTA,
                   sizeof(uint64_t));
        if (char_userid != SEKIRO_TEST_USERID_B) {
            sekiro_printf(L"FAIL: char slot 0 userid expected %llu (re-signed to B), got %llu\n",
                (unsigned long long)SEKIRO_TEST_USERID_B,
                (unsigned long long)char_userid);
            ok = false;
        }
    }

    LocalFree(file_buf);
    DeleteFileW(path_a);
    DeleteFileW(path_b);

    if (!ok) {
        return 1;
    }
    sekiro_printf(L"PASS: sekiro-import-resigns-userid (summary=B, char=B re-signed)\n");
    return 0;
}

/* sekiro-real-save-load <path>
 * Load a real Sekiro save file and print active slot plus availability of all
 * 10 char slots. Read-only: sekiro_save_data_load opens with GENERIC_READ +
 * FILE_SHARE_READ internally and never modifies the file. If the file does
 * not exist, prints SKIP and exits 0 (CI-safe). */
static int handle_sekiro_real_save_load(int argc, wchar_t **argv) {
    const wchar_t *path;
    sekiro_save_data_t *save = NULL;
    int slot = -1;
    int i;

    if (argc < 4) {
        sekiro_printf(L"Usage: sekiro-real-save-load <path>\n");
        return 1;
    }
    path = argv[3];

    if (!sekiro_file_exists(path)) {
        sekiro_printf(L"SKIP: sekiro-real-save-load \u2014 real save not available at %ls\n", path);
        return 0;
    }

    if (!sekiro_save_data_load(path, &save) || !save) {
        sekiro_printf(L"FAIL: sekiro_save_data_load returned false/NULL for real save\n");
        return 1;
    }

    if (sekiro_save_get_active_slot(save, &slot)) {
        sekiro_printf(L"Active slot: %d\n", slot);
    } else {
        sekiro_printf(L"Active slot: (not available)\n");
    }

    for (i = 0; i < 10; i++) {
        sekiro_char_data_t *c = sekiro_char_data_ref(save, i);
        sekiro_printf(L"Slot %d: %ls\n", i, c ? L"available" : L"empty");
    }

    sekiro_save_data_free(save);
    sekiro_printf(L"PASS: sekiro-real-save-load\n");
    return 0;
}

/* sekiro-real-save-classify <path>
 * Open a real Sekiro save read-only (GENERIC_READ | FILE_SHARE_READ), verify
 * BND4 magic at offset 0, and that slot count is >= SEKIRO_BND4_ENTRY_COUNT
 * (12) at offset SEKIRO_BND4_SLOT_COUNT_OFFSET. Never modifies the file.
 * If the file does not exist, prints SKIP and exits 0. */
static int handle_sekiro_real_save_classify(int argc, wchar_t **argv) {
    const wchar_t *path;
    HANDLE fh;
    DWORD file_size = 0;
    uint8_t header[0x10];
    uint8_t slot_count_buf[4];
    DWORD bytes_read = 0;
    int slot_count;

    if (argc < 4) {
        sekiro_printf(L"Usage: sekiro-real-save-classify <path>\n");
        return 1;
    }
    path = argv[3];

    if (!sekiro_file_exists(path)) {
        sekiro_printf(L"SKIP: sekiro-real-save-classify \u2014 real save not available at %ls\n", path);
        return 0;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        sekiro_printf(L"FAIL: cannot open file\n");
        return 1;
    }
    file_size = GetFileSize(fh, NULL);

    if (!ReadFile(fh, header, sizeof(header), &bytes_read, NULL)
        || bytes_read != sizeof(header)) {
        CloseHandle(fh);
        sekiro_printf(L"FAIL: cannot read header\n");
        return 1;
    }
    if (header[0] != 'B' || header[1] != 'N' || header[2] != 'D' || header[3] != '4') {
        CloseHandle(fh);
        sekiro_printf(L"FAIL: not a BND4 file\n");
        return 1;
    }

    SetFilePointer(fh, SEKIRO_BND4_SLOT_COUNT_OFFSET, NULL, FILE_BEGIN);
    if (!ReadFile(fh, slot_count_buf, 4, &bytes_read, NULL) || bytes_read != 4) {
        CloseHandle(fh);
        sekiro_printf(L"FAIL: cannot read slot count\n");
        return 1;
    }
    CloseHandle(fh);
    slot_count = *(int *)slot_count_buf;

    if (slot_count < SEKIRO_BND4_ENTRY_COUNT) {
        sekiro_printf(L"FAIL: slot count %d < %d\n", slot_count, SEKIRO_BND4_ENTRY_COUNT);
        return 1;
    }

    sekiro_printf(L"File size: %u bytes\n", file_size);
    sekiro_printf(L"Slot count: %d\n", slot_count);
    sekiro_printf(L"PASS: sekiro-real-save-classify\n");
    return 0;
}

/* sekiro-real-save-roundtrip-readonly <path> <tmp_copy>
 * Verify the read-only contract on a real Sekiro save:
 *   1. Compute SHA256 of <path> (BEFORE).
 *   2. CopyFileW <path> to <tmp_copy>.
 *   3. Read summary userid from <tmp_copy> (BEFORE_IMPORT).
 *   4. Load <tmp_copy>, get active slot, find first available char slot,
 *      serialize it, import_raw the same bytes back into the same slot.
 *   5. Reload <tmp_copy>; verify active slot is unchanged.
 *   6. Read summary userid from <tmp_copy> (AFTER_IMPORT); verify unchanged.
 *   7. DeleteFileW <tmp_copy>.
 *   8. Compute SHA256 of <path> (AFTER); assert BEFORE == AFTER.
 * If the file does not exist, prints SKIP and exits 0. */
static int handle_sekiro_real_save_roundtrip_readonly(int argc, wchar_t **argv) {
    const wchar_t *path;
    const wchar_t *tmp_copy;
    uint8_t hash_before[32];
    uint8_t hash_after[32];
    sekiro_save_data_t *save = NULL;
    int active_slot = -1;
    int active_slot_after = -1;
    int first_slot = -1;
    int i;
    sekiro_char_data_t *c = NULL;
    uint8_t *ser_buf = NULL;
    uint64_t summary_userid_before = 0;
    uint64_t summary_userid_after = 0;
    uint32_t summary_off = 0;
    HANDLE fh;
    DWORD bytes_read = 0;

    if (argc < 5) {
        sekiro_printf(L"Usage: sekiro-real-save-roundtrip-readonly <path> <tmp_copy>\n");
        return 1;
    }
    path = argv[3];
    tmp_copy = argv[4];

    if (!sekiro_file_exists(path)) {
        sekiro_printf(L"SKIP: sekiro-real-save-roundtrip-readonly \u2014 real save not available at %ls\n", path);
        return 0;
    }

    if (!sekiro_sha256_file(path, hash_before)) {
        sekiro_printf(L"FAIL: sha256 BEFORE\n");
        return 1;
    }

    if (!CopyFileW(path, tmp_copy, FALSE)) {
        sekiro_printf(L"FAIL: CopyFileW to tmp_copy (error 0x%08X)\n",
            (unsigned)GetLastError());
        return 1;
    }

    /* Read summary userid from tmp_copy BEFORE import. We only need a small
     * header region plus the summary slot start; for simplicity we read just
     * enough to grab the summary slot offset and userid bytes. */
    fh = CreateFileW(tmp_copy, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: open tmp_copy for BEFORE read\n");
        return 1;
    }
    SetFilePointer(fh,
        (LONG)(SEKIRO_BND4_OFFSET_ARRAY_OFFSET + 10u * SEKIRO_BND4_ENTRY_STRIDE),
        NULL, FILE_BEGIN);
    if (!ReadFile(fh, &summary_off, sizeof(uint32_t), &bytes_read, NULL)
        || bytes_read != sizeof(uint32_t)) {
        CloseHandle(fh);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: read summary slot offset BEFORE\n");
        return 1;
    }
    SetFilePointer(fh,
        (LONG)(summary_off + SEKIRO_MD5_HEADER_SIZE + SEKIRO_SUMMARY_USERID_OFFSET),
        NULL, FILE_BEGIN);
    if (!ReadFile(fh, &summary_userid_before, sizeof(uint64_t), &bytes_read, NULL)
        || bytes_read != sizeof(uint64_t)) {
        CloseHandle(fh);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: read summary userid BEFORE\n");
        return 1;
    }
    CloseHandle(fh);

    if (!sekiro_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: load tmp_copy\n");
        return 1;
    }
    sekiro_save_get_active_slot(save, &active_slot);

    for (i = 0; i < 10; i++) {
        if (sekiro_char_data_ref(save, i) != NULL) {
            first_slot = i;
            break;
        }
    }

    if (first_slot < 0) {
        sekiro_save_data_free(save);
        DeleteFileW(tmp_copy);
        if (!sekiro_sha256_file(path, hash_after)) {
            sekiro_printf(L"FAIL: sha256 AFTER (no slots case)\n");
            return 1;
        }
        if (RtlCompareMemory(hash_before, hash_after, 32) != 32) {
            sekiro_printf(L"FAIL: <path> hash changed during no-slots run\n");
            return 1;
        }
        sekiro_printf(L"SKIP: no available char slots in save\n");
        return 0;
    }

    c = sekiro_char_data_ref(save, first_slot);
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf) {
        sekiro_save_data_free(save);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: alloc ser_buf\n");
        return 1;
    }
    if (!sekiro_char_data_serialize(c, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        sekiro_save_data_free(save);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!sekiro_char_data_import_raw(save, first_slot, ser_buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        sekiro_save_data_free(save);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    sekiro_save_data_free(save);
    save = NULL;

    /* Reload tmp_copy and verify active slot unchanged. */
    if (!sekiro_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: reload after roundtrip\n");
        return 1;
    }
    sekiro_save_get_active_slot(save, &active_slot_after);
    sekiro_save_data_free(save);

    /* Read summary userid from tmp_copy AFTER import. */
    fh = CreateFileW(tmp_copy, GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: open tmp_copy for AFTER read\n");
        return 1;
    }
    SetFilePointer(fh,
        (LONG)(summary_off + SEKIRO_MD5_HEADER_SIZE + SEKIRO_SUMMARY_USERID_OFFSET),
        NULL, FILE_BEGIN);
    if (!ReadFile(fh, &summary_userid_after, sizeof(uint64_t), &bytes_read, NULL)
        || bytes_read != sizeof(uint64_t)) {
        CloseHandle(fh);
        DeleteFileW(tmp_copy);
        sekiro_printf(L"FAIL: read summary userid AFTER\n");
        return 1;
    }
    CloseHandle(fh);
    DeleteFileW(tmp_copy);

    if (active_slot != active_slot_after) {
        sekiro_printf(L"FAIL: active slot changed from %d to %d\n",
            active_slot, active_slot_after);
        return 1;
    }
    if (summary_userid_before != summary_userid_after) {
        sekiro_printf(L"FAIL: summary userid changed from %llu to %llu\n",
            (unsigned long long)summary_userid_before,
            (unsigned long long)summary_userid_after);
        return 1;
    }

    if (!sekiro_sha256_file(path, hash_after)) {
        sekiro_printf(L"FAIL: sha256 AFTER\n");
        return 1;
    }
    if (RtlCompareMemory(hash_before, hash_after, 32) != 32) {
        int j;
        sekiro_printf(L"FAIL: <path> hash changed during read-only roundtrip\n");
        sekiro_printf(L"  before: ");
        for (j = 0; j < 32; j++) sekiro_printf(L"%02X", hash_before[j]);
        sekiro_printf(L"\n  after:  ");
        for (j = 0; j < 32; j++) sekiro_printf(L"%02X", hash_after[j]);
        sekiro_printf(L"\n");
        return 1;
    }

    sekiro_printf(L"PASS: sekiro-real-save-roundtrip-readonly (slot %d, active=%d, source hash unchanged)\n",
        first_slot, active_slot);
    return 0;
}

/* ==========================================================================
 * Dispatcher
 * ========================================================================== */

int praxis_selftest_sekiro_dispatch(int argc, wchar_t **argv, const wchar_t *sub) {
    if (wcscmp(sub, L"sekiro-null-guards") == 0) return handle_sekiro_null_guards(argc, argv);
    if (wcscmp(sub, L"sekiro-load-min-fixture") == 0) return handle_sekiro_load_min_fixture(argc, argv);
    if (wcscmp(sub, L"sekiro-roundtrip-byte-stable") == 0) return handle_sekiro_roundtrip_byte_stable(argc, argv);
    if (wcscmp(sub, L"sekiro-active-slot") == 0) return handle_sekiro_active_slot(argc, argv);
    if (wcscmp(sub, L"sekiro-import-resigns-userid") == 0) return handle_sekiro_import_resigns_userid(argc, argv);
    if (wcscmp(sub, L"sekiro-real-save-load") == 0) return handle_sekiro_real_save_load(argc, argv);
    if (wcscmp(sub, L"sekiro-real-save-classify") == 0) return handle_sekiro_real_save_classify(argc, argv);
    if (wcscmp(sub, L"sekiro-real-save-roundtrip-readonly") == 0) return handle_sekiro_real_save_roundtrip_readonly(argc, argv);
    return -1;
}
