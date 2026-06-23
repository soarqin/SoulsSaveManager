/**
 * @file ersave.h
 * @brief Header file for Elden Ring face data management functions
 * @details This file contains declarations for functions that handle face data operations
 *          in Elden Ring save files, including loading, saving, importing and exporting face data.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations for data structures */
typedef struct er_save_data_s er_save_data_t;
typedef struct er_save_simple_data_s er_save_simple_data_t;
typedef struct er_char_data_s er_char_data_t;

/**
 * @brief Character info output structure returned by er_char_data_info
 * @details Groups all character metadata into a single struct for easier extension.
 */
typedef struct er_char_info_s {
    uint32_t in_game_time; /* In-game play time in milliseconds */
    uint8_t body_type;     /* Body type (0 = Type B, 1 = Type A) */
    int level;             /* Character level */
    int stats[8];          /* Attribute stats: Vigor, Mind, Endurance, Strength, Dexterity, Intelligence, Faith, Arcane */
    int runes_held;        /* Current runes held */
    int death_count;       /* Number of times the character has died */
} er_char_info_t;

/**
 * @brief Loads save data from a file
 * @param path Path to the save file
 * @return Pointer to loaded save data structure, or NULL if loading fails
 */
er_save_data_t *er_save_data_load(const wchar_t *path);

/**
 * @brief Frees allocated save data memory
 * @param save_data Pointer to save data structure to free
 */
void er_save_data_free(er_save_data_t *save_data);

/**
 * @brief Loads simple save data from a file
 * @param path Path to the save file
 * @return Pointer to loaded simple save data structure, or NULL if loading fails
 */
er_save_simple_data_t *er_save_simple_data_load(const wchar_t *path);

/**
 * @brief Frees allocated simple save data memory
 * @param save_data Pointer to simple save data structure to free
 */
void er_save_simple_data_free(er_save_simple_data_t *save_data);

/**
 * @brief Gets the character name from simple save data
 * @param save_data Pointer to simple save data structure
 * @param slot Slot number (0-9)
 * @return Pointer to character name, or NULL if slot is invalid
 */
const wchar_t *er_save_simple_data_get_char_name(const er_save_simple_data_t *save_data, int slot);

/**
 * @brief Exports simple save data to a file
 * @param save_data Pointer to simple save data structure
 * @param slot Slot number (0-9)
 * @return Pointer to simple save data slot, or NULL if export fails
 */
uint8_t *er_save_simple_data_slot_export(const er_save_simple_data_t *save_data, int slot);

/**
 * @brief Frees allocated simple save data slot memory
 * @param slot_data Pointer to simple save data slot to free
 */
void er_save_simple_data_slot_free(uint8_t *slot_data);

/**
 * @brief Gets the user ID from save data
 * @param save_data Pointer to save data structure
 * @return User ID
 */
uint64_t er_save_get_userid(const er_save_data_t *save_data);

/**
 * @brief Get the active (last-used) character slot index from save data.
 * @details Reads the active_offset byte and returns the slot number (0-9).
 *          The semantics of this byte were verified in Task 1 (Wave 0).
 * @param save_data Pointer to loaded save data
 * @param out_slot Pointer to receive slot index (0-9)
 * @return true on success, false if save_data is NULL or byte is out of range
 */
bool er_save_get_active_slot(const er_save_data_t *save_data, int *out_slot);

/**
 * @brief Re-signs the user ID in save data
 * @param save_data Pointer to save data structure
 * @param user_id User ID to resign
 * @return true if re-signing successful, false otherwise
 */
bool er_save_resign_userid(er_save_data_t *save_data, uint64_t user_id);

/**
 * @brief Gets a reference to a character data structure
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @return Pointer to character data structure, or NULL if slot is invalid
 */
const er_char_data_t *er_char_data_ref(const er_save_data_t *save_data, int slot);

/**
 * @brief Imports character data into a specific slot in save data
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @param char_data Pointer to character data to import
 * @return true if import successful, false otherwise
 */
bool er_char_data_import(er_save_data_t *save_data, int slot, const er_char_data_t *char_data);

/**
 * @brief Imports character data into a specific slot in save data from raw data
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @param raw_data Pointer to raw data to import
 * @return true if import successful, false otherwise
 */
bool er_char_data_import_raw(er_save_data_t *save_data, int slot, const uint8_t *raw_data);

/**
 * @brief Sets the name of a character data structure
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-9)
 * @param name Pointer to name to set
 * @return true if setting successful, false otherwise
 */
bool er_char_data_set_name(er_save_data_t *save_data, int slot, const wchar_t *name);

/**
 * @brief Gets the name of a character data structure
 * @param char_data Pointer to character data structure
 * @return Pointer to the character's name, or NULL if invalid
 */
