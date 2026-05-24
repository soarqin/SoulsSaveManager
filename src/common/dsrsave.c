/**
 * @file dsrsave.c
 * @brief Dark Souls: Remastered save file format module.
 * @details Implements load/free/serialize/import for DSR .sl2 save files.
 *          Modeled after ds3save.c with four deltas:
 *          (1) different AES key,
 *          (2) different summary offsets,
 *          (3) 1-byte active slot at 0x45 (not the DS3 four-byte field),
 *          (4) no Steam ID re-signing in import_raw.
 */

#include "dsrsave.h"
#include <md5.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <windows.h>
#include <bcrypt.h>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#define DSR_AES_KEY_SIZE    16u
#define DSR_SLOT_COUNT      10
#define DSR_SUMMARY_INDEX   10
#define DSR_BND4_MIN_ENTRIES 11   /* 10 char + 1 summary; no regulation slot */

/* Slot on-disk sizes (empirical, T1) */
#define DSR_CHAR_SLOT_FILE_SIZE     0x60030u
#define DSR_SUMMARY_SLOT_FILE_SIZE  0x60030u

/* Plaintext buffer sizes (file_size - 32 for AES-CBC) */
#define DSR_CHAR_PLAINTEXT_SIZE     0x60010u
#define DSR_SUMMARY_PLAINTEXT_SIZE  0x60010u

/* Profile size */
#define DSR_PROFILE_SIZE            0x190u

/* Summary offsets (within decrypted plaintext) */
#define DSR_SUMMARY_ACTIVE_OFFSET    0x45u   /* uint8 — 1 BYTE */
#define DSR_SUMMARY_AVAILABLE_OFFSET 0xB0u   /* 10 bytes, 1 per slot */
#define DSR_SUMMARY_PROFILE_OFFSET   0xC0u

#define DSR_SLOT_HEADER_SIZE         32u      /* 16-byte MD5 + 16-byte IV */
#define DSR_HEADER_SLOT_COUNT_OFFSET 0x0Cu
#define DSR_HEADER_SLOT_SIZE_BASE    0x48u
#define DSR_HEADER_SLOT_OFFSET_BASE  0x50u
#define DSR_HEADER_SLOT_STRIDE       0x20u
#define DSR_FILE_HEADER_SIZE         0x300u

/* AES key (hex-decode of "0123456789ABCDEFFEDCBA9876543210") */
static const uint8_t DSR_AES_KEY[DSR_AES_KEY_SIZE] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

typedef struct dsr_char_data_s {
    uint8_t plaintext[DSR_CHAR_PLAINTEXT_SIZE];
    uint8_t profile[DSR_PROFILE_SIZE];
    bool    available;
} dsr_char_data_t;

typedef struct dsr_save_data_s {
    wchar_t          path[MAX_PATH];
    uint8_t          summary_plaintext[DSR_SUMMARY_PLAINTEXT_SIZE];
    dsr_char_data_t  chars[DSR_SLOT_COUNT];
    /* BND4 slot table (offsets + sizes for writing back) */
    uint32_t         slot_offset[DSR_BND4_MIN_ENTRIES];
    uint32_t         slot_size[DSR_BND4_MIN_ENTRIES];
} dsr_save_data_t;

static bool path_fits_fixed_buffer(const wchar_t *path) {
    return path && (size_t)lstrlenW(path) < MAX_PATH;
}

static uint32_t read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static bool slot_range_is_valid(uint32_t file_size, uint32_t offset, uint32_t size) {
    return offset <= file_size && size <= file_size - offset;
}

static bool dsr_aes_open(BCRYPT_ALG_HANDLE *out_alg, BCRYPT_KEY_HANDLE *out_key) {
    if (!out_alg || !out_key) {
        return false;
    }

    *out_alg = NULL;
    *out_key = NULL;

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (status < 0) {
        return false;
    }

    status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    status = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, (PUCHAR)DSR_AES_KEY, sizeof(DSR_AES_KEY), 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    *out_alg = alg;
    *out_key = key;
    return true;
}

