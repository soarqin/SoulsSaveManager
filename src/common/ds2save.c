/**
 * @file ds2save.c
 * @brief Dark Souls II: Scholar of the First Sin save file format module.
 * @details Implements load/free/serialize/import for DS2S .sl2 save files.
 *          Key differences from DS3:
 *          (1) Dual-slot abstraction: char N = BND4 entry (N+1) [part A] + entry (N+11) [part B]
 *          (2) Steam ID is TEXT (16 lowercase hex chars at summary[0x39]), NOT binary uint64
 *          (3) Availability via int32 flag at profile_base+4 (NOT DS3's bitmap)
 *          (4) 23 BND4 entries (1 summary + 10 part-A + 10 part-B + 2 extras)
 *          (5) Profile size 0x1F0 (NOT DS3's 0x22A)
 *          (6) Active slot: int32 at summary offset 0x36C
 */

#include "ds2save.h"
#include <md5.h>
#include <stdint.h>
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>

#define DS2_AES_KEY_SIZE          16u
#define DS2_SLOT_FILE_HEADER_SIZE 32u   /* 16-byte MD5 + 16-byte IV */
#define DS2_SLOT_COUNT            10
#define DS2_SUMMARY_INDEX         0
#define DS2_BND4_TOTAL_ENTRIES    23
#define DS2_CHAR_A_BASE_INDEX     1     /* BND4 entries 1..10 = char part A */
#define DS2_CHAR_B_BASE_INDEX     11    /* BND4 entries 11..20 = char part B */

/* Slot on-disk sizes (empirical, T1) */
#define DS2_SUMMARY_SLOT_FILE_SIZE   0x1AB0u
#define DS2_CHAR_A_SLOT_FILE_SIZE    0x1B2E0u
#define DS2_CHAR_B_SLOT_FILE_SIZE    0x7A8D0u

/* Plaintext buffer sizes (file_size - 32 for AES-CBC) */
#define DS2_SUMMARY_PLAINTEXT_SIZE   0x1A90u
#define DS2_CHAR_A_PLAINTEXT_SIZE    0x1B2C0u
#define DS2_CHAR_B_PLAINTEXT_SIZE    0x7A8B0u

#define DS2_CHAR_A_SERIALIZED_SIZE    DS2_CHAR_A_PLAINTEXT_SIZE
#define DS2_CHAR_B_SERIALIZED_SIZE    DS2_CHAR_B_PLAINTEXT_SIZE

/* Profile */
#define DS2_PROFILE_SIZE             0x1F0u
#define DS2_PROFILE_AVAILABLE_FLAG_OFFSET  0u  /* int32 LE at profile+0: 1=used, 0=unused */

/* Summary offsets */
#define DS2_SUMMARY_STEAMID_OFFSET   0x39u
#define DS2_STEAMID_TEXT_LENGTH      16u
#define DS2_SUMMARY_ACTIVE_OFFSET    0x36Cu
#define DS2_SUMMARY_PROFILE_OFFSET   0x37Cu

/* DS2_PROFILE_EMBEDDED_STEAMID_OFFSET: T1 found NO embedded Steam ID in profile region.
 * Define as -1 to indicate no patching needed. */
#define DS2_PROFILE_EMBEDDED_STEAMID_OFFSET  (-1)

/* BND4 header offsets (same table layout as ER/DS3, but DS2S has more entries) */
#define DS2_HEADER_SLOT_COUNT_OFFSET  0x0C
#define DS2_HEADER_SLOT_SIZE_BASE     0x48
#define DS2_HEADER_SLOT_OFFSET_BASE   0x50
#define DS2_HEADER_SLOT_STRIDE        0x20
#define DS2_HEADER_TABLE_SIZE         (DS2_HEADER_SLOT_OFFSET_BASE + (DS2_BND4_TOTAL_ENTRIES - 1) * DS2_HEADER_SLOT_STRIDE + sizeof(uint32_t))

