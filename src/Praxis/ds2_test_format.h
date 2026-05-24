#pragma once

/**
 * @file ds2_test_format.h
 * @brief DS2S (Dark Souls II: Scholar of the First Sin) format constants for selftest fixture builder
 * @details
 * Named constants for the hex literals used by the DS2S selftest fixture
 * builder to create minimal valid Dark Souls II save files for testing.
 * These constants mirror the BND4 container format and DS2S save structure
 * offsets.
 *
 * For selftest use only; not part of production save handling.
 * Constants here are consumed only by praxis_selftest.c DS2S fixture and
 * AES vector tests. ds2save.c internal helpers MUST NOT include this header.
 * Production code derives sizes from the BND4 header at runtime.
 *
 * Size constants are empirically derived from a real DS2S save
 * (DS2SOFS0000.sl2) via T1 read-only inspection. See
 * .omo/research/save-format-verification.md Section 2 for the
 * source-of-truth derivation.
 *
 * DS2S has 23 BND4 entries: 1 summary + 10 char-A + 10 char-B + 2 extras.
 * Char slot N consists of BND4 entry (N+1) [part A] + entry (N+11) [part B].
 *
 * IMPORTANT — corrected offsets vs the research doc (T1 empirical):
 *   - Steam ID offset is 0x39, NOT 0x35 (research doc is off by +4).
 *   - Profile availability marker is at profile_base + 4, NOT + 0
 *     (the leading 4 bytes are an int32 used/unused flag).
 */

#include <stdint.h>

/* DS2 AES-128-CBC key (16 bytes) — hex-decode of "599F9B699640A55236EE2D70835EC744"
 * Source: DS2 save format research / community-known constant. */
static const uint8_t DS2_AES_KEY_BYTES[16] = {
    0x59, 0x9F, 0x9B, 0x69, 0x96, 0x40, 0xA5, 0x52,
    0x36, 0xEE, 0x2D, 0x70, 0x83, 0x5E, 0xC7, 0x44
};

/* DS2S BND4 total entry count (CONFIRMED 23 by T1; refutes "21" claim). */
#define DS2_BND4_TOTAL_ENTRY_COUNT  23

/* DS2S save file slot sizes (on-disk, including 16-byte MD5 + 16-byte IV header).
 * DS2S uses a DUAL-SLOT abstraction: each character occupies TWO BND4 entries:
 *   - char_slot N part A = BND4 entry (N + 1)   — smaller, 0x1B2E0 bytes
 *   - char_slot N part B = BND4 entry (N + 11)  — larger,  0x7A8D0 bytes
 *
 * Buffer ("plaintext") sizes are file_size - 32 (AES-CBC formats with
 * 16-byte MD5 + 16-byte IV header).
 */
#define DS2_CHAR_A_SLOT_ON_DISK_SIZE   0x1B2E0u  /* 111328 bytes on-disk (entries 1..10) */
#define DS2_CHAR_A_PLAINTEXT_SIZE      0x1B2C0u  /* buffer = file - 32 */
#define DS2_CHAR_B_SLOT_ON_DISK_SIZE   0x7A8D0u  /* 501968 bytes on-disk (entries 11..20) */
#define DS2_CHAR_B_PLAINTEXT_SIZE      0x7A8B0u  /* buffer = file - 32 */
#define DS2_SUMMARY_SLOT_ON_DISK_SIZE  0x1AB0u   /* 6832 bytes on-disk (entry 0) */
#define DS2_SUMMARY_PLAINTEXT_SIZE     0x1A90u   /* buffer = file - 32 */

/* BND4 container structure offsets (identical to DS3/ER/DSR). */
#define DS2_BND4_SLOT_COUNT_OFFSET   0x0Cu   /* Offset to slot count field in BND4 header */
#define DS2_BND4_SIZE_ARRAY_OFFSET   0x48u   /* Offset to slot size array in BND4 header */
#define DS2_BND4_OFFSET_ARRAY_OFFSET 0x50u   /* Offset to slot offset array in BND4 header */
#define DS2_BND4_ENTRY_STRIDE        0x20u   /* Stride between slot entries in BND4 arrays */
#define DS2_BND4_FILE_HEADER_SIZE    0x300u  /* BND4 file header size */

/* DS2S summary slot payload offsets (within decrypted plaintext).
 * NOTE: Steam ID is TEXT (16 lowercase hex chars), NOT a binary uint64. */
#define DS2_SUMMARY_USERID_TEXT_OFFSET  0x39u   /* 16 ASCII lowercase hex chars — EMPIRICAL T1 (NOT 0x35!) */
#define DS2_USERID_TEXT_LENGTH          16      /* 16 ASCII chars */
#define DS2_SUMMARY_ACTIVE_OFFSET       0x36Cu  /* int32 LE — VERIFIED via T1 */
#define DS2_SUMMARY_PROFILE_OFFSET      0x37Cu  /* Profile area start; stride DS2_PROFILE_SIZE, 10 profiles */
#define DS2_PROFILE_SIZE                0x1F0u  /* Size of one character profile entry */
#define DS2_PROFILE_AVAILABLE_FLAG_OFFSET 0u    /* int32 LE at profile+0: 1=used, 0=unused — EMPIRICAL T1 */

/* Test Steam IDs for cross-account selftest (TEXT format, 16 lowercase hex chars).
 * NEVER use %016llX (uppercase) — DS2S format string is %016llx (lowercase). */
#define DS2_TEST_USERID_A_HEX  "0110000102b5851d"
#define DS2_TEST_USERID_B_HEX  "0110000102b5851e"

/* Fixed 16-byte IV used by the fixture builder (deterministic, reproducible). */
static const uint8_t DS2_TEST_IV[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

/* Known plaintext for ds2-aes-known-vector selftest (16 bytes: "DS2 test vec.\0\0\0"). */
static const uint8_t DS2_TEST_KNOWN_PLAINTEXT[16] = {
    'D', 'S', '2', ' ', 't', 'e', 's', 't',
    ' ', 'v', 'e', 'c', '.', 0x00, 0x00, 0x00
};

/* Pre-computed AES-128-CBC ciphertext of DS2_TEST_KNOWN_PLAINTEXT with DS2_TEST_IV and DS2_AES_KEY_BYTES.
 * Computed with PowerShell [System.Security.Cryptography.Aes]::Create() (CBC, PKCS7 padding):
 *   key = 599F9B699640A55236EE2D70835EC744
 *   iv  = 000102030405060708090A0B0C0D0E0F
 *   pt  = 4453322074657374207665632E000000 (16 bytes)
 *   ct  = E8C91C4726858C3B786B913F9AFB259C 913308AFB99FCB92305D08D9DB0B7002 (32 bytes with PKCS7 padding block)
 * Note: PKCS7 padding adds a full 16-byte block when plaintext is exactly one block.
 * See .omo/evidence/task-3-kat-derivation.txt for the reproducible derivation. */
static const uint8_t DS2_TEST_KNOWN_CIPHERTEXT[32] = {
    0xE8, 0xC9, 0x1C, 0x47, 0x26, 0x85, 0x8C, 0x3B,
    0x78, 0x6B, 0x91, 0x3F, 0x9A, 0xFB, 0x25, 0x9C,
    0x91, 0x33, 0x08, 0xAF, 0xB9, 0x9F, 0xCB, 0x92,
    0x30, 0x5D, 0x08, 0xD9, 0xDB, 0x0B, 0x70, 0x02
};
#define DS2_TEST_KNOWN_CIPHERTEXT_SIZE 32u