static void dsr_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key) {
    if (key) {
        BCryptDestroyKey(key);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
}

static bool dsr_aes_decrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv, const uint8_t *ct, uint32_t ct_size, uint8_t *pt_buf, uint32_t pt_buf_size) {
    if (!key || !iv || !ct || !pt_buf) {
        return false;
    }

    uint8_t iv_copy[16];
    ULONG plaintext_size = 0;
    CopyMemory(iv_copy, iv, sizeof(iv_copy));
    NTSTATUS status = BCryptDecrypt(key, (PUCHAR)ct, ct_size, NULL, iv_copy, sizeof(iv_copy), pt_buf, pt_buf_size, &plaintext_size, 0);
    return status >= 0 && plaintext_size == pt_buf_size;
}

static bool dsr_aes_encrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv, const uint8_t *pt, uint32_t pt_size, uint8_t *ct_buf, uint32_t ct_buf_size, uint32_t *out_ct_size) {
    if (!key || !iv || !pt || !ct_buf || !out_ct_size) {
        return false;
    }

    uint8_t iv_copy[16];
    ULONG ciphertext_size = 0;
    CopyMemory(iv_copy, iv, sizeof(iv_copy));
    NTSTATUS status = BCryptEncrypt(key, (PUCHAR)pt, pt_size, NULL, iv_copy, sizeof(iv_copy), ct_buf, ct_buf_size, &ciphertext_size, 0);
    if (status < 0) {
        return false;
    }

    *out_ct_size = ciphertext_size;
    return true;
}

static bool dsr_md5_iv_ct(const uint8_t *iv, const uint8_t *ct, uint32_t ct_size, uint8_t out_md5[16]) {
    if (!iv || !ct || !out_md5) {
        return false;
    }

    uint8_t *tmp = LocalAlloc(LMEM_FIXED, 16u + ct_size);
    if (!tmp) {
        return false;
    }

    CopyMemory(tmp, iv, 16);
    CopyMemory(tmp + 16, ct, ct_size);
    md5_buffer(tmp, 16u + ct_size, out_md5);
    LocalFree(tmp);
    return true;
}

static bool dsr_read_exact_at(HANDLE file, uint32_t offset, uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) {
        return false;
    }

    DWORD bytes_read = 0;
    return ReadFile(file, buf, len, &bytes_read, NULL) && bytes_read == len;
}

static bool dsr_write_at(HANDLE file, uint32_t offset, const uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) {
        return false;
    }

    DWORD bytes_written = 0;
    return WriteFile(file, buf, len, &bytes_written, NULL) && bytes_written == len;
}

static bool dsr_decrypt_slot(BCRYPT_KEY_HANDLE key, const uint8_t *file_data, uint32_t file_size, uint32_t slot_offset, uint32_t slot_size, uint8_t *out_plaintext, uint32_t plaintext_size) {
    if (!key || !file_data || !out_plaintext || slot_size < DSR_SLOT_HEADER_SIZE) {
        return false;
    }
    if (!slot_range_is_valid(file_size, slot_offset, slot_size)) {
        return false;
    }

    uint32_t ciphertext_size = slot_size - DSR_SLOT_HEADER_SIZE;
    if (ciphertext_size != plaintext_size) {
        return false;
    }

    const uint8_t *iv = file_data + slot_offset + 16;
    const uint8_t *ciphertext = file_data + slot_offset + DSR_SLOT_HEADER_SIZE;
    return dsr_aes_decrypt(key, iv, ciphertext, ciphertext_size, out_plaintext, plaintext_size);
}

