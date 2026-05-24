#pragma once

/**
 * @file sekiro_test_format.h
 * @brief Sekiro format constants for selftest fixture builder
 * @details
 * Named constants for the hex literals used by the Sekiro selftest fixture
 * builder to create minimal valid Sekiro: Shadows Die Twice save files for
 * testing. These constants mirror the BND4 container format and Sekiro
 * save structure offsets.
 *
 * For selftest use only; not part of production save handling.
 * Constants here are consumed only by praxis_selftest.c Sekiro fixture
 * tests. sekirosave.c internal helpers MUST NOT include this header.
 * Production code derives sizes from the BND4 header at runtime.
 *
 * Size constants are empirically derived from a real Sekiro save
 * (S0000.sl2) via T1 read-only inspection. See
 * .omo/research/save-format-verification.md Section 3 for the
 * source-of-truth derivation.
 *
 * Sekiro has 12 BND4 entries: 10 char + 1 summary + 1 regulation.
 *
 * IMPORTANT — Sekiro is UNENCRYPTED:
 *   - No AES key, no IV, no PKCS7 padding.
 *   - Slot layout is [16-byte MD5][raw plaintext], no encryption.
 *   - Buffer size = file_size - 16 (NOT - 32 like AES-CBC formats).
 *   - This header therefore intentionally contains NO AES constants, NO
 *     test IV, and NO known ciphertext. Adding any of those is a bug.
 */

#include <stdint.h>

/* Sekiro BND4 entry count (10 char + 1 summary + 1 regulation). */
#define SEKIRO_BND4_ENTRY_COUNT  12

/* Sekiro save file slot sizes (on-disk, including 16-byte MD5 header only — no IV).
 * Empirically derived from real Sekiro save via T1.
 *
 *   - Char slot on-disk size:    0x100010 (1048592 bytes = 1 MiB + 16-byte MD5)
 *   - Summary slot on-disk size: 0x60010  (393232 bytes  = 384 KiB + 16-byte MD5)
 *
 * For unencrypted formats, buffer size IS the semantic plaintext size:
 *   - Char plaintext:    file - 16 = 0x100000 (1 MiB exact)
 *   - Summary plaintext: file - 16 = 0x60000  (384 KiB exact)
 */
#define SEKIRO_CHAR_SLOT_ON_DISK_SIZE     0x100010u  /* 1048592 bytes on-disk */
#define SEKIRO_CHAR_PLAINTEXT_SIZE        0x100000u  /* buffer = file - 16 (no AES) */
#define SEKIRO_SUMMARY_SLOT_ON_DISK_SIZE  0x60010u   /* 393232 bytes on-disk */
#define SEKIRO_SUMMARY_PLAINTEXT_SIZE     0x60000u   /* buffer = file - 16 (no AES) */

/* BND4 container structure offsets (identical to DS3/ER/DSR/DS2). */
#define SEKIRO_BND4_SLOT_COUNT_OFFSET   0x0Cu   /* Offset to slot count field in BND4 header */
#define SEKIRO_BND4_SIZE_ARRAY_OFFSET   0x48u   /* Offset to slot size array in BND4 header */
#define SEKIRO_BND4_OFFSET_ARRAY_OFFSET 0x50u   /* Offset to slot offset array in BND4 header */
#define SEKIRO_BND4_ENTRY_STRIDE        0x20u   /* Stride between slot entries in BND4 arrays */
#define SEKIRO_BND4_FILE_HEADER_SIZE    0x300u  /* BND4 file header size */

/* Sekiro summary slot payload offsets (within raw plaintext; no decryption needed). */
#define SEKIRO_SUMMARY_USERID_OFFSET     0x24u    /* Steam user ID (uint64 LE) — VERIFIED via T1 */
#define SEKIRO_SUMMARY_ACTIVE_OFFSET     0x2508u  /* Active slot index (int32 LE) — VERIFIED via T1 */
#define SEKIRO_SUMMARY_AVAILABLE_OFFSET  0xD4u    /* Available slots: 10 bytes, 1 byte per slot — VERIFIED via T1 */
#define SEKIRO_SUMMARY_PROFILE_OFFSET    0x104u   /* Profile area start in summary plaintext */
#define SEKIRO_PROFILE_SIZE              0x218u   /* Size of one character profile entry */

/* Sekiro char-slot Steam ID formula: steam_id_uint64 @ (uint32@0x0C + 0x44).
 * The Steam ID offset is VARIABLE per character; bounds-check N before use. */
#define SEKIRO_CHAR_USERID_LEN_OFFSET  0x0Cu   /* Offset of N (uint32 LE) in char plaintext */
#define SEKIRO_CHAR_USERID_DELTA       0x44u   /* Delta added to N to get userid offset */

/* Test Steam IDs for cross-account selftest fixtures. */
#define SEKIRO_TEST_USERID_A  76561198002847837ULL
#define SEKIRO_TEST_USERID_B  76561198002847838ULL

/* Regulation slot — entry 11. Praxis MUST copy this verbatim with the rest
 * of the BND4 file. NEVER interpret as character data, NEVER write through
 * the char-slot codepath. */
#define SEKIRO_REGULATION_SLOT_INDEX  11  /* MUST NOT touch */
