/**
 * @file ds2save.h
 * @brief Header file for Dark Souls II: Scholar of the First Sin save data management functions
 * @details This file contains declarations for functions that load, inspect,
 *          serialize, and import DS2S character save data.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct ds2_save_data_s ds2_save_data_t;
typedef struct ds2_char_data_s ds2_char_data_t;

/* DS2_CHAR_DATA_SERIALIZED_SIZE uses the full plaintext sizes for part A and part B.
 * = 0x1B2C0 + 0x7A8B0 + 0x1F0 = 0x95D60 */
#define DS2_CHAR_DATA_SERIALIZED_SIZE  0x95D60u

bool ds2_save_data_load(const wchar_t *path, ds2_save_data_t **out_save);
void ds2_save_data_free(ds2_save_data_t *save);
bool ds2_save_get_active_slot(const ds2_save_data_t *save, int *out_slot);
ds2_char_data_t *ds2_char_data_ref(ds2_save_data_t *save, int slot);
bool ds2_char_data_serialize(const ds2_char_data_t *char_data, uint8_t *out_buf, size_t buf_size);
bool ds2_char_data_import_raw(ds2_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size);