/* AES key (hex-decode of "599F9B699640A55236EE2D70835EC744") */
static const uint8_t DS2_AES_KEY[DS2_AES_KEY_SIZE] = {
    0x59, 0x9F, 0x9B, 0x69, 0x96, 0x40, 0xA5, 0x52,
    0x36, 0xEE, 0x2D, 0x70, 0x83, 0x5E, 0xC7, 0x44
};

typedef struct ds2_char_data_s {
    uint8_t part_a[DS2_CHAR_A_PLAINTEXT_SIZE];
    uint8_t part_b[DS2_CHAR_B_PLAINTEXT_SIZE];
    uint8_t profile[DS2_PROFILE_SIZE];
    bool    available;
} ds2_char_data_t;

typedef struct ds2_save_data_s {
    wchar_t          path[MAX_PATH];
    uint8_t          summary_plaintext[DS2_SUMMARY_PLAINTEXT_SIZE];
    ds2_char_data_t  chars[DS2_SLOT_COUNT];
    uint32_t         slot_offset[DS2_BND4_TOTAL_ENTRIES];
    uint32_t         slot_size[DS2_BND4_TOTAL_ENTRIES];
} ds2_save_data_t;

static bool path_fits_fixed_buffer(const wchar_t *path) {
    return path && (size_t)lstrlenW(path) < MAX_PATH;
}

static void ds2_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key, uint8_t *key_obj);

static uint32_t read_u32_le(const uint8_t *data) {
    uint32_t value = 0;
    CopyMemory(&value, data, sizeof(value));
    return value;
}

static int32_t read_i32_le(const uint8_t *data) {
    int32_t value = 0;
    CopyMemory(&value, data, sizeof(value));
    return value;
}

static void write_i32_le(uint8_t *data, int32_t value) {
    CopyMemory(data, &value, sizeof(value));
}

static bool ds2_summary_steamid_is_lower_hex_at(const uint8_t *summary_plaintext, size_t offset) {
    if (!summary_plaintext) {
        return false;
    }

    for (size_t i = 0; i < DS2_STEAMID_TEXT_LENGTH; i++) {
        uint8_t ch = summary_plaintext[offset + i];
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) {
            continue;
        }
        return false;
    }
    return true;
}

static bool ds2_summary_steamid_is_lower_hex(const uint8_t *summary_plaintext) {
    return ds2_summary_steamid_is_lower_hex_at(summary_plaintext, DS2_SUMMARY_STEAMID_OFFSET);
}

static bool ds2_profile_is_available(const uint8_t *profile_base) {
    if (!profile_base) {
        return false;
    }

    int32_t flag = read_i32_le(profile_base + DS2_PROFILE_AVAILABLE_FLAG_OFFSET);
    return flag == 1;
}

static bool ds2_aes_open(BCRYPT_ALG_HANDLE *out_alg, BCRYPT_KEY_HANDLE *out_key, uint8_t **out_key_obj, ULONG *out_key_obj_size) {
    if (!out_alg || !out_key || !out_key_obj || !out_key_obj_size) {
        return false;
    }

    *out_alg = NULL;
    *out_key = NULL;
    *out_key_obj = NULL;
    *out_key_obj_size = 0;

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    ULONG key_obj_size = 0;
    ULONG result = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        return false;
    }

    status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(status)) {
        ds2_aes_close(alg, key, NULL);
        return false;
    }

    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&key_obj_size, sizeof(ULONG), &result, 0);
    if (!NT_SUCCESS(status) || key_obj_size == 0) {
        ds2_aes_close(alg, key, NULL);
        return false;
    }

    uint8_t *key_obj = LocalAlloc(LMEM_FIXED, key_obj_size);
    if (!key_obj) {
        ds2_aes_close(alg, key, NULL);
        return false;
    }

    status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size, (PUCHAR)DS2_AES_KEY, sizeof(DS2_AES_KEY), 0);
    if (!NT_SUCCESS(status)) {
        ds2_aes_close(alg, key, key_obj);
        return false;
    }

    *out_alg = alg;
    *out_key = key;
    *out_key_obj = key_obj;
    *out_key_obj_size = key_obj_size;
    return true;
}

