#pragma once

/**
 * @file ds3_test_format.h
 * @brief DS3 format constants for selftest fixture builder
 * @details
 * Named constants for the hex literals used in praxis_make_min_valid_ds3_sl2()
 * to create minimal valid Dark Souls III save files for testing. These constants
 * mirror the BND4 container format and DS3 save structure offsets.
 * For selftest use only; not part of production save handling.
 *
 * Constants ONLY used by praxis_selftest.c DS3 fixture and AES vector tests.
 * ds3save.c internal helpers MUST NOT include this header. Production code
 * derives sizes from the BND4 header at runtime.
 */

#include <stdint.h>

/* DS3 AES-128-CBC key (16 bytes) — from DS3 save format research doc */
static const uint8_t DS3_AES_KEY_BYTES[] = {
    0xFD, 0x46, 0x4D, 0x69, 0x5E, 0x69, 0xA3, 0x9A,
    0x10, 0xE3, 0x19, 0xA7, 0xAC, 0xE8, 0xB7, 0xFA
};

/* DS3 save file slot sizes (on-disk, including 16-byte MD5 + 16-byte IV header).
 * Derived from real DS3 save (DS30000.sl2). Char and summary slots have
 * DIFFERENT sizes in real DS3 saves (BND4 entry header is the source of truth):
 *   - Char slot on-disk size: 0xC0030 (from BND4 entry header at offset 0x48)
 *   - Summary slot on-disk size: 0x60030 (BND4 entry index 10)
 *   - Char plaintext size after AES-128-CBC PKCS7 decrypt = 0xC0004 (12-byte pad)
 *   - Summary plaintext size after AES-128-CBC PKCS7 decrypt = 0x60004 (12-byte pad)
 */
#define DS3_CHAR_PLAINTEXT_SIZE      0xC0004u  /* Must equal real DS3 char slot plaintext size (see T1 extraction). Fixture buffers use this size so ds3_save_data_load's BND4 entry size assertion accepts the fixture. */
#define DS3_SUMMARY_PLAINTEXT_SIZE   0x60004u  /* Real DS3 summary slot plaintext size (derived from real DS3 BND4 entry index 10, AES-128-CBC PKCS7 decrypt). */
#define DS3_CHAR_SLOT_ON_DISK_SIZE   0xC0030u  /* Real DS3 char slot encrypted-blob size from BND4 entry header. Equals 16(MD5)+16(IV)+ciphertext. */
#define DS3_SUMMARY_SLOT_ON_DISK_SIZE 0x60030u /* Real DS3 summary slot on-disk size (BND4 entry index 10). */

/* BND4 container structure offsets (same as ER — DS3 uses identical BND4 header) */
#define DS3_BND4_SLOT_COUNT_OFFSET        0x0Cu   /* Offset to slot count field in BND4 header */
#define DS3_BND4_SLOT_SIZE_ARRAY_OFFSET   0x48u   /* Offset to slot size array in BND4 header */
#define DS3_BND4_SLOT_OFFSET_ARRAY_OFFSET 0x50u   /* Offset to slot offset array in BND4 header */
#define DS3_BND4_SLOT_ENTRY_STRIDE        0x20u   /* Stride between slot entries in BND4 arrays */
#define DS3_BND4_FILE_HEADER_SIZE         0x300u  /* BND4 file header size (same as ER) */
#define DS3_BND4_MD5_HEADER_SIZE          0x10u   /* Size of MD5 checksum prefix in slot */

/* DS3 summary slot payload offsets (within decrypted plaintext) */
#define DS3_SUMMARY_USERID_OFFSET   0x08u   /* Steam user ID (uint64) in summary plaintext */
#define DS3_SUMMARY_ACTIVE_OFFSET   0x0FE8u /* Active slot index (int32) in summary plaintext */
#define DS3_SUMMARY_AVAILABLE_OFFSET 0x1098u /* Available slots bitmap (10 bytes) in summary plaintext */
#define DS3_SUMMARY_PROFILE_OFFSET  0x10A2u /* Profile area start (10 x 0x22A bytes) in summary plaintext */
#define DS3_PROFILE_SIZE            0x22Au  /* Size of one character profile entry */

/* DS3 char slot payload offsets */
#define DS3_CHAR_USERID_LEN_OFFSET  0x58u   /* Offset of N (uint32) in char plaintext; userid is at N+0x6F */
#define DS3_CHAR_USERID_DELTA       0x6Fu   /* Delta added to N to get userid offset */

/* Test Steam IDs for cross-account selftest */
#define DS3_TEST_USERID_A  0x1100001111111111ull
#define DS3_TEST_USERID_B  0x1100002222222222ull

/* Fixed 16-byte IV used by the fixture builder (deterministic, reproducible) */
static const uint8_t DS3_TEST_IV[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

/* Known plaintext for ds3-aes-known-vector selftest (16 bytes: "DS3 test vec.\0\0\0") */
static const uint8_t DS3_TEST_KNOWN_PLAINTEXT[] = {
    0x44, 0x53, 0x33, 0x20, 0x74, 0x65, 0x73, 0x74,
    0x20, 0x76, 0x65, 0x63, 0x2E, 0x00, 0x00, 0x00
};

/* Pre-computed AES-128-CBC ciphertext of DS3_TEST_KNOWN_PLAINTEXT with DS3_TEST_IV and DS3_AES_KEY_BYTES.
 * Computed with PowerShell System.Security.Cryptography.Aes (CBC, PKCS7 padding):
 *   key = FD464D695E69A39A10E319A7ACE8B7FA
 *   iv  = 000102030405060708090A0B0C0D0E0F
 *   pt  = 44533320746573742076656C2E000000 (16 bytes)
 *   ct  = 4156C74C16947A019143A7BF37D614B4 66CA91E42C68B1F0CFE8F4321C90D027 (32 bytes with PKCS7 padding block)
 * Note: PKCS7 padding adds a full 16-byte block when plaintext is exactly one block. */
static const uint8_t DS3_TEST_KNOWN_CIPHERTEXT[] = {
    0x41, 0x56, 0xC7, 0x4C, 0x16, 0x94, 0x7A, 0x01,
    0x91, 0x43, 0xA7, 0xBF, 0x37, 0xD6, 0x14, 0xB4,
    0x66, 0xCA, 0x91, 0xE4, 0x2C, 0x68, 0xB1, 0xF0,
    0xCF, 0xE8, 0xF4, 0x32, 0x1C, 0x90, 0xD0, 0x27
};
#define DS3_TEST_KNOWN_CIPHERTEXT_SIZE 32u
