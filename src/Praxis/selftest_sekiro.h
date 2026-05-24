#pragma once
/**
 * @file selftest_sekiro.h
 * @brief Sekiro: Shadows Die Twice selftest subcommand dispatcher.
 * @details Exposes the dispatcher entry point that routes `sekiro-*`
 *          subcommands to their handlers. Implemented in selftest_sekiro.c
 *          and linked only into the PraxisSelftest executable (not the GUI
 *          praxis target).
 */

#include <wchar.h>

/**
 * @brief Dispatch Sekiro selftest subcommands.
 * @param argc Argument count from selftest_main wmain.
 * @param argv Argument vector (UTF-16 wide strings).
 * @param sub  Subcommand name (argv[2] from --selftest invocation).
 * @return 0 if subcommand handled (success or assertion failure printed),
 *         -1 if subcommand is not a Sekiro subcommand (let next dispatcher try),
 *         positive non-zero on early validation failure.
 */
int praxis_selftest_sekiro_dispatch(int argc, wchar_t **argv, const wchar_t *sub);