static void ds2_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key, uint8_t *key_obj) {
    if (key) {
        BCryptDestroyKey(key);
    }
    if (key_obj) {
        LocalFree(key_obj);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
}

static bool ds2_aes_decrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv16, const uint8_t *ct, ULONG ct_len, uint8_t *out_pt, ULONG out_capacity, ULONG *out_pt_len) {
    if (!key || !iv16 || !ct || !out_pt || !out_pt_len) {
        return false;
    }

    uint8_t iv_copy[16];
    CopyMemory(iv_copy, iv16, sizeof(iv_copy));
    NTSTATUS status = BCryptDecrypt(key, (PUCHAR)ct, ct_len, NULL, iv_copy, sizeof(iv_copy), out_pt, out_capacity, out_pt_len, 0);
    return NT_SUCCESS(status);
}

static bool ds2_aes_encrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv16, const uint8_t *pt, ULONG pt_len, uint8_t *out_ct, ULONG out_capacity, ULONG *out_ct_len) {
    if (!key || !iv16 || !pt || !out_ct || !out_ct_len) {
        return false;
    }

    uint8_t iv_copy[16];
    CopyMemory(iv_copy, iv16, sizeof(iv_copy));
    NTSTATUS status = BCryptEncrypt(key, (PUCHAR)pt, pt_len, NULL, iv_copy, sizeof(iv_copy), out_ct, out_capacity, out_ct_len, 0);
    return NT_SUCCESS(status);
}

static void ds2_md5_iv_ct(const uint8_t *iv16, const uint8_t *ct, size_t ct_len, uint8_t out_md5[16]) {
    uint8_t *tmp = LocalAlloc(LMEM_FIXED, 16 + ct_len);
    if (!tmp) {
        ZeroMemory(out_md5, 16);
        return;
    }

    CopyMemory(tmp, iv16, 16);
    CopyMemory(tmp + 16, ct, ct_len);
    md5_buffer(tmp, 16 + ct_len, out_md5);
    LocalFree(tmp);
}

static bool ds2_write_at(HANDLE file, uint32_t offset, const uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) return false;
    DWORD written;
    if (!WriteFile(file, buf, len, &written, NULL) || written != len) return false;
    return true;
}

static bool ds2_read_exact_at(HANDLE file, uint32_t offset, uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) {
        return false;
    }
    DWORD bytes_read;
    return ReadFile(file, buf, len, &bytes_read, NULL) && bytes_read == len;
}

static bool ds2_read_decrypt_slot(HANDLE file, BCRYPT_KEY_HANDLE key, uint32_t slot_offset, uint8_t *ciphertext, ULONG ciphertext_size, uint8_t *out_plaintext, ULONG plaintext_size) {
    uint8_t stored_md5[16];
    uint8_t iv[16];
    if (!ds2_read_exact_at(file, slot_offset, stored_md5, sizeof(stored_md5))) {
        return false;
    }
    if (!ds2_read_exact_at(file, slot_offset + 16, iv, sizeof(iv))) {
        return false;
    }
    if (!ds2_read_exact_at(file, slot_offset + DS2_SLOT_FILE_HEADER_SIZE, ciphertext, ciphertext_size)) {
        return false;
    }

    ULONG plaintext_len = 0;
    if (!ds2_aes_decrypt(key, iv, ciphertext, ciphertext_size, out_plaintext, plaintext_size, &plaintext_len)) {
        return false;
    }
    return plaintext_len == plaintext_size;
}

