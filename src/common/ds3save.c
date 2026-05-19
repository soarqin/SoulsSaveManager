/**
 * @file ds3save.c
 * @brief Implementation of Dark Souls III save data management functions
 * @details This file handles DS3 BND4 save slots, including AES-CBC slot
 *          decryption/encryption and MD5 persistence for imported characters.
 */

#include "ds3save.h"
#include <md5.h>
#include <stdint.h>
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>

/* AES key -- hard-coded inline; do not include ds3_test_format.h. */
static const uint8_t DS3_AES_KEY[16] = {
    0xFD, 0x46, 0x4D, 0x69, 0x5E, 0x69, 0xA3, 0x9A,
    0x10, 0xE3, 0x19, 0xA7, 0xAC, 0xE8, 0xB7, 0xFA
};

#define DS3_SLOT_HEADER_SIZE    32      /* 16-byte MD5 + 16-byte IV */
#define DS3_PROFILE_SIZE        0x22A
#define DS3_PROFILE_OFFSET      0x10A2
#define DS3_AVAILABLE_OFFSET    0x1098
#define DS3_ACTIVE_OFFSET       0x0FE8
#define DS3_SUMMARY_USERID_OFFSET 0x0008
#define DS3_CHAR_USERID_LEN_OFFSET 0x58  /* offset of N in char plaintext */
#define DS3_CHAR_USERID_DELTA   0x6F     /* userid is at N + 0x6F */

/* Derived from real DS3 save (DS30000.sl2). Char and summary slots have
 * DIFFERENT on-disk sizes in real DS3 saves (confirmed via BND4 entry header
 * + AES-128-CBC PKCS7 decrypt of slot index 0 vs slot index 10):
 *   char    slot: 0xC0030 on-disk -> 0xC0004 plaintext (12 bytes PKCS7 pad)
 *   summary slot: 0x60030 on-disk -> 0x60004 plaintext (12 bytes PKCS7 pad)
 * DS3_CHAR_DATA_SERIALIZED_SIZE = DS3_CHAR_PLAINTEXT_SIZE + DS3_PROFILE_SIZE. */
#define DS3_CHAR_PLAINTEXT_SIZE  0xC0004u
#define DS3_SUMMARY_PLAINTEXT_SIZE 0x60004u
/* On-disk slot size = 16(MD5) + 16(IV) + ciphertext */
#define DS3_CHAR_SLOT_FILE_SIZE  0xC0030u
#define DS3_SUMMARY_SLOT_FILE_SIZE 0x60030u

/* BND4 header offsets (same as ER) */
#define DS3_HEADER_SLOT_COUNT_OFFSET  0x0C
#define DS3_HEADER_SLOT_SIZE_BASE     0x48
#define DS3_HEADER_SLOT_OFFSET_BASE   0x50
#define DS3_HEADER_SLOT_STRIDE        0x20
#define DS3_FILE_HEADER_SIZE          0x300

typedef struct ds3_summary_data_s {
    uint8_t data[DS3_SUMMARY_PLAINTEXT_SIZE];
    uint32_t slot_offset;   /* offset of summary slot in file */
    uint32_t active_offset; /* = DS3_ACTIVE_OFFSET (fixed) */
    uint32_t available_offset; /* = DS3_AVAILABLE_OFFSET (fixed) */
    uint32_t profile_offset;   /* = DS3_PROFILE_OFFSET (fixed) */
} ds3_summary_data_t;

typedef struct ds3_char_data_s {
    uint8_t data[DS3_CHAR_PLAINTEXT_SIZE];
    uint32_t slot_offset;   /* offset of char slot in file */
    uint32_t userid_offset; /* = N + DS3_CHAR_USERID_DELTA where N = *(uint32_t*)(data+0x58) */
    uint8_t profile[DS3_PROFILE_SIZE];
} ds3_char_data_t;

typedef struct ds3_save_data_s {
    wchar_t full_path[MAX_PATH];
    ds3_char_data_t char_data[10];
    ds3_summary_data_t summary_data;
} ds3_save_data_t;

