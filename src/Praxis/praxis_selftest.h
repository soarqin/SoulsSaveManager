#pragma once
/**
 * @file praxis_selftest.h
 * @brief Selftest subcommand dispatcher entry point.
 */

#include <wchar.h>

#ifdef PRAXIS_ENABLE_SELFTEST

/**
 * @brief Run selftest from --selftest command-line.
 * @param argc Argument count (total argc, including a leading "--selftest" token at argv[1])
 * @param argv Argument vector (UTF-16 wide strings)
 * @return Exit code: 0=success, 1=assertion failed, 2=usage error
 */
int praxis_selftest_run(int argc, wchar_t **argv);

#else

/* Stub implementation when selftest is disabled. */
static inline int praxis_selftest_run(int argc, wchar_t **argv) {
    (void)argc; (void)argv; return 2;
}

#endif /* PRAXIS_ENABLE_SELFTEST */