static bool ds2_write_encrypted_slot(HANDLE file,
                                     BCRYPT_KEY_HANDLE key,
                                     uint32_t slot_offset,
                                     uint32_t slot_file_size,
                                     const uint8_t *plaintext,
                                     ULONG plaintext_size,
                                     uint8_t *ciphertext,
                                     ULONG ciphertext_capacity) {
    ULONG ciphertext_size = slot_file_size - DS2_SLOT_FILE_HEADER_SIZE;
    if (!file || !key || !plaintext || !ciphertext || plaintext_size != ciphertext_size || ciphertext_capacity < ciphertext_size) {
        return false;
    }

    uint8_t existing_iv[16];
    if (!ds2_read_exact_at(file, slot_offset + 16, existing_iv, sizeof(existing_iv))) {
        return false;
    }

    ULONG ciphertext_len = 0;
    if (!ds2_aes_encrypt(key, existing_iv, plaintext, plaintext_size, ciphertext, ciphertext_capacity, &ciphertext_len) || ciphertext_len != ciphertext_size) {
        return false;
    }

    uint8_t md5[16];
    ds2_md5_iv_ct(existing_iv, ciphertext, ciphertext_len, md5);
    if (!ds2_write_at(file, slot_offset, md5, sizeof(md5))
        || !ds2_write_at(file, slot_offset + 16, existing_iv, sizeof(existing_iv))
        || !ds2_write_at(file, slot_offset + DS2_SLOT_FILE_HEADER_SIZE, ciphertext, ciphertext_len)) {
        return false;
    }

    return true;
}

