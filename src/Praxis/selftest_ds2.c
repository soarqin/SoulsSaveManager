/**
 * @file selftest_ds2.c
 * @brief DS2S selftest subcommand dispatcher and helpers.
 * @details Houses the headless `--selftest ds2s-*` dispatcher plus helper
 *          routines (fixture builder, summary decryptor, SHA256) used only
 *          by DS2S selftest subcommands. Kept separate from praxis_selftest.c
 *          to limit churn on the main dispatcher file.
 *
 *          Twelve subcommands are implemented:
 *            1. ds2s-aes-known-vector
 *            2. ds2s-null-guards
 *            3. ds2s-load-min-fixture
 *            4. ds2s-roundtrip-byte-stable
 *            5. ds2s-active-slot
 *            6. ds2s-dual-slot-roundtrip
 *            7. ds2s-import-resigns-userid-text
 *            8. ds2s-available-slots-by-profile-byte
 *            9. ds2s-bnd4-entry-count
 *           10. ds2s-real-save-load
 *           11. ds2s-real-save-classify
 *           12. ds2s-real-save-roundtrip-readonly
 *
 *          Subcommands marked DESTRUCTIVE create and then delete the
 *          supplied tmp path; never pass a real save path as <tmp>.
 */

#ifdef PRAXIS_ENABLE_SELFTEST

#include "selftest_ds2.h"

#include "ds2_test_format.h"

#include "../common/ds2save.h"

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
#include <shlwapi.h>

/* --- Output helper ------------------------------------------------------- */

/* Formatted wide-char printf honoring stdout redirect set by the parent process.
 * Duplicated from praxis_selftest.c to keep selftest_ds2.c self-contained;
 * praxis_selftest.c's st_printf is static and therefore not externally visible. */
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

/* --- BCrypt setup helpers ------------------------------------------------ */

/* Open AES-128-CBC with the DS2S master key. Caller must free via ds2_st_aes_close. */
static bool ds2_st_aes_open(BCRYPT_ALG_HANDLE *out_alg, BCRYPT_KEY_HANDLE *out_key,
                            uint8_t **out_key_obj) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    ULONG result_size = 0;
    NTSTATUS status;

    *out_alg = NULL;
    *out_key = NULL;
    *out_key_obj = NULL;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        return false;
    }
    status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&key_obj_size,
                                sizeof(ULONG), &result_size, 0);
    if (!NT_SUCCESS(status) || key_obj_size == 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    key_obj = (uint8_t *)LocalAlloc(LMEM_FIXED, key_obj_size);
    if (!key_obj) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size,
                                        (PUCHAR)DS2_AES_KEY_BYTES, 16, 0);
    if (!NT_SUCCESS(status)) {
        LocalFree(key_obj);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    *out_alg = alg;
    *out_key = key;
    *out_key_obj = key_obj;
    return true;
}

static void ds2_st_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key, uint8_t *key_obj) {
    if (key) BCryptDestroyKey(key);
    if (key_obj) LocalFree(key_obj);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
}

/* --- Fixture builder ----------------------------------------------------- */

/* Slot layout for the DS2S fixture:
 *   Entry 0       : summary slot (DS2_SUMMARY_SLOT_ON_DISK_SIZE)
 *   Entries 1..10 : char part A slots (DS2_CHAR_A_SLOT_ON_DISK_SIZE each)
 *   Entries 11..20: char part B slots (DS2_CHAR_B_SLOT_ON_DISK_SIZE each)
 *   Entries 21..22: trailing extras (16 bytes each, not validated by ds2save.c)
 *
 * Encryption mode: AES-128-CBC with no padding (flags=0), matching production
 * ds2save.c behavior. All entry plaintext sizes are multiples of 16, so no
 * padding is needed; ciphertext == plaintext size. Using BCRYPT_BLOCK_PADDING
 * here would overflow the slot by one block.
 *
 * MD5: each entry's first 16 bytes are MD5(IV || ciphertext). ds2save.c does
 * not validate this on load, but we set it to keep the file structurally
 * faithful to real DS2S saves.
 *
 * Char slot 0 plaintext markers (used by ds2s-dual-slot-roundtrip):
 *   - part A (entry 1) : bytes are 0xAA
 *   - part B (entry 11): bytes are 0xBB
 */
#define DS2_EXTRA_ENTRY_SIZE 16u
#define DS2_TEST_CHAR_A_SERIALIZED_SIZE DS2_CHAR_A_PLAINTEXT_SIZE
#define DS2_TEST_CHAR_B_SERIALIZED_SIZE DS2_CHAR_B_PLAINTEXT_SIZE
#define DS2_TEST_SUMMARY_USERID_TEXT_OFFSET DS2_SUMMARY_USERID_TEXT_OFFSET
#define DS2_TEST_SUMMARY_ACTIVE_OFFSET DS2_SUMMARY_ACTIVE_OFFSET
#define DS2_TEST_SUMMARY_PROFILE_OFFSET DS2_SUMMARY_PROFILE_OFFSET

