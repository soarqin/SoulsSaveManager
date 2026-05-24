/**
 * @file sekirosave.c
 * @brief Sekiro: Shadows Die Twice save file format module.
 * @details Implements load/free/serialize/import for Sekiro S0000.sl2 save files.
 *          Key differences from DS3:
 *          (1) NO AES encryption — slot layout is [16-byte MD5][raw plaintext]
 *          (2) MD5 is over raw plaintext only (NOT over IV||ciphertext like DS3)
 *          (3) Variable char Steam ID offset: uint32@0x0C gives N; Steam ID at N+0x44
 *          (4) 12 BND4 entries: 10 char + 1 summary + 1 regulation (entry 11 MUST NOT be touched)
 *          (5) Availability: summary[0xD4 + i] != 0 (1 byte per slot)
 *          (6) Active slot: int32 at summary offset 0x2508
 */

#include "sekirosave.h"
#include <md5.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <windows.h>
/* NOTE: No encryption header is included — Sekiro is unencrypted. */

#define SEKIRO_SLOT_COUNT           10
#define SEKIRO_SUMMARY_INDEX        10
#define SEKIRO_REGULATION_INDEX     11   /* MUST NOT touch as data */
#define SEKIRO_BND4_MIN_ENTRIES     12   /* 10 char + 1 summary + 1 regulation */

/* Slot on-disk sizes (empirical, T1) */
#define SEKIRO_CHAR_SLOT_FILE_SIZE      0x100010u
#define SEKIRO_SUMMARY_SLOT_FILE_SIZE   0x60010u

/* Plaintext sizes (file_size - 16 for unencrypted: [MD5][plaintext]) */
#define SEKIRO_CHAR_PLAINTEXT_SIZE      0x100000u
#define SEKIRO_SUMMARY_PLAINTEXT_SIZE   0x60000u

/* Profile */
#define SEKIRO_PROFILE_SIZE             0x218u

/* Summary offsets */
#define SEKIRO_SUMMARY_STEAMID_OFFSET   0x24u    /* uint64 LE */
#define SEKIRO_SUMMARY_ACTIVE_OFFSET    0x2508u  /* int32 LE */
#define SEKIRO_SUMMARY_AVAILABLE_OFFSET 0xD4u    /* 10 bytes, 1 per slot */
#define SEKIRO_SUMMARY_PROFILE_OFFSET   0x104u

/* Char Steam ID formula: steam_id @ (uint32@0x0C + 0x44) */
#define SEKIRO_CHAR_USERID_LEN_OFFSET   0x0Cu
#define SEKIRO_CHAR_USERID_DELTA        0x44u

#define SEKIRO_SLOT_HEADER_SIZE         0x10u
#define SEKIRO_FILE_HEADER_SIZE         0x300u
#define SEKIRO_HEADER_SLOT_COUNT_OFFSET 0x0Cu
#define SEKIRO_HEADER_SLOT_SIZE_BASE    0x48u
#define SEKIRO_HEADER_SLOT_OFFSET_BASE  0x50u
#define SEKIRO_HEADER_SLOT_STRIDE       0x20u

typedef struct sekiro_char_data_s {
    uint8_t plaintext[SEKIRO_CHAR_PLAINTEXT_SIZE];
    uint8_t profile[SEKIRO_PROFILE_SIZE];
    bool    available;
    uint32_t userid_n;   /* cached N value from uint32@0x0C for Steam ID offset */
} sekiro_char_data_t;

typedef struct sekiro_save_data_s {
    wchar_t              path[MAX_PATH];
    uint8_t              summary_plaintext[SEKIRO_SUMMARY_PLAINTEXT_SIZE];
    sekiro_char_data_t   chars[SEKIRO_SLOT_COUNT];
    uint32_t             slot_offset[SEKIRO_BND4_MIN_ENTRIES];
    uint32_t             slot_size[SEKIRO_BND4_MIN_ENTRIES];
} sekiro_save_data_t;

static bool path_fits_fixed_buffer(const wchar_t *path) {
    return path && (size_t)lstrlenW(path) < MAX_PATH;
}

static uint32_t read_uint32_le(const uint8_t *data) {
    uint32_t value;
    CopyMemory(&value, data, sizeof(value));
    return value;
}

static uint64_t read_uint64_le(const uint8_t *data) {
    uint64_t value;
    CopyMemory(&value, data, sizeof(value));
    return value;
}

static void write_uint64_le(uint8_t *data, uint64_t value) {
    CopyMemory(data, &value, sizeof(value));
}

