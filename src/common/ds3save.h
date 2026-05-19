/**
 * @file ds3save.h
 * @brief Header file for Dark Souls III save data management functions
 * @details This file contains declarations for functions that load, inspect,
 *          serialize, and import Dark Souls III character save data.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef struct ds3_save_data_s ds3_save_data_t;
typedef struct ds3_char_data_s ds3_char_data_t;

/* Buffer size required for ds3_char_data_serialize out_size and ds3_char_data_import_raw raw_data.
 * Derived from real DS3 save's BND4 entry size: 0xC0030u (raw on-disk)
 * minus 32 (MD5+IV) minus 12 (PKCS7 padding observed after AES decrypt)
 * = 0xC0004u plaintext, plus 0x22A (profile) = 0xC022Eu.
 * ER's analog is hardcoded 0x28024Cu in er_backend.c. */
#define DS3_CHAR_DATA_SERIALIZED_SIZE  0xC022Eu

/**
 * @brief Loads save data from a file
 * @param path Path to the save file
 * @return Pointer to loaded save data structure, or NULL if loading fails
 */
ds3_save_data_t *ds3_save_data_load(const wchar_t *path);

/**
 * @brief Frees allocated save data memory
 * @param save_data Pointer to save data structure to free
 */
void ds3_save_data_free(ds3_save_data_t *save_data);

/**
 * @brief Gets the active character slot index from save data
 * @param save_data Pointer to save data structure
 * @param out_slot Pointer to receive slot index
 * @return true if the active slot was read successfully, false otherwise
 */
bool ds3_save_get_active_slot(const ds3_save_data_t *save_data, int *out_slot);

/**
 * @brief Gets a reference to a character data structure
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @return Pointer to character data structure, or NULL if slot is invalid
 */
const ds3_char_data_t *ds3_char_data_ref(const ds3_save_data_t *save_data, int slot);

/**
 * @brief Serializes character data to a flat buffer
 * @param c Pointer to character data structure
 * @param out Output buffer
 * @param out_size Buffer size
 * @return true if serialization succeeds, false otherwise
 */
bool ds3_char_data_serialize(const ds3_char_data_t *c, uint8_t *out, size_t out_size);

/**
 * @brief Imports raw character data into a specific slot in save data
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @param raw_data Pointer to raw data to import
 * @return true if import succeeds, false otherwise
 */
bool ds3_char_data_import_raw(ds3_save_data_t *save_data, int slot, const uint8_t *raw_data);