static bool praxis_make_min_valid_ds2_sl2(const wchar_t *path, const char *userid_hex16) {
    const uint32_t header_size = DS2_BND4_FILE_HEADER_SIZE;
    const uint32_t summary_size = DS2_SUMMARY_SLOT_ON_DISK_SIZE;
    const uint32_t char_a_size = DS2_CHAR_A_SLOT_ON_DISK_SIZE;
    const uint32_t char_b_size = DS2_CHAR_B_SLOT_ON_DISK_SIZE;
    const uint32_t extra_size = DS2_EXTRA_ENTRY_SIZE;
    const uint32_t summary_pt_size = DS2_SUMMARY_PLAINTEXT_SIZE;
    const uint32_t char_a_pt_size = DS2_CHAR_A_PLAINTEXT_SIZE;
    const uint32_t char_b_pt_size = DS2_CHAR_B_PLAINTEXT_SIZE;
    const uint32_t summary_offset = header_size;
    const uint32_t char_a_base_offset = summary_offset + summary_size;
    const uint32_t char_b_base_offset = char_a_base_offset + 10u * char_a_size;
    const uint32_t extra_base_offset = char_b_base_offset + 10u * char_b_size;
    const uint32_t total_size = extra_base_offset + 2u * extra_size;

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    uint8_t *file_data = NULL;
    uint8_t *plaintext = NULL;
    uint8_t *md5_buf = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    bool ok = true;
    int i;

    if (!path || !userid_hex16) {
        return false;
    }

    file_data = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, total_size);
    if (!file_data) {
        return false;
    }

    /* BND4 magic + entry count */
    CopyMemory(file_data, "BND4", 4);
    *(uint32_t *)(file_data + DS2_BND4_SLOT_COUNT_OFFSET) = DS2_BND4_TOTAL_ENTRY_COUNT;

    /* BND4 entry table: 23 entries (size, offset) */
    /* Entry 0: summary */
    *(uint32_t *)(file_data + DS2_BND4_SIZE_ARRAY_OFFSET + 0u * DS2_BND4_ENTRY_STRIDE) = summary_size;
    *(uint32_t *)(file_data + DS2_BND4_OFFSET_ARRAY_OFFSET + 0u * DS2_BND4_ENTRY_STRIDE) = summary_offset;
    /* Entries 1..10: char part A */
    for (i = 0; i < 10; i++) {
        uint32_t idx = 1u + (uint32_t)i;
        *(uint32_t *)(file_data + DS2_BND4_SIZE_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) = char_a_size;
        *(uint32_t *)(file_data + DS2_BND4_OFFSET_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) =
            char_a_base_offset + (uint32_t)i * char_a_size;
    }
    /* Entries 11..20: char part B */
    for (i = 0; i < 10; i++) {
        uint32_t idx = 11u + (uint32_t)i;
        *(uint32_t *)(file_data + DS2_BND4_SIZE_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) = char_b_size;
        *(uint32_t *)(file_data + DS2_BND4_OFFSET_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) =
            char_b_base_offset + (uint32_t)i * char_b_size;
    }
    /* Entries 21..22: trailing extras */
    for (i = 0; i < 2; i++) {
        uint32_t idx = 21u + (uint32_t)i;
        *(uint32_t *)(file_data + DS2_BND4_SIZE_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) = extra_size;
        *(uint32_t *)(file_data + DS2_BND4_OFFSET_ARRAY_OFFSET + idx * DS2_BND4_ENTRY_STRIDE) =
            extra_base_offset + (uint32_t)i * extra_size;
    }

    /* BCrypt setup */
    if (!ds2_st_aes_open(&alg, &key, &key_obj)) {
        LocalFree(file_data);
        return false;
    }

    /* Encrypt the 21 meaningful entries (0..20). Entries 21..22 stay zero. */
    for (i = 0; i < 21 && ok; i++) {
        uint32_t slot_offset;
        uint32_t pt_size;
        uint8_t *ct_buf;
        uint8_t iv_scratch[16];
        ULONG ct_written = 0;
        NTSTATUS status;

        if (i == 0) {
            slot_offset = summary_offset;
            pt_size = summary_pt_size;
        } else if (i < 11) {
            slot_offset = char_a_base_offset + (uint32_t)(i - 1) * char_a_size;
            pt_size = char_a_pt_size;
        } else {
            slot_offset = char_b_base_offset + (uint32_t)(i - 11) * char_b_size;
            pt_size = char_b_pt_size;
        }

        plaintext = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, pt_size);
        if (!plaintext) {
            ok = false;
            break;
        }

        /* Populate slot-specific fields in plaintext */
        if (i == 0) {
            /* Summary slot: Steam ID text @ 0x39, active=0 @ 0x36C, profile[0] flag=1 */
            CopyMemory(plaintext + DS2_TEST_SUMMARY_USERID_TEXT_OFFSET, userid_hex16,
                       DS2_USERID_TEXT_LENGTH);
            *(int32_t *)(plaintext + DS2_TEST_SUMMARY_ACTIVE_OFFSET) = 0;
            *(int32_t *)(plaintext + DS2_TEST_SUMMARY_PROFILE_OFFSET
                          + DS2_PROFILE_AVAILABLE_FLAG_OFFSET) = 1;
        } else if (i == 1) {
            /* Char slot 0 part A: distinguishable marker for dual-slot test */
            FillMemory(plaintext, char_a_pt_size, 0xAA);
        } else if (i == 11) {
            /* Char slot 0 part B: distinguishable marker for dual-slot test */
            FillMemory(plaintext, char_b_pt_size, 0xBB);
        }

        /* Write IV into the file buffer right after the 16-byte MD5 header */
        CopyMemory(file_data + slot_offset + 16u, DS2_TEST_IV, 16);

        /* Encrypt plaintext -> file_data[slot_offset+32 .. slot_offset+32+pt_size).
         * No padding (flags=0); plaintext size is always a multiple of 16. */
        ct_buf = file_data + slot_offset + 32u;
        CopyMemory(iv_scratch, DS2_TEST_IV, 16);
        status = BCryptEncrypt(key, (PUCHAR)plaintext, pt_size, NULL, iv_scratch, 16,
                                ct_buf, pt_size, &ct_written, 0);
        if (!NT_SUCCESS(status) || ct_written != pt_size) {
            ok = false;
            LocalFree(plaintext);
            plaintext = NULL;
            break;
        }

        /* MD5(IV || ciphertext) into the slot's 16-byte MD5 header */
        md5_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, 16u + pt_size);
        if (!md5_buf) {
            ok = false;
            LocalFree(plaintext);
            plaintext = NULL;
            break;
        }
        CopyMemory(md5_buf, DS2_TEST_IV, 16);
        CopyMemory(md5_buf + 16, ct_buf, pt_size);
        md5_buffer(md5_buf, 16u + pt_size, file_data + slot_offset);
        LocalFree(md5_buf);
        md5_buf = NULL;

        LocalFree(plaintext);
        plaintext = NULL;
    }

    ds2_st_aes_close(alg, key, key_obj);

    if (!ok) {
        LocalFree(file_data);
        return false;
    }

    /* Write file */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(file_data);
        return false;
    }
    ok = WriteFile(file, file_data, total_size, &written, NULL) && written == total_size;
    CloseHandle(file);
    LocalFree(file_data);
    return ok;
}

/* --- Summary decrypt helper (for verification) --------------------------- */

/* Read the summary slot from a fixture file and decrypt it into out_pt.
 * Assumes the fixture layout: summary slot at offset DS2_BND4_FILE_HEADER_SIZE,
 * on-disk size DS2_SUMMARY_SLOT_ON_DISK_SIZE. */
