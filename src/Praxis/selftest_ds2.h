#pragma once
/**
 * @file selftest_ds2.h
 * @brief DS2S selftest subcommand dispatcher.
 * @details Provides the entry point for the DS2S (Dark Souls II: Scholar of the
 *          First Sin) selftest subcommands. The dispatcher chains
 *          `wcscmp(sub, L"ds2s-...")` checks and returns -1 if no DS2S
 *          subcommand matches, allowing the parent dispatcher to try other
 *          handlers or print "unknown selftest subcommand".
 */

#include <wchar.h>

/**
 * @brief Dispatch a DS2S selftest subcommand.
 * @param argc Total argc passed from praxis_selftest_run (includes "--selftest").
 * @param argv Argument vector (UTF-16 wide strings).
 * @param sub  Subcommand name (equal to argv[2]).
 * @return Exit code: 0=success, 1=assertion/usage failure, -1=no match.
 */
int praxis_selftest_ds2_dispatch(int argc, wchar_t **argv, const wchar_t *sub);