/* Compute MD5 over raw plaintext (no IV, no ciphertext — Sekiro is unencrypted).
 * Uses the md5 library (same as ersave.c). */
static bool sekiro_md5_data(const uint8_t *data, uint32_t data_size, uint8_t out_md5[16]) {
    if (!data || !out_md5) {
        return false;
    }

    md5_buffer(data, data_size, out_md5);
    return true;
}

static bool range_fits(uint32_t file_size, uint32_t offset, uint32_t size) {
    return offset <= file_size && size <= file_size - offset;
}

static bool slot_payload_size_matches(uint32_t slot_size, uint32_t plaintext_size) {
    return slot_size == plaintext_size + SEKIRO_SLOT_HEADER_SIZE;
}

static bool write_at(HANDLE file, uint32_t offset, const void *data, DWORD size) {
    if (SetFilePointer(file, offset, NULL, FILE_BEGIN) != offset) return false;
    DWORD written;
    if (!WriteFile(file, data, size, &written, NULL) || written != size) return false;
    return true;
}

static bool read_file_to_memory(const wchar_t *path, uint8_t **out_data, uint32_t *out_size) {
    if (!path || !out_data || !out_size) {
        return false;
    }

    *out_data = NULL;
    *out_size = 0;

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size_li;
    if (!GetFileSizeEx(file, &file_size_li) || file_size_li.QuadPart <= 0 || file_size_li.QuadPart > UINT32_MAX) {
        CloseHandle(file);
        return false;
    }

    uint32_t file_size = (uint32_t)file_size_li.QuadPart;
    uint8_t *data = LocalAlloc(LMEM_FIXED, file_size);
    if (!data) {
        CloseHandle(file);
        return false;
    }

    DWORD bytes_read;
    if (!ReadFile(file, data, file_size, &bytes_read, NULL) || bytes_read != file_size) {
        LocalFree(data);
        CloseHandle(file);
        return false;
    }

    CloseHandle(file);
    *out_data = data;
    *out_size = file_size;
    return true;
}

static bool parse_slot_table(sekiro_save_data_t *save, const uint8_t *file_data, uint32_t file_size) {
    if (!save || !file_data || file_size < SEKIRO_FILE_HEADER_SIZE) {
        return false;
    }
    if (RtlCompareMemory(file_data, "BND4", 4) != 4) {
        return false;
    }

    uint32_t slot_count = read_uint32_le(file_data + SEKIRO_HEADER_SLOT_COUNT_OFFSET);
    if (slot_count < SEKIRO_BND4_MIN_ENTRIES) {
        return false;
    }

    for (int i = 0; i < SEKIRO_BND4_MIN_ENTRIES; i++) {
        uint32_t table_index = (uint32_t)i * SEKIRO_HEADER_SLOT_STRIDE;
        uint32_t size_offset = SEKIRO_HEADER_SLOT_SIZE_BASE + table_index;
        uint32_t data_offset = SEKIRO_HEADER_SLOT_OFFSET_BASE + table_index;
        if (size_offset + sizeof(uint32_t) > SEKIRO_FILE_HEADER_SIZE
            || data_offset + sizeof(uint32_t) > SEKIRO_FILE_HEADER_SIZE) {
            return false;
        }

        save->slot_size[i] = read_uint32_le(file_data + size_offset);
        save->slot_offset[i] = read_uint32_le(file_data + data_offset);
        if (!range_fits(file_size, save->slot_offset[i], save->slot_size[i])) {
            return false;
        }
    }

    for (int i = 0; i < SEKIRO_SLOT_COUNT; i++) {
        if (save->slot_size[i] != SEKIRO_CHAR_SLOT_FILE_SIZE) {
            return false;
        }
    }
    if (save->slot_size[SEKIRO_SUMMARY_INDEX] != SEKIRO_SUMMARY_SLOT_FILE_SIZE) {
        return false;
    }
    /* Entry 11 is the regulation slot. It is only range-validated above and
     * never copied into a character or summary data buffer. */
    if (save->slot_size[SEKIRO_REGULATION_INDEX] < SEKIRO_SLOT_HEADER_SIZE) {
        return false;
    }

    return true;
}