static bool path_fits_fixed_buffer(const wchar_t *path) {
    return path && (size_t)lstrlenW(path) < MAX_PATH;
}

static void ds3_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key, uint8_t *key_obj);

static bool ds3_userid_offset_from_plaintext(const uint8_t *data, uint32_t *out_offset) {
    if (!data || !out_offset) {
        return false;
    }

    uint32_t n = *(const uint32_t *)(data + DS3_CHAR_USERID_LEN_OFFSET);
    if (n > DS3_CHAR_PLAINTEXT_SIZE || DS3_CHAR_USERID_DELTA > DS3_CHAR_PLAINTEXT_SIZE - n) {
        return false;
    }
    uint32_t offset = n + DS3_CHAR_USERID_DELTA;
    if (offset > DS3_CHAR_PLAINTEXT_SIZE - sizeof(uint64_t)) {
        return false;
    }

    *out_offset = offset;
    return true;
}

static bool ds3_aes_open(BCRYPT_ALG_HANDLE *out_alg, BCRYPT_KEY_HANDLE *out_key, uint8_t **out_key_obj, ULONG *out_key_obj_size) {
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
        ds3_aes_close(alg, key, NULL);
        return false;
    }

    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&key_obj_size, sizeof(ULONG), &result, 0);
    if (!NT_SUCCESS(status) || key_obj_size == 0) {
        ds3_aes_close(alg, key, NULL);
        return false;
    }

    uint8_t *key_obj = LocalAlloc(LMEM_FIXED, key_obj_size);
    if (!key_obj) {
        ds3_aes_close(alg, key, NULL);
        return false;
    }

    status = BCryptGenerateSymmetricKey(alg, &key, key_obj, key_obj_size, (PUCHAR)DS3_AES_KEY, sizeof(DS3_AES_KEY), 0);
    if (!NT_SUCCESS(status)) {
        ds3_aes_close(alg, key, key_obj);
        return false;
    }

    *out_alg = alg;
    *out_key = key;
    *out_key_obj = key_obj;
    *out_key_obj_size = key_obj_size;
    return true;
}

static void ds3_aes_close(BCRYPT_ALG_HANDLE alg, BCRYPT_KEY_HANDLE key, uint8_t *key_obj) {
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

static bool ds3_aes_decrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv16, const uint8_t *ct, ULONG ct_len, uint8_t *out_pt, ULONG out_capacity, ULONG *out_pt_len) {
    if (!key || !iv16 || !ct || !out_pt || !out_pt_len) {
        return false;
    }

    uint8_t iv_copy[16];
    CopyMemory(iv_copy, iv16, sizeof(iv_copy));
    NTSTATUS status = BCryptDecrypt(key, (PUCHAR)ct, ct_len, NULL, iv_copy, sizeof(iv_copy), out_pt, out_capacity, out_pt_len, BCRYPT_BLOCK_PADDING);
    return NT_SUCCESS(status);
}

static bool ds3_aes_encrypt(BCRYPT_KEY_HANDLE key, const uint8_t *iv16, const uint8_t *pt, ULONG pt_len, uint8_t *out_ct, ULONG out_capacity, ULONG *out_ct_len) {
    if (!key || !iv16 || !pt || !out_ct || !out_ct_len) {
        return false;
    }

    uint8_t iv_copy[16];
    CopyMemory(iv_copy, iv16, sizeof(iv_copy));
    NTSTATUS status = BCryptEncrypt(key, (PUCHAR)pt, pt_len, NULL, iv_copy, sizeof(iv_copy), out_ct, out_capacity, out_ct_len, BCRYPT_BLOCK_PADDING);
    return NT_SUCCESS(status);
}

static void ds3_md5_iv_ct(const uint8_t *iv16, const uint8_t *ct, size_t ct_len, uint8_t out_md5[16]) {
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

static bool ds3_write_at(HANDLE file, uint32_t offset, const uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) return false;
    DWORD written;
    if (!WriteFile(file, buf, len, &written, NULL) || written != len) return false;
    return true;
}