bool ds2_save_data_load(const wchar_t *path, ds2_save_data_t **out_save) {
    if (!path_fits_fixed_buffer(path) || !out_save) {
        return false;
    }
    *out_save = NULL;

    ds2_save_data_t *save = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(ds2_save_data_t));
    if (!save) {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(save);
        return false;
    }

    uint8_t header[DS2_HEADER_TABLE_SIZE];
    DWORD bytes_read;
    if (!ReadFile(file, header, (DWORD)sizeof(header), &bytes_read, NULL) || bytes_read != sizeof(header)) {
        LocalFree(save);
        CloseHandle(file);
        return false;
    }
    if (RtlCompareMemory(header, "BND4", 4) < 4) {
        LocalFree(save);
        CloseHandle(file);
        return false;
    }

    uint32_t save_slot_count = read_u32_le(header + DS2_HEADER_SLOT_COUNT_OFFSET);
    if (save_slot_count != DS2_BND4_TOTAL_ENTRIES) {
        LocalFree(save);
        CloseHandle(file);
        return false;
    }

    for (int i = 0; i < DS2_BND4_TOTAL_ENTRIES; i++) {
        save->slot_size[i] = read_u32_le(header + DS2_HEADER_SLOT_SIZE_BASE + i * DS2_HEADER_SLOT_STRIDE);
        save->slot_offset[i] = read_u32_le(header + DS2_HEADER_SLOT_OFFSET_BASE + i * DS2_HEADER_SLOT_STRIDE);
    }

    if (save->slot_size[DS2_SUMMARY_INDEX] != DS2_SUMMARY_SLOT_FILE_SIZE) {
        LocalFree(save);
        CloseHandle(file);
        return false;
    }
    for (int i = 0; i < DS2_SLOT_COUNT; i++) {
        if (save->slot_size[DS2_CHAR_A_BASE_INDEX + i] != DS2_CHAR_A_SLOT_FILE_SIZE
            || save->slot_size[DS2_CHAR_B_BASE_INDEX + i] != DS2_CHAR_B_SLOT_FILE_SIZE) {
            LocalFree(save);
            CloseHandle(file);
            return false;
        }
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    if (!ds2_aes_open(&alg, &key, &key_obj, &key_obj_size)) {
        LocalFree(save);
        CloseHandle(file);
        return false;
    }

    ULONG max_ciphertext_size = DS2_CHAR_B_SLOT_FILE_SIZE - DS2_SLOT_FILE_HEADER_SIZE;
    uint8_t *ciphertext = LocalAlloc(LMEM_FIXED, max_ciphertext_size);
    if (!ciphertext) {
        ds2_aes_close(alg, key, key_obj);
        LocalFree(save);
        CloseHandle(file);
        return false;
    }

    if (!ds2_read_decrypt_slot(file, key, save->slot_offset[DS2_SUMMARY_INDEX], ciphertext, DS2_SUMMARY_SLOT_FILE_SIZE - DS2_SLOT_FILE_HEADER_SIZE, save->summary_plaintext, DS2_SUMMARY_PLAINTEXT_SIZE)) {
        LocalFree(ciphertext);
        ds2_aes_close(alg, key, key_obj);
        LocalFree(save);
        CloseHandle(file);
        return false;
    }
    if (!ds2_summary_steamid_is_lower_hex(save->summary_plaintext)) {
        LocalFree(ciphertext);
        ds2_aes_close(alg, key, key_obj);
        LocalFree(save);
        CloseHandle(file);
        return false;
    }

    for (int i = 0; i < DS2_SLOT_COUNT; i++) {
        uint8_t *profile_base = save->summary_plaintext + DS2_SUMMARY_PROFILE_OFFSET + i * DS2_PROFILE_SIZE;
        bool available = ds2_profile_is_available(profile_base);
        save->chars[i].available = available;
        if (available) {
            if (!ds2_read_decrypt_slot(file, key, save->slot_offset[DS2_CHAR_A_BASE_INDEX + i], ciphertext, DS2_CHAR_A_SLOT_FILE_SIZE - DS2_SLOT_FILE_HEADER_SIZE, save->chars[i].part_a, DS2_CHAR_A_PLAINTEXT_SIZE)) {
                LocalFree(ciphertext);
                ds2_aes_close(alg, key, key_obj);
                LocalFree(save);
                CloseHandle(file);
                return false;
            }
            if (!ds2_read_decrypt_slot(file, key, save->slot_offset[DS2_CHAR_B_BASE_INDEX + i], ciphertext, DS2_CHAR_B_SLOT_FILE_SIZE - DS2_SLOT_FILE_HEADER_SIZE, save->chars[i].part_b, DS2_CHAR_B_PLAINTEXT_SIZE)) {
                LocalFree(ciphertext);
                ds2_aes_close(alg, key, key_obj);
                LocalFree(save);
                CloseHandle(file);
                return false;
            }
            CopyMemory(save->chars[i].profile, profile_base, DS2_PROFILE_SIZE);
        }
    }

    LocalFree(ciphertext);
    ds2_aes_close(alg, key, key_obj);
    CloseHandle(file);
    lstrcpyW(save->path, path);
    *out_save = save;
    return true;
}

void ds2_save_data_free(ds2_save_data_t *save) {
    if (save) {
        LocalFree(save);
    }
}

bool ds2_save_get_active_slot(const ds2_save_data_t *save, int *out_slot) {
    if (!save || !out_slot) {
        return false;
    }

    int32_t active;
    CopyMemory(&active, save->summary_plaintext + DS2_SUMMARY_ACTIVE_OFFSET, sizeof(int32_t));
    if (active < 0 || active >= DS2_SLOT_COUNT) {
        return false;
    }

    *out_slot = (int)active;
    return true;
}

ds2_char_data_t *ds2_char_data_ref(ds2_save_data_t *save, int slot) {
    if (!save || slot < 0 || slot >= DS2_SLOT_COUNT) {
        return NULL;
    }
    if (!save->chars[slot].available) {
        return NULL;
    }
    return &save->chars[slot];
}