const wchar_t *er_char_data_get_name(const er_char_data_t *char_data);

/**
 * @brief Gets information about a character data structure
 * @param char_data Pointer to character data structure
 * @param info Pointer to er_char_info_t structure to fill with character info
 * @return true if information retrieval successful, false otherwise
 */
bool er_char_data_info(const er_char_data_t *char_data, er_char_info_t *info);

/**
 * @brief Loads character data from a file
 * @param path Path to character data file
 * @return Pointer to loaded character data structure, or NULL if loading fails
 */
er_char_data_t *er_char_data_from_file(const wchar_t *path);

/**
 * @brief Loads character data from a memory buffer
 * @param data Pointer to memory buffer, must be 0x280000 + 0x24C bytes long
 * @return Pointer to loaded character data structure, or NULL if loading fails
 */
er_char_data_t *er_char_data_from_memory(const uint8_t *data);

/**
 * @brief Exports character data to a file
 * @param char_data Pointer to character data structure
 * @param path Path to save the character data
 */
bool er_char_data_to_file(const er_char_data_t *char_data, const wchar_t *path);

/**
 * @brief Serializes character data to a flat buffer
 * @param c        Pointer to character data structure
 * @param out      Output buffer (must be at least out_size bytes)
 * @param out_size Buffer size; must be >= ER_CHAR_DATA_SIZE + ER_PROFILE_SIZE
 * @return true on success, false if out_size is too small
 */
bool er_char_data_serialize(const er_char_data_t *c, uint8_t *out, size_t out_size);

/**
 * @brief Frees allocated character data memory
 * @param char_data Pointer to character data structure to free
 */
void er_char_data_free(er_char_data_t *char_data);

/**
 * @brief Exports face data from a specific slot in save data
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-14)
 * @return Pointer to face data structure, or NULL if slot is invalid
 */
const uint8_t *er_face_data_ref(const er_save_data_t *save_data, int slot);

/**
 * @brief Imports face data into a specific slot in save data
 * @param save_data Pointer to save data structure
 * @param slot Slot number (0-14)
 * @param face_data Pointer to face data to import
 * @return true if import successful, false otherwise
 */
bool er_face_data_import(er_save_data_t *save_data, int slot, const uint8_t *face_data);

/**
 * @brief Gets information about face data
 * @param face_data Pointer to face data structure
 * @param available Pointer to store availability status
 * @param gender Pointer to store gender information
 */
void er_face_data_info(const uint8_t *face_data, uint8_t *available, uint8_t *gender);

/**
 * @brief Loads face data from a file
 * @param path Path to face data file
 * @return Pointer to loaded face data structure, or NULL if loading fails
 */
uint8_t *er_face_data_from_file(const wchar_t *path);

/**
 * @brief Saves face data to a file
 * @param face_data Pointer to face data structure
 * @param path Path where to save the face data
 */
bool er_face_data_to_file(const uint8_t *face_data, const wchar_t *path);

/**
 * @brief Frees allocated face data memory
 * @param face_data Pointer to face data structure to free
 */
void er_face_data_free(uint8_t *face_data);

/**
 * @brief Returns the byte value at summary_data.data[active_offset] (0-255),
 *        or -1 if save is NULL or active_offset is out of bounds.
 */
int er_save_debug_get_active_slot_byte(const er_save_data_t *save);

/**
 * @brief Returns active_offset (byte offset into summary_data.data), or 0 if save is NULL.
 */
uint32_t er_save_debug_get_active_offset(const er_save_data_t *save);

/**
 * @brief Writes value to summary_data.data[active_offset], recomputes summary MD5,
 *        persists the update to persist_path. Returns true on success.
 */
bool er_save_debug_set_active_slot_byte(er_save_data_t *save, uint8_t value, const wchar_t *persist_path);

/**
 * @brief Downpatches an Elden Ring save to version 1.02.1.
 * @details Writes game version 0x0B to the summary slot, writes the regulation
 *          version 0x9BCAF6 and the provided 1.02.1 regulation.bin body into the
 *          regulation slot (BND4 entry 11), and recomputes slot MD5 checksums.
 *          The regulation buffer must be at most (regulation_slot_size - 0x10)
 *          bytes; remaining bytes are zero-padded. The file is modified in place.
 * @param path Path to ER0000.sl2
 * @param regulation_data Raw 1.02.1 regulation.bin bytes
 * @param regulation_size Number of bytes in regulation_data
 * @return true on success, false on read/write error or invalid save structure
 */
bool er_save_downpatch_to_1_02_1(const wchar_t *path, const uint8_t *regulation_data, uint32_t regulation_size);