static bool ds2_st_decrypt_summary_from_file(const wchar_t *path, uint8_t *out_pt) {
    HANDLE fh;
    DWORD bytes_read = 0;
    uint8_t iv[16];
    uint8_t *ct = NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG pt_len = 0;
    uint8_t iv_copy[16];
    NTSTATUS status;
    bool ok;

    if (!path || !out_pt) {
        return false;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }

    /* Skip MD5 (16 bytes), then read IV (16 bytes) at slot_offset + 16 */
    if (SetFilePointer(fh, DS2_BND4_FILE_HEADER_SIZE + 16, NULL, FILE_BEGIN)
            != (DWORD)(DS2_BND4_FILE_HEADER_SIZE + 16)
        || !ReadFile(fh, iv, sizeof(iv), &bytes_read, NULL)
        || bytes_read != sizeof(iv)) {
        CloseHandle(fh);
        return false;
    }

    ct = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_SUMMARY_PLAINTEXT_SIZE);
    if (!ct) {
        CloseHandle(fh);
        return false;
    }

    if (!ReadFile(fh, ct, DS2_SUMMARY_PLAINTEXT_SIZE, &bytes_read, NULL)
        || bytes_read != DS2_SUMMARY_PLAINTEXT_SIZE) {
        LocalFree(ct);
        CloseHandle(fh);
        return false;
    }
    CloseHandle(fh);

    if (!ds2_st_aes_open(&alg, &key, &key_obj)) {
        LocalFree(ct);
        return false;
    }

    CopyMemory(iv_copy, iv, 16);
    status = BCryptDecrypt(key, ct, DS2_SUMMARY_PLAINTEXT_SIZE, NULL, iv_copy, 16,
                           out_pt, DS2_SUMMARY_PLAINTEXT_SIZE, &pt_len, 0);
    ok = NT_SUCCESS(status) && pt_len == DS2_SUMMARY_PLAINTEXT_SIZE;

    ds2_st_aes_close(alg, key, key_obj);
    LocalFree(ct);
    return ok;
}

/* Mutate the summary plaintext profile flag for `slot` in the on-disk file,
 * then re-encrypt the summary slot and recompute its MD5. Used by
 * ds2s-available-slots-by-profile-byte. */
static bool ds2_st_set_summary_profile_flag(const wchar_t *path, int slot, int32_t flag_value) {
    HANDLE fh = INVALID_HANDLE_VALUE;
    DWORD io_done = 0;
    uint8_t iv[16];
    uint8_t iv_copy[16];
    uint8_t *pt = NULL;
    uint8_t *ct = NULL;
    uint8_t *md5_input = NULL;
    uint8_t md5_out[16];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    NTSTATUS status;
    ULONG io_len = 0;
    bool ok;
    uint32_t flag_offset;

    if (!path || slot < 0 || slot >= 10) {
        return false;
    }

    pt = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_SUMMARY_PLAINTEXT_SIZE);
    ct = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_SUMMARY_PLAINTEXT_SIZE);
    md5_input = (uint8_t *)LocalAlloc(LMEM_FIXED, 16u + DS2_SUMMARY_PLAINTEXT_SIZE);
    if (!pt || !ct || !md5_input) {
        if (pt) LocalFree(pt);
        if (ct) LocalFree(ct);
        if (md5_input) LocalFree(md5_input);
        return false;
    }

    if (!ds2_st_decrypt_summary_from_file(path, pt)) {
        LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
        return false;
    }

    flag_offset = DS2_TEST_SUMMARY_PROFILE_OFFSET
                  + (uint32_t)slot * DS2_PROFILE_SIZE
                  + DS2_PROFILE_AVAILABLE_FLAG_OFFSET;
    *(int32_t *)(pt + flag_offset) = flag_value;

    /* Re-encrypt summary slot in place */
    if (!ds2_st_aes_open(&alg, &key, &key_obj)) {
        LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
        return false;
    }

    fh = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        ds2_st_aes_close(alg, key, key_obj);
        LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
        return false;
    }

    /* Read IV from file (preserves existing IV) */
    if (SetFilePointer(fh, DS2_BND4_FILE_HEADER_SIZE + 16, NULL, FILE_BEGIN)
            != (DWORD)(DS2_BND4_FILE_HEADER_SIZE + 16)
        || !ReadFile(fh, iv, sizeof(iv), &io_done, NULL)
        || io_done != sizeof(iv)) {
        CloseHandle(fh);
        ds2_st_aes_close(alg, key, key_obj);
        LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
        return false;
    }

    CopyMemory(iv_copy, iv, 16);
    status = BCryptEncrypt(key, pt, DS2_SUMMARY_PLAINTEXT_SIZE, NULL, iv_copy, 16,
                            ct, DS2_SUMMARY_PLAINTEXT_SIZE, &io_len, 0);
    if (!NT_SUCCESS(status) || io_len != DS2_SUMMARY_PLAINTEXT_SIZE) {
        CloseHandle(fh);
        ds2_st_aes_close(alg, key, key_obj);
        LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
        return false;
    }

    /* Compute MD5(IV || CT) */
    CopyMemory(md5_input, iv, 16);
    CopyMemory(md5_input + 16, ct, DS2_SUMMARY_PLAINTEXT_SIZE);
    md5_buffer(md5_input, 16u + DS2_SUMMARY_PLAINTEXT_SIZE, md5_out);

    /* Write back: MD5 at slot_offset+0, leave IV unchanged, CT at slot_offset+32 */
    ok = SetFilePointer(fh, DS2_BND4_FILE_HEADER_SIZE, NULL, FILE_BEGIN)
            == (DWORD)DS2_BND4_FILE_HEADER_SIZE
         && WriteFile(fh, md5_out, sizeof(md5_out), &io_done, NULL)
         && io_done == sizeof(md5_out)
         && SetFilePointer(fh, DS2_BND4_FILE_HEADER_SIZE + 32, NULL, FILE_BEGIN)
            == (DWORD)(DS2_BND4_FILE_HEADER_SIZE + 32)
         && WriteFile(fh, ct, DS2_SUMMARY_PLAINTEXT_SIZE, &io_done, NULL)
         && io_done == DS2_SUMMARY_PLAINTEXT_SIZE;

    CloseHandle(fh);
    ds2_st_aes_close(alg, key, key_obj);
    LocalFree(pt); LocalFree(ct); LocalFree(md5_input);
    return ok;
}

/* --- SHA256 file helper (for real-save-roundtrip-readonly) -------------- */

