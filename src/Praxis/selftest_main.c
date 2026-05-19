/**
 * @file selftest_main.c
 * @brief Console entry point for the standalone PraxisSelftest executable.
 * @details Wraps praxis_selftest_run so it can be invoked as a plain console
 *          program: PraxisSelftest.exe <subcommand> [args...]
 *          Internally prepends a synthetic "--selftest" token so the existing
 *          praxis_selftest_run argument layout is preserved unchanged.
 */

#include "praxis_selftest.h"
#include "praxis_window_common.h"
#include "profile_store.h"
#include "../common/save_compress.h"

#include <stdlib.h>
#include <wchar.h>

#include <windows.h>

/* Global state defined in main.c for the GUI Praxis target. The selftest
 * runner pulls in object files that reference these symbols (theme.c,
 * praxis_window_common.c), so we provide matching definitions here. The
 * selftest subcommands themselves do not exercise these paths. */
praxis_app_t g_app = {0};
profile_store_t g_profile_store;
HANDLE g_log_file = INVALID_HANDLE_VALUE;

int wmain(int argc, wchar_t **argv) {
    /* praxis_selftest_run expects:
     *   argv[0] = exe name  (ignored)
     *   argv[1] = "--selftest"
     *   argv[2] = subcommand
     *   argv[3..] = subcommand args
     * We receive:
     *   argv[0] = exe name
     *   argv[1] = subcommand
     *   argv[2..] = subcommand args
     * Build a synthetic argv with "--selftest" inserted at [1]. */

    int new_argc = argc + 1;
    wchar_t **new_argv;
    int result;
    int i;

    new_argv = (wchar_t **)LocalAlloc(LMEM_FIXED, (size_t)new_argc * sizeof(wchar_t *));
    if (!new_argv) {
        return 2;
    }

    new_argv[0] = argc > 0 ? argv[0] : L"PraxisSelftest.exe";
    new_argv[1] = L"--selftest";
    for (i = 1; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }

    save_compress_init();
    result = praxis_selftest_run(new_argc, new_argv);
    LocalFree(new_argv);
    return result;
}