bool dsr_save_data_load(const wchar_t *path, dsr_save_data_t **out_save) {
    if (!path_fits_fixed_buffer(path) || !out_save) {
        return false;
    }
    *out_save = NULL;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size_value;
    if (!GetFileSizeEx(file, &file_size_value) || file_size_value.QuadPart < DSR_FILE_HEADER_SIZE || file_size_value.QuadPart > (LONGLONG)UINT32_MAX) {
        CloseHandle(file);
        return false;
    }

    uint32_t file_size = (uint32_t)file_size_value.QuadPart;
    uint8_t *file_data = LocalAlloc(LMEM_FIXED, file_size);
    if (!file_data) {
        CloseHandle(file);
        return false;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(file, file_data, file_size, &bytes_read, NULL) || bytes_read != file_size) {
        LocalFree(file_data);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);

    if (file_data[0] != 'B' || file_data[1] != 'N' || file_data[2] != 'D' || file_data[3] != '4') {
        LocalFree(file_data);
        return false;
    }

    uint32_t save_slot_count = read_u32_le(file_data + DSR_HEADER_SLOT_COUNT_OFFSET);
    if (save_slot_count < DSR_BND4_MIN_ENTRIES) {
        LocalFree(file_data);
        return false;
    }

    uint32_t slot_offset[DSR_BND4_MIN_ENTRIES];
    uint32_t slot_size[DSR_BND4_MIN_ENTRIES];
    for (int i = 0; i < DSR_BND4_MIN_ENTRIES; i++) {
        uint32_t table_pos = (uint32_t)(i * DSR_HEADER_SLOT_STRIDE);
        slot_size[i] = read_u32_le(file_data + DSR_HEADER_SLOT_SIZE_BASE + table_pos);
        slot_offset[i] = read_u32_le(file_data + DSR_HEADER_SLOT_OFFSET_BASE + table_pos);
        if (!slot_range_is_valid(file_size, slot_offset[i], slot_size[i])) {
            LocalFree(file_data);
            return false;
        }
        if (i < DSR_SLOT_COUNT && slot_size[i] != DSR_CHAR_SLOT_FILE_SIZE) {
            LocalFree(file_data);
            return false;
        }
        if (i == DSR_SUMMARY_INDEX && slot_size[i] != DSR_SUMMARY_SLOT_FILE_SIZE) {
            LocalFree(file_data);
            return false;
        }
    }

    dsr_save_data_t *save = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(dsr_save_data_t));
    if (!save) {
        LocalFree(file_data);
        return false;
    }

    for (int i = 0; i < DSR_BND4_MIN_ENTRIES; i++) {
        save->slot_offset[i] = slot_offset[i];
        save->slot_size[i] = slot_size[i];
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    if (!dsr_aes_open(&alg, &key)) {
        LocalFree(save);
        LocalFree(file_data);
        return false;
    }

    if (!dsr_decrypt_slot(key, file_data, file_size, save->slot_offset[DSR_SUMMARY_INDEX], save->slot_size[DSR_SUMMARY_INDEX], save->summary_plaintext, DSR_SUMMARY_PLAINTEXT_SIZE)) {
        dsr_aes_close(alg, key);
        LocalFree(save);
        LocalFree(file_data);
        return false;
    }

    for (int i = 0; i < DSR_SLOT_COUNT; i++) {
        if (save->summary_plaintext[DSR_SUMMARY_AVAILABLE_OFFSET + i] != 0) {
            if (!dsr_decrypt_slot(key, file_data, file_size, save->slot_offset[i], save->slot_size[i], save->chars[i].plaintext, DSR_CHAR_PLAINTEXT_SIZE)) {
                dsr_aes_close(alg, key);
                LocalFree(save);
                LocalFree(file_data);
                return false;
            }
            CopyMemory(save->chars[i].profile, save->summary_plaintext + DSR_SUMMARY_PROFILE_OFFSET + DSR_PROFILE_SIZE * i, DSR_PROFILE_SIZE);
            save->chars[i].available = true;
        }
    }

    dsr_aes_close(alg, key);
    LocalFree(file_data);
    lstrcpyW(save->path, path);
    *out_save = save;
    return true;
}

void dsr_save_data_free(dsr_save_data_t *save) {
    if (save) {
        LocalFree(save);
    }
}

bool dsr_save_get_active_slot(const dsr_save_data_t *save, int *out_slot) {
    if (!save || !out_slot) {
        return false;
    }

    /* CRITICAL: DSR uses 1 BYTE at offset 0x45, not the DS3 four-byte field. */
    uint8_t active = save->summary_plaintext[DSR_SUMMARY_ACTIVE_OFFSET];
    if (active >= DSR_SLOT_COUNT) {
        return false;
    }

    *out_slot = (int)active;
    return true;
}

dsr_char_data_t *dsr_char_data_ref(dsr_save_data_t *save, int slot) {
    if (!save || slot < 0 || slot >= DSR_SLOT_COUNT) {
        return NULL;
    }
    if (!save->chars[slot].available) {
        return NULL;
    }

    return &save->chars[slot];
}