static bool ds2_st_sha256_file(const wchar_t *path, uint8_t out_hash[32]) {
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint8_t *buf = NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS status;
    bool ok = false;

    if (!path || !out_hash) {
        return false;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    file_size = GetFileSize(fh, NULL);
    if (file_size == INVALID_FILE_SIZE) {
        CloseHandle(fh);
        return false;
    }
    buf = (uint8_t *)LocalAlloc(LMEM_FIXED, file_size > 0 ? file_size : 1);
    if (!buf) {
        CloseHandle(fh);
        return false;
    }
    if (file_size > 0
        && (!ReadFile(fh, buf, file_size, &bytes_read, NULL) || bytes_read != file_size)) {
        LocalFree(buf);
        CloseHandle(fh);
        return false;
    }
    CloseHandle(fh);

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        LocalFree(buf);
        return false;
    }
    status = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        LocalFree(buf);
        return false;
    }
    if (file_size > 0) {
        status = BCryptHashData(hash, buf, file_size, 0);
        if (!NT_SUCCESS(status)) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            LocalFree(buf);
            return false;
        }
    }
    status = BCryptFinishHash(hash, out_hash, 32, 0);
    ok = NT_SUCCESS(status);

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    LocalFree(buf);
    return ok;
}

/* --- Subcommand implementations ----------------------------------------- */

/* 1. ds2s-aes-known-vector
 * Encrypts DS2_TEST_KNOWN_PLAINTEXT (16 bytes) with DS2_AES_KEY_BYTES and
 * DS2_TEST_IV using AES-128-CBC PKCS7, asserts byte-exact equality with
 * DS2_TEST_KNOWN_CIPHERTEXT (32 bytes), then decrypts back and asserts
 * equality with the original plaintext. Validates BCrypt wiring independent
 * of the fixture builder. */
static int cmd_ds2s_aes_known_vector(void) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    uint8_t ct_buf[64];
    uint8_t pt_buf[64];
    uint8_t iv_copy[16];
    ULONG ct_len = 0;
    ULONG pt_len = 0;
    NTSTATUS status;
    bool ok = true;

    if (!ds2_st_aes_open(&alg, &key, &key_obj)) {
        st_printf(L"FAIL: ds2_st_aes_open\n");
        return 1;
    }

    CopyMemory(iv_copy, DS2_TEST_IV, 16);
    status = BCryptEncrypt(key, (PUCHAR)DS2_TEST_KNOWN_PLAINTEXT, 16, NULL, iv_copy, 16,
                            ct_buf, sizeof(ct_buf), &ct_len, BCRYPT_BLOCK_PADDING);
    if (!NT_SUCCESS(status)) {
        st_printf(L"FAIL: BCryptEncrypt\n");
        ok = false;
    }
    if (ok && (ct_len != DS2_TEST_KNOWN_CIPHERTEXT_SIZE
               || RtlCompareMemory(ct_buf, DS2_TEST_KNOWN_CIPHERTEXT,
                                   DS2_TEST_KNOWN_CIPHERTEXT_SIZE)
                  != DS2_TEST_KNOWN_CIPHERTEXT_SIZE)) {
        st_printf(L"FAIL: ciphertext mismatch\n");
        ok = false;
    }

    if (ok) {
        CopyMemory(iv_copy, DS2_TEST_IV, 16);
        status = BCryptDecrypt(key, ct_buf, ct_len, NULL, iv_copy, 16, pt_buf,
                                sizeof(pt_buf), &pt_len, BCRYPT_BLOCK_PADDING);
        if (!NT_SUCCESS(status) || pt_len != 16
            || RtlCompareMemory(pt_buf, DS2_TEST_KNOWN_PLAINTEXT, 16) != 16) {
            st_printf(L"FAIL: decrypt mismatch\n");
            ok = false;
        }
    }

    ds2_st_aes_close(alg, key, key_obj);

    if (ok) {
        st_printf(L"PASS: ds2s-aes-known-vector\n");
        return 0;
    }
    return 1;
}

/* 2. ds2s-null-guards
 * Verify each of the 6 public DS2 functions rejects NULL / out-of-range
 * inputs cleanly without crashing or returning success. */
