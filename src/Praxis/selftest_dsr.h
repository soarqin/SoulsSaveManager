#pragma once
/**
 * @file selftest_dsr.h
 * @brief DSR (Dark Souls: Remastered) selftest subcommand dispatcher.
 * @details Exposes the dispatcher entry point that routes `dsr-*` subcommands
 *          to their handlers. Implemented in selftest_dsr.c and linked only
 *          into the PraxisSelftest executable (not the GUI praxis target).
 */

#include <wchar.h>

/**
 * @brief Dispatch DSR selftest subcommands.
 * @param argc Argument count from selftest_main wmain.
 * @param argv Argument vector (UTF-16 wide strings).
 * @param sub  Subcommand name (argv[2] from --selftest invocation).
 * @return 0 if subcommand handled (success or assertion failure printed),
 *         -1 if subcommand is not a DSR subcommand (let next dispatcher try),
 *         positive non-zero on early validation failure.
 */
int praxis_selftest_dsr_dispatch(int argc, wchar_t **argv, const wchar_t *sub);