static bool copy_plaintext_slot(const uint8_t *file_data,
                                uint32_t file_size,
                                uint32_t slot_offset,
                                uint32_t slot_size,
                                uint8_t *out_plaintext,
                                uint32_t plaintext_size) {
    if (!file_data || !out_plaintext) {
        return false;
    }
    if (!slot_payload_size_matches(slot_size, plaintext_size)) {
        return false;
    }
    if (!range_fits(file_size, slot_offset, slot_size)) {
        return false;
    }

    const uint8_t *stored_md5 = file_data + slot_offset;
    const uint8_t *plaintext = stored_md5 + SEKIRO_SLOT_HEADER_SIZE;
    uint8_t computed_md5[16];
    if (!sekiro_md5_data(plaintext, plaintext_size, computed_md5)) {
        return false;
    }
    if (RtlCompareMemory(stored_md5, computed_md5, sizeof(computed_md5)) != sizeof(computed_md5)) {
        return false;
    }

    CopyMemory(out_plaintext, plaintext, plaintext_size);
    return true;
}

static bool char_userid_n_from_plaintext(const uint8_t *plaintext, uint32_t *out_n) {
    if (!plaintext || !out_n) {
        return false;
    }

    uint32_t n = read_uint32_le(plaintext + SEKIRO_CHAR_USERID_LEN_OFFSET);
    /* Bounds check N before using N + 0x44 and the 8-byte Steam ID. */
    if (n > SEKIRO_CHAR_PLAINTEXT_SIZE || SEKIRO_CHAR_USERID_DELTA > SEKIRO_CHAR_PLAINTEXT_SIZE - n) {
        return false;
    }
    uint32_t userid_offset = n + SEKIRO_CHAR_USERID_DELTA;
    if (userid_offset > SEKIRO_CHAR_PLAINTEXT_SIZE - sizeof(uint64_t)) {
        return false;
    }

    *out_n = n;
    return true;
}

static bool copy_summary_profile(sekiro_save_data_t *save, int slot) {
    if (!save || slot < 0 || slot >= SEKIRO_SLOT_COUNT) {
        return false;
    }

    uint32_t profile_offset = SEKIRO_SUMMARY_PROFILE_OFFSET + (uint32_t)slot * SEKIRO_PROFILE_SIZE;
    if (profile_offset > SEKIRO_SUMMARY_PLAINTEXT_SIZE - SEKIRO_PROFILE_SIZE) {
        return false;
    }

    CopyMemory(save->chars[slot].profile, save->summary_plaintext + profile_offset, SEKIRO_PROFILE_SIZE);
    return true;
}

static bool load_plaintext_slots(sekiro_save_data_t *save, const uint8_t *file_data, uint32_t file_size) {
    if (!save || !file_data) {
        return false;
    }

    if (!copy_plaintext_slot(file_data,
                             file_size,
                             save->slot_offset[SEKIRO_SUMMARY_INDEX],
                             save->slot_size[SEKIRO_SUMMARY_INDEX],
                             save->summary_plaintext,
                             SEKIRO_SUMMARY_PLAINTEXT_SIZE)) {
        return false;
    }

    for (int i = 0; i < SEKIRO_SLOT_COUNT; i++) {
        if (!copy_summary_profile(save, i)) {
            return false;
        }
        if (save->summary_plaintext[SEKIRO_SUMMARY_AVAILABLE_OFFSET + i] == 0) {
            save->chars[i].available = false;
            continue;
        }

        if (!copy_plaintext_slot(file_data,
                                 file_size,
                                 save->slot_offset[i],
                                 save->slot_size[i],
                                 save->chars[i].plaintext,
                                 SEKIRO_CHAR_PLAINTEXT_SIZE)) {
            return false;
        }

        uint32_t userid_n = 0;
        if (!char_userid_n_from_plaintext(save->chars[i].plaintext, &userid_n)) {
            save->chars[i].available = false;
            continue;
        }
        save->chars[i].userid_n = userid_n;
        save->chars[i].available = true;
    }

    return true;
}

static bool write_plaintext_slot(HANDLE file, uint32_t slot_offset, uint32_t slot_size, const uint8_t *plaintext, uint32_t plaintext_size) {
    if (!file || !plaintext) {
        return false;
    }
    if (!slot_payload_size_matches(slot_size, plaintext_size)) {
        return false;
    }

    uint8_t md5[16];
    if (!sekiro_md5_data(plaintext, plaintext_size, md5)) {
        return false;
    }

    return write_at(file, slot_offset, md5, sizeof(md5))
        && write_at(file, slot_offset + SEKIRO_SLOT_HEADER_SIZE, plaintext, plaintext_size);
}

