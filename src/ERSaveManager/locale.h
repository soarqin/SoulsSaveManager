/**
 * @file locale.h
 * @brief Header file for localization functions
 * @details This file contains declarations for functions that handle localization
 *          and string translations for the Elden Ring face data manager.
 */

#pragma once

#include <wchar.h>

/**
 * @brief Enumeration of string indices for localization
 */
typedef enum locale_string_index_e {
    STR_APP_TITLE = 0, /* "Elden Ring Save Face Data Manager" */
    STR_CONFIRM, /* "Confirm" */
    STR_CANCEL, /* "Cancel" */
    STR_CHANGE_SAVE_FOLDER, /* "Change Save Folder" */
    STR_SLOT, /* "Slot" */
    STR_BODY_TYPE, /* "Body Type" */
    STR_TYPE_A, /* "Type A" */
    STR_TYPE_B, /* "Type B" */
    STR_EMPTY, /* "(EMPTY)" */
    STR_IMPORT_FACE_DATA, /* "Import Face Data" */
    STR_EXPORT_FACE_DATA, /* "Export Face Data" */
    STR_ALL_FILES, /* "All Files" */
    STR_FAILED_LOAD_SAVE, /* "Failed to load save file" */
    STR_ERROR, /* "Error" */
    STR_IMPORT_SUCCESS, /* "Face data imported successfully" */
    STR_SUCCESS, /* "Success" */
    STR_IMPORT_FAILED, /* "Failed to import face data" */
    STR_EXPORT_SUCCESS, /* "Face data exported successfully" */
    STR_EXPORT_FAILED, /* "Failed to export face data" */
    STR_NAME, /* "Name" */
    STR_LEVEL, /* "Level" */
    STR_ATTRIBUTES, /* "Attributes" */
    STR_IN_GAME_TIME, /* "In-Game Time" */
    STR_CHARACTERS, /* "Characters" */
    STR_FACES, /* "Faces" */
    STR_IMPORT_CHARACTER, /* "Import Character" */
    STR_EXPORT_CHARACTER, /* "Export Character" */
    STR_RENAME_CHARACTER, /* "Rename Character" */
    STR_ENTER_NEW_NAME, /* "Enter new character name:" */
    STR_CHARACTER_IMPORT_SUCCESS, /* "Character data imported successfully" */
    STR_CHARACTER_IMPORT_FAILED, /* "Failed to import character data" */
    STR_CHARACTER_EXPORT_SUCCESS, /* "Character data exported successfully" */
    STR_CHARACTER_EXPORT_FAILED, /* "Failed to export character data" */
    STR_RENAME_CHARACTER_FAILED, /* "Failed to rename character" */
    STR_LANGUAGE, /* "Language" */
    STR_STEAM_ID_MISMATCH, /* "The save folder does not match the Steam ID..." */
    STR_IMPORT_NPC_FACE_DATA, /* "Import NPC face data" */
    STR_NPC_BASE, /* "Base Game" */
    STR_NPC_BASE_NON_INTERACTABLE, /* "Base Game (Non-Interactable)" */
    STR_NPC_DLC, /* "DLC" */
    STR_NPC_DLC_NON_INTERACTABLE, /* "DLC (Non-Interactable)" */
    STR_SELECT_CHARACTER_CONTENT, /* "Select a character to import data from." */
    STR_NO_CHARACTER_FOUND, /* "No character found" */
    STR_MANAGE_FACES, /* "Face Data..." */
    STR_VIGOR, /* "Vigor" */
    STR_MIND, /* "Mind" */
    STR_ENDURANCE, /* "Endurance" */
    STR_STRENGTH, /* "Strength" */
    STR_DEXTERITY, /* "Dexterity" */
    STR_INTELLIGENCE, /* "Intelligence" */
    STR_FAITH, /* "Faith" */
    STR_ARCANE, /* "Arcane" */
    STR_RUNES_HELD, /* "Runes Held" */
    STR_DEATH_COUNT, /* "Deaths" */
    STR_OPTIONS,            /* "Options" */
    STR_COMPRESSION_LEVEL,  /* "Compression" */
    STR_COMPRESSION_FAST,   /* "Fast" */
    STR_COMPRESSION_NORMAL, /* "Normal" */
    STR_COMPRESSION_MAX,    /* "Max" */
    STR_COMPRESSION_ERROR,  /* "Compression failed" */
    STR_DECOMPRESSION_ERROR, /* "Invalid or corrupt compressed file" */
    STR_THEME,              /* "Theme" */
    STR_THEME_SYSTEM,       /* "System" */
    STR_THEME_LIGHT,        /* "Light" */
    STR_THEME_DARK,         /* "Dark" */
    STR_TOOLS,              /* "Tools" */
    STR_DOWNPATCH_1_02_1,   /* "Downpatch to 1.02.1" */
    STR_DOWNPATCH_NO_SAVE,  /* "No save file loaded" */
    STR_DOWNPATCH_SUCCESS,  /* "Save downpatched to 1.02.1" */
    STR_DOWNPATCH_FAILED,   /* "Failed to downpatch save" */
    STR_MAX /* Total number of strings */
} locale_string_index_t;

/**
 * @brief Gets a localized string by index
 * @param string_index Index of the string to retrieve
 * @return Pointer to the localized string
 */
const wchar_t *locale_str(locale_string_index_t string_index);

/**
 * @brief Sets the current locale
 * @param locale_index Index of the locale to set
 */
void set_current_locale(int locale_index);

/**
 * @brief Gets the current locale index
 * @return Current locale index
 */
int get_current_locale(void);

/**
 * @brief Gets the total number of available locales
 * @return Number of available locales
 */
int locale_count(void);

/**
 * @brief Gets the name of a locale by index
 * @param locale_index Index of the locale
 * @return Pointer to the locale name string
 */
const wchar_t *locale_name(int locale_index);

/**
 * @brief Detects the system language and returns the best matching language index
 * @return Best matching language index
 */
int detect_system_language(void);