static bool ds3_read_exact_at(HANDLE file, uint32_t offset, uint8_t *buf, DWORD len) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) {
        return false;
    }
    DWORD bytes_read;
    return ReadFile(file, buf, len, &bytes_read, NULL) && bytes_read == len;
}

static bool ds3_read_decrypt_slot(HANDLE file, BCRYPT_KEY_HANDLE key, uint32_t slot_offset, uint8_t *ciphertext, ULONG ciphertext_size, uint8_t *out_plaintext, ULONG plaintext_size) {
    uint8_t stored_md5[16];
    uint8_t iv[16];
    if (!ds3_read_exact_at(file, slot_offset, stored_md5, sizeof(stored_md5))) {
        return false;
    }
    if (!ds3_read_exact_at(file, slot_offset + 16, iv, sizeof(iv))) {
        return false;
    }
    if (!ds3_read_exact_at(file, slot_offset + DS3_SLOT_HEADER_SIZE, ciphertext, ciphertext_size)) {
        return false;
    }

    ULONG plaintext_len = 0;
    if (!ds3_aes_decrypt(key, iv, ciphertext, ciphertext_size, out_plaintext, plaintext_size, &plaintext_len)) {
        return false;
    }
    return plaintext_len == plaintext_size;
}

ds3_save_data_t * ds3_save_data_load(const wchar_t *path) {
    if (!path_fits_fixed_buffer(path)) {
        return NULL;
    }

    ds3_save_data_t *save_data = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(ds3_save_data_t));
    if (!save_data) {
        return NULL;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(save_data);
        return NULL;
    }

    DWORD bytes_read;
    uint8_t sig[4];
    if (!ReadFile(file, sig, 4, &bytes_read, NULL) || bytes_read != 4 || RtlCompareMemory(sig, "BND4", 4) < 4) {
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }

    uint8_t header[DS3_FILE_HEADER_SIZE];
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!ReadFile(file, header, sizeof(header), &bytes_read, NULL) || bytes_read != sizeof(header)) {
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }

    int save_slot_count = *(int *)&header[DS3_HEADER_SLOT_COUNT_OFFSET];
    if (save_slot_count < 12) {
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }

    for (int i = 0; i < 10; i++) {
        uint32_t save_slot_size = *(uint32_t *)(&header[DS3_HEADER_SLOT_SIZE_BASE + i * DS3_HEADER_SLOT_STRIDE]);
        if (save_slot_size != DS3_CHAR_SLOT_FILE_SIZE) {
            LocalFree(save_data);
            CloseHandle(file);
            return NULL;
        }
        save_data->char_data[i].slot_offset = *(uint32_t *)(&header[DS3_HEADER_SLOT_OFFSET_BASE + i * DS3_HEADER_SLOT_STRIDE]);
    }

    uint32_t summary_slot_size = *(uint32_t *)(&header[DS3_HEADER_SLOT_SIZE_BASE + 10 * DS3_HEADER_SLOT_STRIDE]);
    if (summary_slot_size != DS3_SUMMARY_SLOT_FILE_SIZE) {
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }
    save_data->summary_data.slot_offset = *(uint32_t *)(&header[DS3_HEADER_SLOT_OFFSET_BASE + 10 * DS3_HEADER_SLOT_STRIDE]);

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    if (!ds3_aes_open(&alg, &key, &key_obj, &key_obj_size)) {
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }

    /* Buffer must fit the LARGER of char/summary ciphertexts (char in real DS3). */
    ULONG char_ciphertext_size = DS3_CHAR_SLOT_FILE_SIZE - DS3_SLOT_HEADER_SIZE;
    ULONG summary_ciphertext_size = DS3_SUMMARY_SLOT_FILE_SIZE - DS3_SLOT_HEADER_SIZE;
    ULONG max_ciphertext_size = char_ciphertext_size > summary_ciphertext_size
        ? char_ciphertext_size : summary_ciphertext_size;
    uint8_t *ciphertext = LocalAlloc(LMEM_FIXED, max_ciphertext_size);
    if (!ciphertext) {
        ds3_aes_close(alg, key, key_obj);
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }

    if (!ds3_read_decrypt_slot(file, key, save_data->summary_data.slot_offset, ciphertext, summary_ciphertext_size, save_data->summary_data.data, DS3_SUMMARY_PLAINTEXT_SIZE)) {
        LocalFree(ciphertext);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(save_data);
        CloseHandle(file);
        return NULL;
    }
    save_data->summary_data.active_offset = DS3_ACTIVE_OFFSET;
    save_data->summary_data.available_offset = DS3_AVAILABLE_OFFSET;
    save_data->summary_data.profile_offset = DS3_PROFILE_OFFSET;

    for (int i = 0; i < 10; i++) {
        if (save_data->summary_data.data[DS3_AVAILABLE_OFFSET + i] != 0) {
            if (!ds3_read_decrypt_slot(file, key, save_data->char_data[i].slot_offset, ciphertext, char_ciphertext_size, save_data->char_data[i].data, DS3_CHAR_PLAINTEXT_SIZE)) {
                LocalFree(ciphertext);
                ds3_aes_close(alg, key, key_obj);
                LocalFree(save_data);
                CloseHandle(file);
                return NULL;
            }
            if (!ds3_userid_offset_from_plaintext(save_data->char_data[i].data, &save_data->char_data[i].userid_offset)) {
                LocalFree(ciphertext);
                ds3_aes_close(alg, key, key_obj);
                LocalFree(save_data);
                CloseHandle(file);
                return NULL;
            }
            CopyMemory(save_data->char_data[i].profile, save_data->summary_data.data + DS3_PROFILE_OFFSET + DS3_PROFILE_SIZE * i, DS3_PROFILE_SIZE);
        }
    }

    LocalFree(ciphertext);
    ds3_aes_close(alg, key, key_obj);
    CloseHandle(file);
    lstrcpyW(save_data->full_path, path);
    return save_data;
}

