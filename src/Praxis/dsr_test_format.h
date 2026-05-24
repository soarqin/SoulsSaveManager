#pragma once

/**
 * @file dsr_test_format.h
 * @brief DSR (Dark Souls: Remastered) format constants for selftest fixture builder
 * @details
 * Named constants for the hex literals used by the DSR selftest fixture
 * builder to create minimal valid Dark Souls: Remastered save files for
 * testing. These constants mirror the BND4 container format and DSR save
 * structure offsets.
 *
 * For selftest use only; not part of production save handling.
 * Constants here are consumed only by praxis_selftest.c DSR fixture and AES
 * vector tests. dsrsave.c internal helpers MUST NOT include this header.
 * Production code derives sizes from the BND4 header at runtime.
 *
 * Size constants are empirically derived from a real DSR save (DRAKS0005.sl2)
 * via T1 read-only inspection. See .omo/research/save-format-verification.md
 * Section 1 for the source-of-truth derivation.
 *
 * DSR has 11 BND4 entries: 10 char slots + 1 summary slot. There is NO
 * regulation slot in DSR.
 */

#include <stdint.h>

/* DSR AES-128-CBC key (16 bytes) — hex-decode of "0123456789ABCDEFFEDCBA9876543210"
 * Source: DSR save format research / community-known constant. */
static const uint8_t DSR_AES_KEY_BYTES[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

/* DSR BND4 entry count (10 char + 1 summary; NO regulation slot — verify >= 11, NOT >= 12). */
#define DSR_BND4_ENTRY_COUNT  11

/* DSR save file slot sizes (on-disk, including 16-byte MD5 + 16-byte IV header).
 * Empirically derived from real DSR save via T1. All 11 entries share the
 * SAME on-disk size (0x60030).
 *
 *   - Char slot on-disk size: 0x60030 (393264 bytes)
 *   - Summary slot on-disk size: 0x60030 (393264 bytes)
 *   - Char plaintext buffer (file - 32 for AES-CBC formats):     0x60010
 *   - Summary plaintext buffer (file - 32):                       0x60010
 *
 * The "plaintext buffer" includes any PKCS7 padding; the semantic plaintext
 * size after PKCS7 strip is 0x60004 (verified on summary; 12-byte pad).
 */
#define DSR_CHAR_SLOT_ON_DISK_SIZE     0x60030u  /* 393264 bytes on-disk */
#define DSR_SUMMARY_SLOT_ON_DISK_SIZE  0x60030u  /* 393264 bytes on-disk */
#define DSR_CHAR_PLAINTEXT_SIZE        0x60010u  /* buffer = file - 32 */
#define DSR_SUMMARY_PLAINTEXT_SIZE     0x60010u  /* buffer = file - 32 */

/* BND4 container structure offsets (identical to DS3/ER — BND4 header is game-agnostic). */
#define DSR_BND4_SLOT_COUNT_OFFSET   0x0Cu   /* Offset to slot count field in BND4 header */
#define DSR_BND4_SIZE_ARRAY_OFFSET   0x48u   /* Offset to slot size array in BND4 header */
#define DSR_BND4_OFFSET_ARRAY_OFFSET 0x50u   /* Offset to slot offset array in BND4 header */
#define DSR_BND4_ENTRY_STRIDE        0x20u   /* Stride between slot entries in BND4 arrays */
#define DSR_BND4_FILE_HEADER_SIZE    0x300u  /* BND4 file header size */

/* DSR summary slot payload offsets (absolute offsets within decrypted plaintext).
 * NOTE: active_slot is a 1-BYTE field in DSR (not int32 like DS3).
 * The first 4 decrypted bytes are an internal slot header before game data. */
#define DSR_PLAINTEXT_SLOT_HEADER_SIZE 0x04u
#define DSR_SUMMARY_ACTIVE_OFFSET      (DSR_PLAINTEXT_SLOT_HEADER_SIZE + 0x45u)  /* Active slot index (uint8) in summary plaintext — VERIFIED via T1 */
#define DSR_SUMMARY_AVAILABLE_OFFSET   (DSR_PLAINTEXT_SLOT_HEADER_SIZE + 0xB0u)  /* Available slots: 10 bytes, 1 byte per slot — VERIFIED via T1 */
#define DSR_SUMMARY_PROFILE_OFFSET     (DSR_PLAINTEXT_SLOT_HEADER_SIZE + 0xC0u)  /* Profile area start in summary plaintext */
#define DSR_PROFILE_SIZE             0x190u  /* Size of one character profile entry */

/* Test Steam IDs for cross-account selftest fixtures.
 * NOTE: DSR has NO Steam ID embedded in save data — these IDs are present
 * for fixture / filename / parity purposes only and are NOT patched into
 * the decrypted plaintext. See learnings.md "DSR Entry Count = 11" section. */
#define DSR_TEST_USERID_A  76561198002847837ULL
#define DSR_TEST_USERID_B  76561198002847838ULL

/* Fixed 16-byte IV used by the fixture builder (deterministic, reproducible). */
static const uint8_t DSR_TEST_IV[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

/* Known plaintext for dsr-aes-known-vector selftest (16 bytes: "DSR test vec.\0\0\0"). */
static const uint8_t DSR_TEST_KNOWN_PLAINTEXT[16] = {
    'D', 'S', 'R', ' ', 't', 'e', 's', 't',
    ' ', 'v', 'e', 'c', '.', 0x00, 0x00, 0x00
};

/* Pre-computed AES-128-CBC ciphertext of DSR_TEST_KNOWN_PLAINTEXT with DSR_TEST_IV and DSR_AES_KEY_BYTES.
 * Computed with PowerShell [System.Security.Cryptography.Aes]::Create() (CBC, PKCS7 padding):
 *   key = 0123456789ABCDEFFEDCBA9876543210
 *   iv  = 000102030405060708090A0B0C0D0E0F
 *   pt  = 4453522074657374207665632E000000 (16 bytes)
 *   ct  = 177AD08B9C2946E91F1856E5CC5827FF 43D0257C79B3B15EE30390A88911686A (32 bytes with PKCS7 padding block)
 * Note: PKCS7 padding adds a full 16-byte block when plaintext is exactly one block.
 * See .omo/evidence/task-3-kat-derivation.txt for the reproducible derivation. */
static const uint8_t DSR_TEST_KNOWN_CIPHERTEXT[32] = {
    0x17, 0x7A, 0xD0, 0x8B, 0x9C, 0x29, 0x46, 0xE9,
    0x1F, 0x18, 0x56, 0xE5, 0xCC, 0x58, 0x27, 0xFF,
    0x43, 0xD0, 0x25, 0x7C, 0x79, 0xB3, 0xB1, 0x5E,
    0xE3, 0x03, 0x90, 0xA8, 0x89, 0x11, 0x68, 0x6A
};
#define DSR_TEST_KNOWN_CIPHERTEXT_SIZE 32u