bool sekiro_save_data_load(const wchar_t *path, sekiro_save_data_t **out_save) {
    if (!path_fits_fixed_buffer(path) || !out_save) {
        return false;
    }

    *out_save = NULL;
    sekiro_save_data_t *save = LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(sekiro_save_data_t));
    if (!save) {
        return false;
    }

    uint8_t *file_data = NULL;
    uint32_t file_size = 0;
    if (!read_file_to_memory(path, &file_data, &file_size)) {
        LocalFree(save);
        return false;
    }

    if (!parse_slot_table(save, file_data, file_size) || !load_plaintext_slots(save, file_data, file_size)) {
        LocalFree(file_data);
        LocalFree(save);
        return false;
    }

    LocalFree(file_data);
    lstrcpyW(save->path, path);
    *out_save = save;
    return true;
}

void sekiro_save_data_free(sekiro_save_data_t *save) {
    if (save) {
        LocalFree(save);
    }
}

bool sekiro_save_get_active_slot(const sekiro_save_data_t *save, int *out_slot) {
    if (!save || !out_slot) {
        return false;
    }

    int32_t active;
    CopyMemory(&active, save->summary_plaintext + SEKIRO_SUMMARY_ACTIVE_OFFSET, sizeof(int32_t));
    if (active < 0 || active >= SEKIRO_SLOT_COUNT) {
        return false;
    }

    *out_slot = (int)active;
    return true;
}

sekiro_char_data_t *sekiro_char_data_ref(sekiro_save_data_t *save, int slot) {
    if (!save || slot < 0 || slot >= SEKIRO_SLOT_COUNT) {
        return NULL;
    }
    if (!save->chars[slot].available) {
        return NULL;
    }
    return &save->chars[slot];
}

bool sekiro_char_data_serialize(const sekiro_char_data_t *char_data, uint8_t *out_buf, size_t buf_size) {
    if (!char_data || !out_buf || buf_size < SEKIRO_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    CopyMemory(out_buf, char_data->plaintext, SEKIRO_CHAR_PLAINTEXT_SIZE);
    CopyMemory(out_buf + SEKIRO_CHAR_PLAINTEXT_SIZE, char_data->profile, SEKIRO_PROFILE_SIZE);
    return true;
}

bool sekiro_char_data_import_raw(sekiro_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size) {
    if (!save || slot < 0 || slot >= SEKIRO_SLOT_COUNT || !raw_data || raw_size != SEKIRO_CHAR_DATA_SERIALIZED_SIZE) {
        return false;
    }

    uint8_t *char_plaintext = LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_PLAINTEXT_SIZE);
    if (!char_plaintext) {
        return false;
    }
    CopyMemory(char_plaintext, raw_data, SEKIRO_CHAR_PLAINTEXT_SIZE);

    uint32_t userid_n = 0;
    if (!char_userid_n_from_plaintext(char_plaintext, &userid_n)) {
        LocalFree(char_plaintext);
        return false;
    }

    uint32_t userid_offset = userid_n + SEKIRO_CHAR_USERID_DELTA;
    uint64_t destination_userid = read_uint64_le(save->summary_plaintext + SEKIRO_SUMMARY_STEAMID_OFFSET);
    write_uint64_le(char_plaintext + userid_offset, destination_userid);

    HANDLE file = CreateFileW(save->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LocalFree(char_plaintext);
        return false;
    }

    const uint8_t *profile = raw_data + SEKIRO_CHAR_PLAINTEXT_SIZE;
    CopyMemory(save->chars[slot].plaintext, char_plaintext, SEKIRO_CHAR_PLAINTEXT_SIZE);
    CopyMemory(save->chars[slot].profile, profile, SEKIRO_PROFILE_SIZE);
    save->chars[slot].available = true;
    save->chars[slot].userid_n = userid_n;

    uint32_t profile_offset = SEKIRO_SUMMARY_PROFILE_OFFSET + (uint32_t)slot * SEKIRO_PROFILE_SIZE;
    CopyMemory(save->summary_plaintext + profile_offset, profile, SEKIRO_PROFILE_SIZE);
    save->summary_plaintext[SEKIRO_SUMMARY_AVAILABLE_OFFSET + slot] = 1;

    bool ok = write_plaintext_slot(file,
                                   save->slot_offset[slot],
                                   save->slot_size[slot],
                                   char_plaintext,
                                   SEKIRO_CHAR_PLAINTEXT_SIZE);
    ok = ok && write_plaintext_slot(file,
                                    save->slot_offset[SEKIRO_SUMMARY_INDEX],
                                    save->slot_size[SEKIRO_SUMMARY_INDEX],
                                    save->summary_plaintext,
                                    SEKIRO_SUMMARY_PLAINTEXT_SIZE);

    CloseHandle(file);
    LocalFree(char_plaintext);
    return ok;
}