bool ds2_char_data_serialize(const ds2_char_data_t *char_data, uint8_t *out_buf, size_t buf_size) {
    if (!char_data || !out_buf || buf_size < DS2_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    CopyMemory(out_buf, char_data->part_a, DS2_CHAR_A_SERIALIZED_SIZE);
    CopyMemory(out_buf + DS2_CHAR_A_SERIALIZED_SIZE, char_data->part_b, DS2_CHAR_B_SERIALIZED_SIZE);
    CopyMemory(out_buf + DS2_CHAR_A_SERIALIZED_SIZE + DS2_CHAR_B_SERIALIZED_SIZE, char_data->profile, DS2_PROFILE_SIZE);
    return true;
}

bool ds2_char_data_import_raw(ds2_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size) {
    if (!save || slot < 0 || slot >= DS2_SLOT_COUNT || !raw_data || raw_size != DS2_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    const uint8_t *raw_part_a = raw_data;
    const uint8_t *raw_part_b = raw_data + DS2_CHAR_A_SERIALIZED_SIZE;
    const uint8_t *raw_profile = raw_data + DS2_CHAR_A_SERIALIZED_SIZE + DS2_CHAR_B_SERIALIZED_SIZE;
    uint8_t *summary_profile = save->summary_plaintext + DS2_SUMMARY_PROFILE_OFFSET + slot * DS2_PROFILE_SIZE;

    CopyMemory(save->chars[slot].part_a, raw_part_a, DS2_CHAR_A_SERIALIZED_SIZE);
    CopyMemory(save->chars[slot].part_b, raw_part_b, DS2_CHAR_B_SERIALIZED_SIZE);
    CopyMemory(save->chars[slot].profile, raw_profile, DS2_PROFILE_SIZE);
    CopyMemory(summary_profile, raw_profile, DS2_PROFILE_SIZE);
    write_i32_le(summary_profile + DS2_PROFILE_AVAILABLE_FLAG_OFFSET, 1);
    write_i32_le(save->chars[slot].profile + DS2_PROFILE_AVAILABLE_FLAG_OFFSET, 1);
    save->chars[slot].available = true;

#if DS2_PROFILE_EMBEDDED_STEAMID_OFFSET != -1
    CopyMemory(summary_profile + DS2_PROFILE_EMBEDDED_STEAMID_OFFSET, save->summary_plaintext + DS2_SUMMARY_STEAMID_OFFSET, DS2_STEAMID_TEXT_LENGTH);
    CopyMemory(save->chars[slot].profile + DS2_PROFILE_EMBEDDED_STEAMID_OFFSET, save->summary_plaintext + DS2_SUMMARY_STEAMID_OFFSET, DS2_STEAMID_TEXT_LENGTH);
#endif

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    if (!ds2_aes_open(&alg, &key, &key_obj, &key_obj_size)) {
        return false;
    }

    HANDLE file = CreateFileW(save->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ds2_aes_close(alg, key, key_obj);
        return false;
    }

    ULONG ciphertext_capacity = DS2_CHAR_B_SLOT_FILE_SIZE - DS2_SLOT_FILE_HEADER_SIZE;
    uint8_t *ciphertext = LocalAlloc(LMEM_FIXED, ciphertext_capacity);
    if (!ciphertext) {
        CloseHandle(file);
        ds2_aes_close(alg, key, key_obj);
        return false;
    }

    if (!ds2_write_encrypted_slot(file, key, save->slot_offset[DS2_SUMMARY_INDEX], save->slot_size[DS2_SUMMARY_INDEX], save->summary_plaintext, DS2_SUMMARY_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity)
        || !ds2_write_encrypted_slot(file, key, save->slot_offset[DS2_CHAR_A_BASE_INDEX + slot], save->slot_size[DS2_CHAR_A_BASE_INDEX + slot], save->chars[slot].part_a, DS2_CHAR_A_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity)
        || !ds2_write_encrypted_slot(file, key, save->slot_offset[DS2_CHAR_B_BASE_INDEX + slot], save->slot_size[DS2_CHAR_B_BASE_INDEX + slot], save->chars[slot].part_b, DS2_CHAR_B_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity)) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds2_aes_close(alg, key, key_obj);
        return false;
    }

    LocalFree(ciphertext);
    CloseHandle(file);
    ds2_aes_close(alg, key, key_obj);
    return true;
}
