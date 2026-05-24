/**
 * @file sekirosave.h
 * @brief Header file for Sekiro save data management functions.
 * @details This file contains declarations for functions that load, inspect,
 *          serialize, and import Sekiro character save data.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct sekiro_save_data_s sekiro_save_data_t;
typedef struct sekiro_char_data_s sekiro_char_data_t;

/* SEKIRO_CHAR_DATA_SERIALIZED_SIZE = SEKIRO_CHAR_PLAINTEXT_SIZE + SEKIRO_PROFILE_SIZE
 * = 0x100000 + 0x218 = 0x100218 */
#define SEKIRO_CHAR_DATA_SERIALIZED_SIZE  0x100218u

/**
 * @brief Loads Sekiro save data from a file.
 * @param path Path to the save file.
 * @param out_save Pointer to receive loaded save data.
 * @return true if loading succeeds, false otherwise.
 */
bool sekiro_save_data_load(const wchar_t *path, sekiro_save_data_t **out_save);

/**
 * @brief Frees allocated Sekiro save data memory.
 * @param save Pointer to save data structure to free.
 */
void sekiro_save_data_free(sekiro_save_data_t *save);

/**
 * @brief Gets the active character slot index from save data.
 * @param save Pointer to save data structure.
 * @param out_slot Pointer to receive slot index.
 * @return true if the active slot was read successfully, false otherwise.
 */
bool sekiro_save_get_active_slot(const sekiro_save_data_t *save, int *out_slot);

/**
 * @brief Gets a reference to an available character data structure.
 * @param save Pointer to save data structure.
 * @param slot Slot number (0-9).
 * @return Pointer to character data structure, or NULL if slot is invalid or unavailable.
 */
sekiro_char_data_t *sekiro_char_data_ref(sekiro_save_data_t *save, int slot);

/**
 * @brief Serializes character data to a flat buffer.
 * @param char_data Pointer to character data structure.
 * @param out_buf Output buffer.
 * @param buf_size Output buffer size.
 * @return true if serialization succeeds, false otherwise.
 */
bool sekiro_char_data_serialize(const sekiro_char_data_t *char_data, uint8_t *out_buf, size_t buf_size);

/**
 * @brief Imports raw serialized character data into a save slot.
 * @param save Pointer to save data structure.
 * @param slot Slot number (0-9).
 * @param raw_data Serialized character data.
 * @param raw_size Serialized character data size.
 * @return true if import succeeds, false otherwise.
 */
bool sekiro_char_data_import_raw(sekiro_save_data_t *save, int slot, const uint8_t *raw_data, size_t raw_size);
