/**
 * @file dsrsave.h
 * @brief Header file for Dark Souls: Remastered save data management functions.
 * @details This file contains declarations for functions that load, inspect,
 *          serialize, and import Dark Souls: Remastered character save data.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct dsr_save_data_s dsr_save_data_t;
typedef struct dsr_char_data_s dsr_char_data_t;

/* DSR_CHAR_DATA_SERIALIZED_SIZE = (DSR_CHAR_PLAINTEXT_SIZE - DSR_SLOT_HEADER_SIZE) + DSR_PROFILE_SIZE
 * = (0x60010 - 4) + 0x190 = 0x6019C */
#define DSR_CHAR_DATA_SERIALIZED_SIZE  0x6019Cu

/**
 * @brief Loads save data from a file.
 * @param path Path to the save file.
 * @param out_save Pointer to receive the loaded save data structure.
 * @return true if loading succeeds, false otherwise.
 */
bool dsr_save_data_load(const wchar_t *path, dsr_save_data_t **out_save);

/**
 * @brief Frees allocated save data memory.
 * @param save Pointer to save data structure to free.
 */
void dsr_save_data_free(dsr_save_data_t *save);

/**
 * @brief Gets the active character slot index from save data.
 * @param save Pointer to save data structure.
 * @param out_slot Pointer to receive slot index.
 * @return true if the active slot was read successfully, false otherwise.
 */
bool dsr_save_get_active_slot(const dsr_save_data_t *save, int *out_slot);

/**
 * @brief Gets a reference to a character data structure.
 * @param save Pointer to save data structure.
 * @param slot Slot number (0-9).
 * @return Pointer to character data structure, or NULL if slot is invalid or unavailable.
 */
dsr_char_data_t *dsr_char_data_ref(dsr_save_data_t *save, int slot);

/**
 * @brief Serializes character data to a flat buffer.
 * @param char_data Pointer to character data structure.
 * @param out_buf Output buffer.
 * @param buf_size Buffer size.
 * @return true if serialization succeeds, false otherwise.
 */
bool dsr_char_data_serialize(const dsr_char_data_t *char_data, uint8_t *out_buf, size_t buf_size);

/**
 * @brief Imports raw character data into a specific slot in save data.
 * @param save Pointer to save data structure.
 * @param slot Slot number (0-9).
 * @param raw_data Pointer to raw data to import.
 * @param raw_size Size of raw data in bytes.
 * @return true if import succeeds, false otherwise.
 */
bool dsr_char_data_import_raw(dsr_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size);