void ds3_save_data_free(ds3_save_data_t *save_data) {
    if (save_data) {
        LocalFree(save_data);
    }
}

bool ds3_save_get_active_slot(const ds3_save_data_t *save_data, int *out_slot) {
    if (!save_data || !out_slot) {
        return false;
    }

    int slot = *(const int32_t *)(save_data->summary_data.data + DS3_ACTIVE_OFFSET);
    if (slot < 0 || slot > 9) {
        return false;
    }

    *out_slot = slot;
    return true;
}

const ds3_char_data_t * ds3_char_data_ref(const ds3_save_data_t *save_data, int slot) {
    if (!save_data || slot < 0 || slot >= 10) {
        return NULL;
    }
    if (save_data->summary_data.data[DS3_AVAILABLE_OFFSET + slot] == 0) {
        return NULL;
    }
    return &save_data->char_data[slot];
}

bool ds3_char_data_serialize(const ds3_char_data_t *c, uint8_t *out, size_t out_size) {
    if (!c || !out || out_size < DS3_CHAR_PLAINTEXT_SIZE + DS3_PROFILE_SIZE) {
        return false;
    }

    CopyMemory(out, c->data, DS3_CHAR_PLAINTEXT_SIZE);
    CopyMemory(out + DS3_CHAR_PLAINTEXT_SIZE, c->profile, DS3_PROFILE_SIZE);
    return true;
}