static int cmd_ds2s_null_guards(void) {
    bool ok = true;
    int slot_out = -1;
    uint8_t *dummy_buf = NULL;
    wchar_t tmp_path[MAX_PATH];
    ds2_save_data_t *save = NULL;
    ds2_char_data_t *c0 = NULL;
    ds2_save_data_t *probe = NULL;

    dummy_buf = (uint8_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, DS2_CHAR_DATA_SERIALIZED_SIZE);
    if (!dummy_buf) {
        st_printf(L"FAIL: dummy_buf alloc\n");
        return 1;
    }

    /* ds2_save_data_load(NULL, &out) returns false */
    if (ds2_save_data_load(NULL, &probe)) {
        st_printf(L"FAIL: load(NULL, &out) should return false\n");
        ok = false;
    }
    /* ds2_save_data_load(path, NULL) returns false */
    if (ds2_save_data_load(L"x", NULL)) {
        st_printf(L"FAIL: load(path, NULL) should return false\n");
        ok = false;
    }
    /* ds2_save_data_free(NULL) does not crash */
    ds2_save_data_free(NULL);
    /* ds2_save_get_active_slot(NULL, &slot) returns false */
    if (ds2_save_get_active_slot(NULL, &slot_out)) {
        st_printf(L"FAIL: get_active_slot(NULL, ...) should return false\n");
        ok = false;
    }

    /* Build a fixture for the rest of the checks */
    tmp_path[0] = L'\0';
    GetTempPathW(MAX_PATH, tmp_path);
    PathAppendW(tmp_path, L"ds2s-null-guards-test.sl2");

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        LocalFree(dummy_buf);
        return 1;
    }
    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        LocalFree(dummy_buf);
        st_printf(L"FAIL: load fixture\n");
        return 1;
    }

    /* ds2_save_get_active_slot(valid, NULL) returns false */
    if (ds2_save_get_active_slot(save, NULL)) {
        st_printf(L"FAIL: get_active_slot(..., NULL) should return false\n");
        ok = false;
    }
    /* ds2_char_data_ref(NULL, 0) returns NULL */
    if (ds2_char_data_ref(NULL, 0) != NULL) {
        st_printf(L"FAIL: ref(NULL, 0) should return NULL\n");
        ok = false;
    }
    /* ds2_char_data_ref(valid, -1) returns NULL */
    if (ds2_char_data_ref(save, -1) != NULL) {
        st_printf(L"FAIL: ref(save, -1) should return NULL\n");
        ok = false;
    }
    /* ds2_char_data_ref(valid, 10) returns NULL */
    if (ds2_char_data_ref(save, 10) != NULL) {
        st_printf(L"FAIL: ref(save, 10) should return NULL\n");
        ok = false;
    }

    c0 = ds2_char_data_ref(save, 0);

    /* ds2_char_data_serialize(NULL, buf, size) returns false */
    if (ds2_char_data_serialize(NULL, dummy_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: serialize(NULL, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_serialize(c0, NULL, size) returns false */
    if (c0 && ds2_char_data_serialize(c0, NULL, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: serialize(..., NULL, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_serialize(c0, buf, 0) returns false */
    if (c0 && ds2_char_data_serialize(c0, dummy_buf, 0)) {
        st_printf(L"FAIL: serialize(..., ..., 0) should return false\n");
        ok = false;
    }
    /* ds2_char_data_import_raw(NULL, 0, buf, size) returns false */
    if (ds2_char_data_import_raw(NULL, 0, dummy_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: import_raw(NULL, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_import_raw(save, 0, NULL, size) returns false */
    if (ds2_char_data_import_raw(save, 0, NULL, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: import_raw(..., NULL, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_import_raw(save, -1, buf, size) returns false */
    if (ds2_char_data_import_raw(save, -1, dummy_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: import_raw(..., -1, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_import_raw(save, 10, buf, size) returns false */
    if (ds2_char_data_import_raw(save, 10, dummy_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        st_printf(L"FAIL: import_raw(..., 10, ...) should return false\n");
        ok = false;
    }
    /* ds2_char_data_import_raw(save, 0, buf, 0) returns false */
    if (ds2_char_data_import_raw(save, 0, dummy_buf, 0)) {
        st_printf(L"FAIL: import_raw(..., ..., 0) should return false\n");
        ok = false;
    }

    ds2_save_data_free(save);
    DeleteFileW(tmp_path);
    LocalFree(dummy_buf);

    if (!ok) {
        return 1;
    }
    st_printf(L"PASS: ds2s-null-guards\n");
    return 0;
}

/* 3. ds2s-load-min-fixture <tmp>
 * Build a minimal valid DS2S fixture, load it, verify active slot is 0,
 * verify slot 0 ref is non-NULL, and verify slot 1 ref is NULL.
 * DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_load_min_fixture(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    ds2_save_data_t *save = NULL;
    int slot = -1;
    ds2_char_data_t *c0;
    ds2_char_data_t *c1;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-load-min-fixture <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: ds2_save_data_load returned false\n");
        return 1;
    }
    if (!ds2_save_get_active_slot(save, &slot) || slot != 0) {
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: active slot expected 0, got %d\n", slot);
        return 1;
    }
    if ((c0 = ds2_char_data_ref(save, 0)) == NULL) {
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: slot 0 should be non-NULL\n");
        return 1;
    }
    if ((c1 = ds2_char_data_ref(save, 1)) != NULL) {
        (void)c1;
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: slot 1 should be NULL\n");
        return 1;
    }

    (void)c0;
    ds2_save_data_free(save);
    DeleteFileW(tmp_path);
    st_printf(L"PASS: ds2s-load-min-fixture\n");
    return 0;
}

/* 4. ds2s-roundtrip-byte-stable <tmp>
 * Build a fixture, read raw bytes A, load + serialize slot 0 + import_raw
 * the same bytes back, read raw bytes B, assert A == B.
 * A no-op roundtrip must be byte-stable. DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_roundtrip_byte_stable(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint8_t *buf_a = NULL;
    uint8_t *buf_b = NULL;
    uint8_t *ser_buf = NULL;
    ds2_save_data_t *save = NULL;
    ds2_char_data_t *c0 = NULL;
    bool ok = false;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-roundtrip-byte-stable <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: open A\n");
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
        st_printf(L"FAIL: read A\n");
        return 1;
    }
    CloseHandle(fh);

    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: load\n");
        return 1;
    }
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf) {
        ds2_save_data_free(save);
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: ser_buf alloc\n");
        return 1;
    }
    c0 = ds2_char_data_ref(save, 0);
    if (!c0) {
        LocalFree(ser_buf);
        ds2_save_data_free(save);
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: ref\n");
        return 1;
    }
    if (!ds2_char_data_serialize(c0, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        ds2_save_data_free(save);
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!ds2_char_data_import_raw(save, 0, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        ds2_save_data_free(save);
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    ds2_save_data_free(save);

    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE
        || !ReadFile(fh, buf_b, file_size, &bytes_read, NULL)
        || bytes_read != file_size) {
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        LocalFree(buf_a); LocalFree(buf_b);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: read B\n");
        return 1;
    }
    CloseHandle(fh);

    ok = (RtlCompareMemory(buf_a, buf_b, file_size) == file_size);
    LocalFree(buf_a); LocalFree(buf_b);
    DeleteFileW(tmp_path);

    if (!ok) {
        st_printf(L"FAIL: bytes differ after no-op roundtrip\n");
        return 1;
    }
    st_printf(L"PASS: ds2s-roundtrip-byte-stable\n");
    return 0;
}

/* 5. ds2s-active-slot <tmp> <expected_int>
 * Build a fixture, load it, verify the active slot equals <expected_int>.
 * The fixture always has active=0, so caller should pass expected=0.
 * DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_active_slot(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    int expected;
    ds2_save_data_t *save = NULL;
    int slot = -1;
    bool got_ok;

    if (argc < 5) {
        st_printf(L"Usage: ds2s-active-slot <tmp_path> <expected_int>\n");
        return 1;
    }
    tmp_path = argv[3];
    expected = _wtoi(argv[4]);

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: load\n");
        return 1;
    }
    got_ok = ds2_save_get_active_slot(save, &slot);
    ds2_save_data_free(save);
    DeleteFileW(tmp_path);
    if (!got_ok || slot != expected) {
        st_printf(L"FAIL: active slot %d, expected %d\n", slot, expected);
        return 1;
    }
    st_printf(L"PASS: ds2s-active-slot (slot=%d)\n", slot);
    return 0;
}

/* 6. ds2s-dual-slot-roundtrip <tmp>
 * Build fixture, load, serialize slot 0. Assert the serialized buffer:
 *   - has size DS2_CHAR_DATA_SERIALIZED_SIZE
 *   - first DS2_TEST_CHAR_A_SERIALIZED_SIZE bytes are all 0xAA (part A marker)
 *   - next DS2_TEST_CHAR_B_SERIALIZED_SIZE bytes are all 0xBB (part B marker)
 * Verifies that the dual-slot abstraction round-trips the full part A and
 * part B plaintext blobs into the serialized buffer.
 * DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_dual_slot_roundtrip(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    ds2_save_data_t *save = NULL;
    ds2_char_data_t *c0 = NULL;
    uint8_t *ser_buf = NULL;
    uint8_t *expected_a = NULL;
    uint8_t *expected_b = NULL;
    bool ok;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-dual-slot-roundtrip <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: load\n");
        return 1;
    }
    c0 = ds2_char_data_ref(save, 0);
    if (!c0) {
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: ref slot 0\n");
        return 1;
    }

    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_CHAR_DATA_SERIALIZED_SIZE);
    expected_a = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_TEST_CHAR_A_SERIALIZED_SIZE);
    expected_b = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_TEST_CHAR_B_SERIALIZED_SIZE);
    if (!ser_buf || !expected_a || !expected_b) {
        if (ser_buf) LocalFree(ser_buf);
        if (expected_a) LocalFree(expected_a);
        if (expected_b) LocalFree(expected_b);
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: alloc\n");
        return 1;
    }

    FillMemory(expected_a, DS2_TEST_CHAR_A_SERIALIZED_SIZE, 0xAA);
    FillMemory(expected_b, DS2_TEST_CHAR_B_SERIALIZED_SIZE, 0xBB);

    /* Reject buf_size = DS2_CHAR_DATA_SERIALIZED_SIZE - 1 to confirm size gate */
    if (ds2_char_data_serialize(c0, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE - 1)) {
        LocalFree(ser_buf); LocalFree(expected_a); LocalFree(expected_b);
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: serialize should reject undersized buffer\n");
        return 1;
    }

    if (!ds2_char_data_serialize(c0, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf); LocalFree(expected_a); LocalFree(expected_b);
        ds2_save_data_free(save);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: serialize\n");
        return 1;
    }

    ok = (RtlCompareMemory(ser_buf, expected_a, DS2_TEST_CHAR_A_SERIALIZED_SIZE)
              == DS2_TEST_CHAR_A_SERIALIZED_SIZE)
         && (RtlCompareMemory(ser_buf + DS2_TEST_CHAR_A_SERIALIZED_SIZE, expected_b,
                              DS2_TEST_CHAR_B_SERIALIZED_SIZE)
              == DS2_TEST_CHAR_B_SERIALIZED_SIZE);

    LocalFree(ser_buf); LocalFree(expected_a); LocalFree(expected_b);
    ds2_save_data_free(save);
    DeleteFileW(tmp_path);

    if (!ok) {
        st_printf(L"FAIL: part A/B markers not preserved in serialized blob\n");
        return 1;
    }
    st_printf(L"PASS: ds2s-dual-slot-roundtrip\n");
    return 0;
}

/* 7. ds2s-import-resigns-userid-text <srcA> <dstB>
 * Build srcA with userid_hex16 DS2_TEST_USERID_A_HEX.
 * Build dstB with userid_hex16 DS2_TEST_USERID_B_HEX.
 * Import slot 0 from A into B. Reload B and assert that B's summary
 * Steam ID at offset 0x39 is unchanged (still DS2_TEST_USERID_B_HEX) -
 * import must preserve the destination's userid (the "re-sign" semantic).
 * DESTRUCTIVE: deletes both <srcA> and <dstB>. */
static int cmd_ds2s_import_resigns_userid_text(int argc, wchar_t **argv) {
    const wchar_t *path_a;
    const wchar_t *path_b;
    ds2_save_data_t *save_a = NULL;
    ds2_save_data_t *save_b = NULL;
    ds2_char_data_t *c0_a = NULL;
    uint8_t *ser_buf = NULL;
    uint8_t *summary_pt = NULL;
    bool ok;

    if (argc < 5) {
        st_printf(L"Usage: ds2s-import-resigns-userid-text <srcA> <dstB>\n");
        return 1;
    }
    path_a = argv[3];
    path_b = argv[4];

    if (!praxis_make_min_valid_ds2_sl2(path_a, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture A builder failed\n");
        return 1;
    }
    if (!praxis_make_min_valid_ds2_sl2(path_b, DS2_TEST_USERID_B_HEX)) {
        DeleteFileW(path_a);
        st_printf(L"FAIL: fixture B builder failed\n");
        return 1;
    }

    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_CHAR_DATA_SERIALIZED_SIZE);
    summary_pt = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_SUMMARY_PLAINTEXT_SIZE);
    if (!ser_buf || !summary_pt) {
        if (ser_buf) LocalFree(ser_buf);
        if (summary_pt) LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: alloc\n");
        return 1;
    }

    if (!ds2_save_data_load(path_a, &save_a) || !save_a) {
        LocalFree(ser_buf); LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: load A\n");
        return 1;
    }
    c0_a = ds2_char_data_ref(save_a, 0);
    if (!c0_a || !ds2_char_data_serialize(c0_a, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        ds2_save_data_free(save_a);
        LocalFree(ser_buf); LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: serialize A slot 0\n");
        return 1;
    }
    ds2_save_data_free(save_a);
    save_a = NULL;

    if (!ds2_save_data_load(path_b, &save_b) || !save_b) {
        LocalFree(ser_buf); LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: load B\n");
        return 1;
    }
    if (!ds2_char_data_import_raw(save_b, 0, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        ds2_save_data_free(save_b);
        LocalFree(ser_buf); LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: import_raw into B\n");
        return 1;
    }
    ds2_save_data_free(save_b);
    save_b = NULL;
    LocalFree(ser_buf);
    ser_buf = NULL;

    /* Re-read B's summary directly from disk and check Steam ID text. */
    if (!ds2_st_decrypt_summary_from_file(path_b, summary_pt)) {
        LocalFree(summary_pt);
        DeleteFileW(path_a); DeleteFileW(path_b);
        st_printf(L"FAIL: decrypt B summary after import\n");
        return 1;
    }

    ok = (RtlCompareMemory(summary_pt + DS2_TEST_SUMMARY_USERID_TEXT_OFFSET,
                           DS2_TEST_USERID_B_HEX, DS2_USERID_TEXT_LENGTH)
            == DS2_USERID_TEXT_LENGTH);

    LocalFree(summary_pt);
    DeleteFileW(path_a);
    DeleteFileW(path_b);

    if (!ok) {
        st_printf(L"FAIL: B's summary userid changed after import (destination must be preserved)\n");
        return 1;
    }
    st_printf(L"PASS: ds2s-import-resigns-userid-text (B's userid preserved)\n");
    return 0;
}

/* 8. ds2s-available-slots-by-profile-byte <tmp>
 * Build fixture, manually flip profile[3] availability flag to 1 and
 * profile[5] flag to 0 (already 0 in the fixture) in the raw file. Load
 * and assert ds2_char_data_ref(save, 3) is non-NULL while
 * ds2_char_data_ref(save, 5) is NULL. Verifies that availability is
 * driven by the int32 flag at profile_base+4, not by DS3's bitmap.
 * DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_available_slots_by_profile_byte(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    ds2_save_data_t *save = NULL;
    bool ok;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-available-slots-by-profile-byte <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    if (!ds2_st_set_summary_profile_flag(tmp_path, 3, 1)) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: set profile[3] flag = 1\n");
        return 1;
    }
    if (!ds2_st_set_summary_profile_flag(tmp_path, 5, 0)) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: set profile[5] flag = 0\n");
        return 1;
    }

    if (!ds2_save_data_load(tmp_path, &save) || !save) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: load after flag mutation\n");
        return 1;
    }

    ok = (ds2_char_data_ref(save, 3) != NULL)
         && (ds2_char_data_ref(save, 5) == NULL);

    ds2_save_data_free(save);
    DeleteFileW(tmp_path);

    if (!ok) {
        st_printf(L"FAIL: ref(3) must be non-NULL and ref(5) must be NULL\n");
        return 1;
    }
    st_printf(L"PASS: ds2s-available-slots-by-profile-byte\n");
    return 0;
}

/* 9. ds2s-bnd4-entry-count <tmp>
 * Build fixture, read raw BND4 header, assert uint32 at offset 0x0C ==
 * DS2_BND4_TOTAL_ENTRY_COUNT (23). DESTRUCTIVE: deletes <tmp>. */
static int cmd_ds2s_bnd4_entry_count(int argc, wchar_t **argv) {
    const wchar_t *tmp_path;
    HANDLE fh;
    DWORD bytes_read = 0;
    uint32_t header[4];
    uint32_t entry_count;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-bnd4-entry-count <tmp_path>\n");
        return 1;
    }
    tmp_path = argv[3];

    if (!praxis_make_min_valid_ds2_sl2(tmp_path, DS2_TEST_USERID_A_HEX)) {
        st_printf(L"FAIL: fixture builder failed\n");
        return 1;
    }
    fh = CreateFileW(tmp_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: open\n");
        return 1;
    }
    if (!ReadFile(fh, header, sizeof(header), &bytes_read, NULL)
        || bytes_read != sizeof(header)) {
        CloseHandle(fh);
        DeleteFileW(tmp_path);
        st_printf(L"FAIL: read header\n");
        return 1;
    }
    CloseHandle(fh);
    DeleteFileW(tmp_path);

    entry_count = header[3]; /* uint32 at offset 0x0C */
    if (entry_count != DS2_BND4_TOTAL_ENTRY_COUNT) {
        st_printf(L"FAIL: entry count %u, expected %u\n",
                  entry_count, (uint32_t)DS2_BND4_TOTAL_ENTRY_COUNT);
        return 1;
    }
    st_printf(L"PASS: ds2s-bnd4-entry-count (%u)\n", entry_count);
    return 0;
}

/* 10. ds2s-real-save-load <path>
 * Load a real DS2S save (argv[3]) and print active slot + per-slot
 * availability. If the path is absent, print SKIP and return 0.
 * Does NOT modify the file. */
static int cmd_ds2s_real_save_load(int argc, wchar_t **argv) {
    const wchar_t *path;
    ds2_save_data_t *save = NULL;
    int slot = -1;
    int i;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-real-save-load <path>\n");
        return 1;
    }
    path = argv[3];

    if (!PathFileExistsW(path)) {
        st_printf(L"SKIP: real save not present: %ls\n", path);
        return 0;
    }

    if (!ds2_save_data_load(path, &save) || !save) {
        st_printf(L"FAIL: ds2_save_data_load returned false for real save\n");
        return 1;
    }

    if (ds2_save_get_active_slot(save, &slot)) {
        st_printf(L"Active slot: %d\n", slot);
    } else {
        st_printf(L"Active slot: (not available)\n");
    }
    for (i = 0; i < 10; i++) {
        ds2_char_data_t *c = ds2_char_data_ref(save, i);
        st_printf(L"Slot %d: %ls\n", i, c ? L"available" : L"empty");
    }

    ds2_save_data_free(save);
    st_printf(L"PASS: ds2s-real-save-load\n");
    return 0;
}

/* 11. ds2s-real-save-classify <path>
 * Read BND4 magic + entry count of a real DS2S save and print them.
 * If the path is absent, print SKIP and return 0. Does NOT modify the file. */
static int cmd_ds2s_real_save_classify(int argc, wchar_t **argv) {
    const wchar_t *path;
    HANDLE fh;
    DWORD file_size = 0;
    DWORD bytes_read = 0;
    uint8_t header[0x10];
    uint8_t entry_count_buf[4];
    int entry_count;

    if (argc < 4) {
        st_printf(L"Usage: ds2s-real-save-classify <path>\n");
        return 1;
    }
    path = argv[3];

    if (!PathFileExistsW(path)) {
        st_printf(L"SKIP: real save not present: %ls\n", path);
        return 0;
    }

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        st_printf(L"FAIL: cannot open file\n");
        return 1;
    }
    file_size = GetFileSize(fh, NULL);
    if (!ReadFile(fh, header, sizeof(header), &bytes_read, NULL)
        || bytes_read != sizeof(header)) {
        CloseHandle(fh);
        st_printf(L"FAIL: cannot read header\n");
        return 1;
    }
    if (header[0] != 'B' || header[1] != 'N'
        || header[2] != 'D' || header[3] != '4') {
        CloseHandle(fh);
        st_printf(L"FAIL: not a BND4 file\n");
        return 1;
    }
    SetFilePointer(fh, 0x0C, NULL, FILE_BEGIN);
    if (!ReadFile(fh, entry_count_buf, 4, &bytes_read, NULL) || bytes_read != 4) {
        CloseHandle(fh);
        st_printf(L"FAIL: cannot read entry count\n");
        return 1;
    }
    CloseHandle(fh);

    entry_count = *(int *)entry_count_buf;
    if (entry_count != DS2_BND4_TOTAL_ENTRY_COUNT) {
        st_printf(L"FAIL: entry count %d, expected %d\n",
                  entry_count, DS2_BND4_TOTAL_ENTRY_COUNT);
        return 1;
    }
    st_printf(L"File size: %u bytes\n", file_size);
    st_printf(L"Entry count: %d\n", entry_count);
    st_printf(L"PASS: ds2s-real-save-classify\n");
    return 0;
}

/* 12. ds2s-real-save-roundtrip-readonly <path> <tmp_copy>
 * SHA256(path) -> hash_before.
 * CopyFileW(path -> tmp_copy).
 * Load tmp_copy, find active slot, serialize that slot, import_raw it back
 * into tmp_copy at the same slot index. Reload tmp_copy and verify the
 * active slot is unchanged.
 * DeleteFileW(tmp_copy).
 * SHA256(path) -> hash_after.
 * Assert hash_before == hash_after (original real save must be untouched).
 * If <path> is absent, print SKIP and return 0. */
static int cmd_ds2s_real_save_roundtrip_readonly(int argc, wchar_t **argv) {
    const wchar_t *path;
    const wchar_t *tmp_copy;
    uint8_t hash_before[32];
    uint8_t hash_after[32];
    ds2_save_data_t *save = NULL;
    int active_slot = -1;
    int active_slot_after = -1;
    int first_slot = -1;
    int i;
    ds2_char_data_t *c;
    uint8_t *ser_buf = NULL;

    if (argc < 5) {
        st_printf(L"Usage: ds2s-real-save-roundtrip-readonly <path> <tmp_copy>\n");
        return 1;
    }
    path = argv[3];
    tmp_copy = argv[4];

    if (!PathFileExistsW(path)) {
        st_printf(L"SKIP: real save not present: %ls\n", path);
        return 0;
    }

    if (!ds2_st_sha256_file(path, hash_before)) {
        st_printf(L"FAIL: sha256 before\n");
        return 1;
    }
    if (!CopyFileW(path, tmp_copy, FALSE)) {
        st_printf(L"FAIL: CopyFileW\n");
        return 1;
    }

    if (!ds2_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        st_printf(L"FAIL: load tmp copy\n");
        return 1;
    }

    ds2_save_get_active_slot(save, &active_slot);

    /* Prefer the active slot if available; otherwise fall back to the first
     * available slot. Some real saves may have an out-of-range active slot. */
    if (active_slot >= 0 && active_slot < 10 && ds2_char_data_ref(save, active_slot)) {
        first_slot = active_slot;
    } else {
        for (i = 0; i < 10; i++) {
            if (ds2_char_data_ref(save, i)) {
                first_slot = i;
                break;
            }
        }
    }

    if (first_slot < 0) {
        ds2_save_data_free(save);
        DeleteFileW(tmp_copy);
        st_printf(L"SKIP: no available char slots in real save\n");
        return 0;
    }

    c = ds2_char_data_ref(save, first_slot);
    ser_buf = (uint8_t *)LocalAlloc(LMEM_FIXED, DS2_CHAR_DATA_SERIALIZED_SIZE);
    if (!ser_buf) {
        ds2_save_data_free(save);
        DeleteFileW(tmp_copy);
        st_printf(L"FAIL: alloc\n");
        return 1;
    }
    if (!ds2_char_data_serialize(c, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        ds2_save_data_free(save);
        DeleteFileW(tmp_copy);
        st_printf(L"FAIL: serialize\n");
        return 1;
    }
    if (!ds2_char_data_import_raw(save, first_slot, ser_buf, DS2_CHAR_DATA_SERIALIZED_SIZE)) {
        LocalFree(ser_buf);
        ds2_save_data_free(save);
        DeleteFileW(tmp_copy);
        st_printf(L"FAIL: import_raw\n");
        return 1;
    }
    LocalFree(ser_buf);
    ds2_save_data_free(save);
    save = NULL;

    if (!ds2_save_data_load(tmp_copy, &save) || !save) {
        DeleteFileW(tmp_copy);
        st_printf(L"FAIL: reload after roundtrip\n");
        return 1;
    }
    ds2_save_get_active_slot(save, &active_slot_after);
    ds2_save_data_free(save);
    DeleteFileW(tmp_copy);

    if (active_slot != active_slot_after) {
        st_printf(L"FAIL: active slot changed from %d to %d\n",
                  active_slot, active_slot_after);
        return 1;
    }

    if (!ds2_st_sha256_file(path, hash_after)) {
        st_printf(L"FAIL: sha256 after\n");
        return 1;
    }
    if (RtlCompareMemory(hash_before, hash_after, 32) != 32) {
        st_printf(L"FAIL: real save modified by readonly roundtrip\n");
        return 1;
    }

    st_printf(L"PASS: ds2s-real-save-roundtrip-readonly (slot %d, active=%d)\n",
              first_slot, active_slot);
    return 0;
}

/* --- Dispatcher --------------------------------------------------------- */

int praxis_selftest_ds2_dispatch(int argc, wchar_t **argv, const wchar_t *sub) {
    if (!sub) {
        return -1;
    }

    if (wcscmp(sub, L"ds2s-aes-known-vector") == 0) {
        return cmd_ds2s_aes_known_vector();
    } else if (wcscmp(sub, L"ds2s-null-guards") == 0) {
        return cmd_ds2s_null_guards();
    } else if (wcscmp(sub, L"ds2s-load-min-fixture") == 0) {
        return cmd_ds2s_load_min_fixture(argc, argv);
    } else if (wcscmp(sub, L"ds2s-roundtrip-byte-stable") == 0) {
        return cmd_ds2s_roundtrip_byte_stable(argc, argv);
    } else if (wcscmp(sub, L"ds2s-active-slot") == 0) {
        return cmd_ds2s_active_slot(argc, argv);
    } else if (wcscmp(sub, L"ds2s-dual-slot-roundtrip") == 0) {
        return cmd_ds2s_dual_slot_roundtrip(argc, argv);
    } else if (wcscmp(sub, L"ds2s-import-resigns-userid-text") == 0) {
        return cmd_ds2s_import_resigns_userid_text(argc, argv);
    } else if (wcscmp(sub, L"ds2s-available-slots-by-profile-byte") == 0) {
        return cmd_ds2s_available_slots_by_profile_byte(argc, argv);
    } else if (wcscmp(sub, L"ds2s-bnd4-entry-count") == 0) {
        return cmd_ds2s_bnd4_entry_count(argc, argv);
    } else if (wcscmp(sub, L"ds2s-real-save-load") == 0) {
        return cmd_ds2s_real_save_load(argc, argv);
    } else if (wcscmp(sub, L"ds2s-real-save-classify") == 0) {
        return cmd_ds2s_real_save_classify(argc, argv);
    } else if (wcscmp(sub, L"ds2s-real-save-roundtrip-readonly") == 0) {
        return cmd_ds2s_real_save_roundtrip_readonly(argc, argv);
    }

    return -1;
}

#endif /* PRAXIS_ENABLE_SELFTEST */