bool dsr_char_data_serialize(const dsr_char_data_t *char_data, uint8_t *out_buf, size_t buf_size) {
    if (!char_data || !out_buf || buf_size < DSR_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    CopyMemory(out_buf, char_data->plaintext, DSR_CHAR_PLAINTEXT_SIZE);
    CopyMemory(out_buf + DSR_CHAR_PLAINTEXT_SIZE, char_data->profile, DSR_PROFILE_SIZE);
    return true;
}

bool dsr_char_data_import_raw(dsr_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size) {
    if (!save || slot < 0 || slot >= DSR_SLOT_COUNT || !raw_data || raw_size != DSR_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    CopyMemory(save->chars[slot].plaintext, raw_data, DSR_CHAR_PLAINTEXT_SIZE);
    CopyMemory(save->chars[slot].profile, raw_data + DSR_CHAR_PLAINTEXT_SIZE, DSR_PROFILE_SIZE);
    save->chars[slot].available = true;
    CopyMemory(save->summary_plaintext + DSR_SUMMARY_PROFILE_OFFSET + slot * DSR_PROFILE_SIZE, raw_data + DSR_CHAR_PLAINTEXT_SIZE, DSR_PROFILE_SIZE);
    save->summary_plaintext[DSR_SUMMARY_AVAILABLE_OFFSET + slot] = 1;

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    if (!dsr_aes_open(&alg, &key)) {
        return false;
    }

    HANDLE file = CreateFileW(save->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        dsr_aes_close(alg, key);
        return false;
    }

    uint32_t char_ciphertext_capacity = save->slot_size[slot] - DSR_SLOT_HEADER_SIZE;
    uint32_t summary_ciphertext_capacity = save->slot_size[DSR_SUMMARY_INDEX] - DSR_SLOT_HEADER_SIZE;
    uint32_t ciphertext_capacity = char_ciphertext_capacity > summary_ciphertext_capacity
        ? char_ciphertext_capacity : summary_ciphertext_capacity;
    uint8_t *ciphertext = LocalAlloc(LMEM_FIXED, ciphertext_capacity);
    if (!ciphertext) {
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    uint8_t iv[16];
    if (!dsr_read_exact_at(file, save->slot_offset[slot] + 16, iv, sizeof(iv))) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    uint32_t ciphertext_size = 0;
    if (!dsr_aes_encrypt(key, iv, save->chars[slot].plaintext, DSR_CHAR_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity, &ciphertext_size)
        || ciphertext_size != char_ciphertext_capacity) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    uint8_t md5[16];
    if (!dsr_md5_iv_ct(iv, ciphertext, ciphertext_size, md5)
        || !dsr_write_at(file, save->slot_offset[slot], md5, sizeof(md5))
        || !dsr_write_at(file, save->slot_offset[slot] + 16, iv, sizeof(iv))
        || !dsr_write_at(file, save->slot_offset[slot] + DSR_SLOT_HEADER_SIZE, ciphertext, ciphertext_size)) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    uint8_t summary_iv[16];
    if (!dsr_read_exact_at(file, save->slot_offset[DSR_SUMMARY_INDEX] + 16, summary_iv, sizeof(summary_iv))) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    uint32_t summary_ciphertext_size = 0;
    if (!dsr_aes_encrypt(key, summary_iv, save->summary_plaintext, DSR_SUMMARY_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity, &summary_ciphertext_size)
        || summary_ciphertext_size != summary_ciphertext_capacity) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    if (!dsr_md5_iv_ct(summary_iv, ciphertext, summary_ciphertext_size, md5)
        || !dsr_write_at(file, save->slot_offset[DSR_SUMMARY_INDEX], md5, sizeof(md5))
        || !dsr_write_at(file, save->slot_offset[DSR_SUMMARY_INDEX] + 16, summary_iv, sizeof(summary_iv))
        || !dsr_write_at(file, save->slot_offset[DSR_SUMMARY_INDEX] + DSR_SLOT_HEADER_SIZE, ciphertext, summary_ciphertext_size)) {
        LocalFree(ciphertext);
        CloseHandle(file);
        dsr_aes_close(alg, key);
        return false;
    }

    LocalFree(ciphertext);
    CloseHandle(file);
    dsr_aes_close(alg, key);
    return true;
}