bool ds3_char_data_import_raw(ds3_save_data_t *save_data, int slot, const uint8_t *raw_data) {
    if (!save_data || slot < 0 || slot >= 10 || !raw_data) {
        return false;
    }

    uint32_t userid_offset = 0;
    if (!ds3_userid_offset_from_plaintext(raw_data, &userid_offset)) {
        return false;
    }

    uint8_t *mutable_copy = LocalAlloc(LMEM_FIXED, DS3_CHAR_PLAINTEXT_SIZE);
    if (!mutable_copy) {
        return false;
    }
    CopyMemory(mutable_copy, raw_data, DS3_CHAR_PLAINTEXT_SIZE);

    uint64_t destination_userid = *(uint64_t *)(save_data->summary_data.data + DS3_SUMMARY_USERID_OFFSET);
    /* Implicit Steam ID re-signing: overwrite imported char's userid with destination save's userid (mirrors ersave.c:791-793) */
    *(uint64_t *)(mutable_copy + userid_offset) = destination_userid;

    CopyMemory(save_data->char_data[slot].data, mutable_copy, DS3_CHAR_PLAINTEXT_SIZE);
    save_data->char_data[slot].userid_offset = userid_offset;
    CopyMemory(save_data->char_data[slot].profile, raw_data + DS3_CHAR_PLAINTEXT_SIZE, DS3_PROFILE_SIZE);
    CopyMemory(save_data->summary_data.data + DS3_PROFILE_OFFSET + slot * DS3_PROFILE_SIZE, raw_data + DS3_CHAR_PLAINTEXT_SIZE, DS3_PROFILE_SIZE);
    save_data->summary_data.data[DS3_AVAILABLE_OFFSET + slot] = 1;

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    uint8_t *key_obj = NULL;
    ULONG key_obj_size = 0;
    if (!ds3_aes_open(&alg, &key, &key_obj, &key_obj_size)) {
        LocalFree(mutable_copy);
        return false;
    }

    HANDLE file = CreateFileW(save_data->full_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    ULONG ciphertext_capacity = DS3_CHAR_SLOT_FILE_SIZE - DS3_SLOT_HEADER_SIZE;
    uint8_t *ciphertext = LocalAlloc(LMEM_FIXED, ciphertext_capacity);
    if (!ciphertext) {
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    uint8_t existing_iv[16];
    if (!ds3_read_exact_at(file, save_data->char_data[slot].slot_offset + 16, existing_iv, sizeof(existing_iv))) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    ULONG ciphertext_len = 0;
    if (!ds3_aes_encrypt(key, existing_iv, mutable_copy, DS3_CHAR_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity, &ciphertext_len) || ciphertext_len != ciphertext_capacity) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    uint8_t md5[16];
    ds3_md5_iv_ct(existing_iv, ciphertext, ciphertext_len, md5);
    if (!ds3_write_at(file, save_data->char_data[slot].slot_offset, md5, sizeof(md5))
        || !ds3_write_at(file, save_data->char_data[slot].slot_offset + 16, existing_iv, sizeof(existing_iv))
        || !ds3_write_at(file, save_data->char_data[slot].slot_offset + DS3_SLOT_HEADER_SIZE, ciphertext, ciphertext_len)) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    uint8_t summary_iv[16];
    if (!ds3_read_exact_at(file, save_data->summary_data.slot_offset + 16, summary_iv, sizeof(summary_iv))) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    /* Summary ciphertext capacity differs from char in real DS3 (0x60010 vs 0xC0010). */
    ULONG summary_ct_capacity = DS3_SUMMARY_SLOT_FILE_SIZE - DS3_SLOT_HEADER_SIZE;
    ULONG summary_ciphertext_len = 0;
    if (!ds3_aes_encrypt(key, summary_iv, save_data->summary_data.data, DS3_SUMMARY_PLAINTEXT_SIZE, ciphertext, ciphertext_capacity, &summary_ciphertext_len) || summary_ciphertext_len != summary_ct_capacity) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    ds3_md5_iv_ct(summary_iv, ciphertext, summary_ciphertext_len, md5);
    if (!ds3_write_at(file, save_data->summary_data.slot_offset, md5, sizeof(md5))
        || !ds3_write_at(file, save_data->summary_data.slot_offset + 16, summary_iv, sizeof(summary_iv))
        || !ds3_write_at(file, save_data->summary_data.slot_offset + DS3_SLOT_HEADER_SIZE, ciphertext, summary_ciphertext_len)) {
        LocalFree(ciphertext);
        CloseHandle(file);
        ds3_aes_close(alg, key, key_obj);
        LocalFree(mutable_copy);
        return false;
    }

    LocalFree(ciphertext);
    CloseHandle(file);
    ds3_aes_close(alg, key, key_obj);
    LocalFree(mutable_copy);
    return true;
}
