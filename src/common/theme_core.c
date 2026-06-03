/**
 * @file theme_core.c
 * @brief Generic Win32 dark/light theme infrastructure.
 * @details Implements the public API in theme_core.h:
 *
 *    - Win10 build detection via RtlGetNtVersionNumbers (GetVersionEx lies
 *      under app-compat) and dynamic loading of uxtheme.dll undocumented
 *      ordinals (104/132/133/135/136), user32!SetWindowCompositionAttribute,
 *      and dwmapi!DwmSetWindowAttribute. Everything is a no-op on
 *      Windows < 1809 (build 17763).
 *
 *    - Dark + light color palettes with cached GDI brushes.
 *
 *    - Per-control appliers (apply_to_window/treeview/listview/combobox/
 *      statusbar/button/window_and_children) and message helpers
 *      (WM_CTLCOLOR*, NM_CUSTOMDRAW, WM_ERASEBKGND, WM_UAH*).
 *
 *    - Subclasses for controls whose internal paint code uses hardcoded
 *      GetSysColor values and ignores both SetWindowTheme and WM_CTLCOLOR*:
 *        * msctls_statusbar32 -> own paint
 *        * SysListView32      -> intercept header NM_CUSTOMDRAW
 *        * msctls_hotkey32    -> own paint (HKM_GETHOTKEY + GetKeyState)
 *        * EDIT               -> overlay flat 1px border in WM_NCPAINT
 *        * BUTTON groupbox/   -> own paint of label + indicator
 *          radio / checkbox
 *
 *  References: Notepad++ NppDarkMode.cpp, System Informer phlib/theme.c,
 *  ysc3839/win32-darkmode, ReactOS dll/win32/comctl32/hotkey.c.
 */

#include "theme_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>

/* ============================================================================
 * §1  Constants
 * ========================================================================== */

/* Win10 build numbers we branch on. */
#define WIN10_BUILD_1809  17763
#define WIN10_BUILD_1903  18362
#define WIN10_BUILD_2004  19041

/* DWM dark titlebar attribute (some SDKs do not define it). */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1 19   /* Win10 1903-1909 */

/* user32!SetWindowCompositionAttribute attribute (1903-1909 fallback). */
#define WCA_USEDARKMODECOLORS 26

#ifndef CLR_DEFAULT
#define CLR_DEFAULT ((COLORREF)0xFF000000)
#endif

/* uxtheme!SetPreferredAppMode (ord 135 on 1903+) values. */
enum {
    PREFERRED_APP_MODE_DEFAULT     = 0,
    PREFERRED_APP_MODE_ALLOW_DARK  = 1,  /* Dark-aware app, follow OS pref */
    PREFERRED_APP_MODE_FORCE_DARK  = 2,
    PREFERRED_APP_MODE_FORCE_LIGHT = 3,
};

/* MENU theme part / state IDs (vssym32.h equivalents, hardcoded to keep the
 * include surface small). The Notepad++ pattern uses these for menu bar
 * item rendering via DrawThemeBackground / DrawThemeTextEx. */
#define THEME_CLASS_MENU L"Menu"
#ifndef MENU_BARITEM
#define MENU_BARITEM        8
#endif
#ifndef MBI_NORMAL
#define MBI_NORMAL          1
#define MBI_HOT             2
#define MBI_PUSHED          3
#define MBI_DISABLED        4
#define MBI_DISABLEDHOT     5
#define MBI_DISABLEDPUSHED  6
#endif

/* BUTTON theme parts and states for radio + checkbox. */
#define BP_RADIOBUTTON          2
#define BP_CHECKBOX             3
#define RBS_UNCHECKEDNORMAL     1
#define RBS_UNCHECKEDHOT        2
#define RBS_UNCHECKEDPRESSED    3
#define RBS_UNCHECKEDDISABLED   4
#define RBS_CHECKEDNORMAL       5
#define RBS_CHECKEDHOT          6
#define RBS_CHECKEDPRESSED      7
#define RBS_CHECKEDDISABLED     8
#define CBS_UNCHECKEDNORMAL     1
#define CBS_UNCHECKEDHOT        2
#define CBS_UNCHECKEDPRESSED    3
#define CBS_UNCHECKEDDISABLED   4
#define CBS_CHECKEDNORMAL       5
#define CBS_CHECKEDHOT          6
#define CBS_CHECKEDPRESSED      7
#define CBS_CHECKEDDISABLED     8

/* Subclass IDs - one per control class we subclass. */
enum {
    STATUSBAR_SUBCLASS_ID = 0xBEEF1U,
    LISTVIEW_SUBCLASS_ID  = 0xBEEF2U,
    HOTKEY_SUBCLASS_ID    = 0xBEEF3U,
    EDIT_SUBCLASS_ID      = 0xBEEF4U,
    BUTTON_SUBCLASS_ID    = 0xBEEF5U,
};

/* ============================================================================
 * §2  UAH (User-mode App Hooks) — undocumented WM_UAH* parameter structs
 *
 *  Layout matches Notepad++'s PowerEditor/src/DarkMode/UAHMenuBar.h, which
 *  was reverse-engineered from running Windows. We use only a few fields
 *  but keep the full layout so sizeof matches what the OS expects.
 * ========================================================================== */

typedef union {
    struct { DWORD cx; DWORD cy; } rgsize_arr[5];
    struct { DWORD cx; DWORD cy; } rgsize;
} uah_menuitem_metrics_t;

typedef struct {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
} uah_menupopup_metrics_t;

typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } uah_menu_t;

typedef struct {
    int                       iPosition;
    uah_menuitem_metrics_t    umim;
    uah_menupopup_metrics_t   umpm;
} uah_menuitem_t;

typedef struct {
    DRAWITEMSTRUCT  dis;
    uah_menu_t      um;
    uah_menuitem_t  umi;
} uah_drawmenuitem_t;

/* ============================================================================
 * §3  Dynamic API typedefs and resolved pointers
 * ========================================================================== */

typedef bool    (WINAPI *fn_should_apps_use_dark_mode_t)(void);                 /* uxtheme ord 132 */
typedef bool    (WINAPI *fn_allow_dark_mode_for_window_t)(HWND, bool);          /* uxtheme ord 133 */
typedef bool    (WINAPI *fn_allow_dark_mode_for_app_t)(bool);                   /* uxtheme ord 135, 1809 */
typedef int     (WINAPI *fn_set_preferred_app_mode_t)(int);                     /* uxtheme ord 135, 1903+ */
typedef void    (WINAPI *fn_flush_menu_themes_t)(void);                         /* uxtheme ord 136 */
typedef void    (WINAPI *fn_refresh_immersive_color_policy_state_t)(void);      /* uxtheme ord 104 */

typedef struct {
    DWORD attribute;
    PVOID data;
    SIZE_T length;
} window_composition_attribute_data_t;
typedef BOOL    (WINAPI *fn_set_window_composition_attribute_t)(HWND, window_composition_attribute_data_t *);
typedef HRESULT (WINAPI *fn_dwm_set_window_attribute_t)(HWND, DWORD, LPCVOID, DWORD);

static struct {
    fn_should_apps_use_dark_mode_t           should_apps_use_dark_mode;
    fn_allow_dark_mode_for_window_t          allow_dark_mode_for_window;
    fn_allow_dark_mode_for_app_t             allow_dark_mode_for_app;
    fn_set_preferred_app_mode_t              set_preferred_app_mode;
    fn_flush_menu_themes_t                   flush_menu_themes;
    fn_refresh_immersive_color_policy_state_t refresh_immersive_color_policy_state;
    fn_set_window_composition_attribute_t    set_window_composition_attribute;
    fn_dwm_set_window_attribute_t            dwm_set_window_attribute;
    DWORD build_number;
    bool initialized;
} g_api;

/* ============================================================================
 * §4  Module state
 * ========================================================================== */

static theme_mode_t    g_mode = THEME_MODE_SYSTEM;
static bool            g_is_dark = false;     /* Resolved from g_mode + OS state */
static theme_palette_t g_dark_palette;
static theme_palette_t g_light_palette;
static HTHEME          g_menu_theme;          /* Cached on first UAH item paint */

/* Subclass procs - forward declared, defined in §10. */
static LRESULT CALLBACK statusbar_subclass_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK listview_subclass_proc (HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK hotkey_subclass_proc   (HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK edit_subclass_proc     (HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK button_subclass_proc   (HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

/* ============================================================================
 * §5  Color palette templates and brush management
 * ========================================================================== */

/* Notepad++ "black tone" palette - widely-tested production values. */
static const theme_palette_t k_dark_palette_template = {
    .bg            = RGB(0x20, 0x20, 0x20),  /* main window  */
    .ctrl_bg       = RGB(0x38, 0x38, 0x38),  /* edit / list  */
    .hot_bg        = RGB(0x45, 0x45, 0x45),  /* hover / sel  */
    .text          = RGB(0xE0, 0xE0, 0xE0),
    .text_disabled = RGB(0x80, 0x80, 0x80),
    .edge          = RGB(0x64, 0x64, 0x64),  /* borders      */
    .dlg_bg        = RGB(0x2B, 0x2B, 0x2B),  /* dialog body  */
};

static void release_palette_brushes(theme_palette_t *p) {
    HBRUSH *brushes[] = {
        &p->brush_bg, &p->brush_ctrl_bg, &p->brush_hot_bg,
        &p->brush_dlg_bg, &p->brush_edge,
    };
    for (size_t i = 0; i < sizeof(brushes) / sizeof(brushes[0]); i++) {
        if (*brushes[i]) {
            DeleteObject(*brushes[i]);
            *brushes[i] = NULL;
        }
    }
}

static void create_palette_brushes(theme_palette_t *p) {
    p->brush_bg      = CreateSolidBrush(p->bg);
    p->brush_ctrl_bg = CreateSolidBrush(p->ctrl_bg);
    p->brush_hot_bg  = CreateSolidBrush(p->hot_bg);
    p->brush_dlg_bg  = CreateSolidBrush(p->dlg_bg);
    p->brush_edge    = CreateSolidBrush(p->edge);
}

/* Refresh both palettes' brushes; the light palette also re-reads system
 * colors so it tracks the current Windows theme / accent. */
static void refresh_palettes(void) {
    release_palette_brushes(&g_light_palette);
    g_light_palette.bg            = GetSysColor(COLOR_WINDOW);
    g_light_palette.ctrl_bg       = GetSysColor(COLOR_WINDOW);
    g_light_palette.hot_bg        = GetSysColor(COLOR_HIGHLIGHT);
    g_light_palette.text          = GetSysColor(COLOR_WINDOWTEXT);
    g_light_palette.text_disabled = GetSysColor(COLOR_GRAYTEXT);
    g_light_palette.edge          = GetSysColor(COLOR_BTNSHADOW);
    g_light_palette.dlg_bg        = GetSysColor(COLOR_BTNFACE);
    create_palette_brushes(&g_light_palette);

    release_palette_brushes(&g_dark_palette);
    g_dark_palette = k_dark_palette_template;
    create_palette_brushes(&g_dark_palette);
}

static void drop_menu_theme(void) {
    if (g_menu_theme) {
        CloseThemeData(g_menu_theme);
        g_menu_theme = NULL;
    }
}

/* ============================================================================
 * §6  Internal helpers
 * ========================================================================== */

/* High-contrast mode forces light mode regardless of user choice. */
static bool is_high_contrast_active(void) {
    HIGHCONTRASTW hc = { sizeof(hc), 0, NULL };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)
        && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

/* AllowDarkModeForWindow boilerplate used by every theme_core_apply_to_*. */
static void apply_dark_attr(HWND hwnd) {
    if (g_api.allow_dark_mode_for_window) {
        g_api.allow_dark_mode_for_window(hwnd, g_is_dark);
    }
}

/* Resolve g_is_dark from g_mode plus OS state. */
static void resolve_effective_mode(void) {
    if (is_high_contrast_active()) {
        g_is_dark = false;
    } else if (g_mode == THEME_MODE_LIGHT) {
        g_is_dark = false;
    } else if (g_mode == THEME_MODE_DARK) {
        g_is_dark = true;
    } else {
        g_is_dark = g_api.should_apps_use_dark_mode
                 && g_api.should_apps_use_dark_mode();
    }
}

/* Push g_mode to the OS via SetPreferredAppMode (or AllowDarkModeForApp on
 * 1809). SYSTEM uses ALLOW_DARK so the OS still drives popup menus when the
 * user toggles the system theme later. */
static void apply_os_preferred_mode(void) {
    bool high_contrast = is_high_contrast_active();

    if (g_api.set_preferred_app_mode) {
        int os_mode;
        switch (g_mode) {
        case THEME_MODE_LIGHT: os_mode = PREFERRED_APP_MODE_FORCE_LIGHT; break;
        case THEME_MODE_DARK:  os_mode = PREFERRED_APP_MODE_FORCE_DARK;  break;
        default:               os_mode = PREFERRED_APP_MODE_ALLOW_DARK;  break;
        }
        if (high_contrast) {
            os_mode = PREFERRED_APP_MODE_FORCE_LIGHT;
        }
        g_api.set_preferred_app_mode(os_mode);
    } else if (g_api.allow_dark_mode_for_app) {
        g_api.allow_dark_mode_for_app(!high_contrast && g_mode != THEME_MODE_LIGHT);
    }
    if (g_api.refresh_immersive_color_policy_state) {
        g_api.refresh_immersive_color_policy_state();
    }
}

/* Set the DWM dark titlebar attribute, picking the right method for the
 * Win10 build. Falls back through three undocumented paths for 1903 / 1809. */
static void set_dwm_dark_titlebar(HWND hwnd, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    if (g_api.dwm_set_window_attribute && g_api.build_number >= WIN10_BUILD_2004) {
        g_api.dwm_set_window_attribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
    } else if (g_api.dwm_set_window_attribute && g_api.build_number >= WIN10_BUILD_1903) {
        g_api.dwm_set_window_attribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_PRE_20H1, &value, sizeof(value));
    } else if (g_api.set_window_composition_attribute && g_api.build_number >= WIN10_BUILD_1903) {
        window_composition_attribute_data_t data = { WCA_USEDARKMODECOLORS, &value, sizeof(value) };
        g_api.set_window_composition_attribute(hwnd, &data);
    } else if (g_api.build_number >= WIN10_BUILD_1809) {
        SetPropW(hwnd, L"UseImmersiveDarkModeColors", (HANDLE)(intptr_t)value);
    }
}

/* True for BUTTON subtypes whose label text the BUTTON proc paints with
 * hardcoded GetSysColor (independent of WM_CTLCOLOR* and SetWindowTheme). */
static bool is_label_button(LONG btn_type) {
    return btn_type == BS_GROUPBOX
        || btn_type == BS_RADIOBUTTON   || btn_type == BS_AUTORADIOBUTTON
        || btn_type == BS_CHECKBOX      || btn_type == BS_AUTOCHECKBOX;
}

/* ============================================================================
 * §7  Init / cleanup
 * ========================================================================== */

/* RtlGetNtVersionNumbers in ntdll - GetVersionEx lies under app-compat. */
static DWORD detect_win10_build(void) {
    typedef void (WINAPI *fn_t)(LPDWORD, LPDWORD, LPDWORD);
    HMODULE hntdll = GetModuleHandleW(L"ntdll.dll");
    if (!hntdll) return 0;
    fn_t fn = (fn_t)(void *)GetProcAddress(hntdll, "RtlGetNtVersionNumbers");
    if (!fn) return 0;
    DWORD major = 0, minor = 0, build = 0;
    fn(&major, &minor, &build);
    return build & ~0xF0000000u;  /* strip the non-release-build flag */
}

static void load_dynamic_apis(void) {
    /* uxtheme.dll undocumented ordinals (Win10 1809+) */
    if (g_api.build_number >= WIN10_BUILD_1809) {
        HMODULE hux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (hux) {
            g_api.should_apps_use_dark_mode  = (fn_should_apps_use_dark_mode_t)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(132));
            g_api.allow_dark_mode_for_window = (fn_allow_dark_mode_for_window_t)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(133));
            FARPROC ord135 = GetProcAddress(hux, MAKEINTRESOURCEA(135));
            if (g_api.build_number >= WIN10_BUILD_1903) {
                g_api.set_preferred_app_mode = (fn_set_preferred_app_mode_t)(void *)ord135;
            } else {
                g_api.allow_dark_mode_for_app = (fn_allow_dark_mode_for_app_t)(void *)ord135;
            }
            g_api.flush_menu_themes                    = (fn_flush_menu_themes_t)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(136));
            g_api.refresh_immersive_color_policy_state = (fn_refresh_immersive_color_policy_state_t)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(104));
            /* hux is intentionally not freed - the resolved pointers must stay valid. */
        }
    }
    HMODULE huser = GetModuleHandleW(L"user32.dll");
    if (huser) {
        g_api.set_window_composition_attribute =
            (fn_set_window_composition_attribute_t)(void *)GetProcAddress(huser, "SetWindowCompositionAttribute");
    }
    HMODULE hdwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hdwm) {
        g_api.dwm_set_window_attribute =
            (fn_dwm_set_window_attribute_t)(void *)GetProcAddress(hdwm, "DwmSetWindowAttribute");
    }
}

void theme_core_init(void) {
    if (g_api.initialized) return;
    g_api.build_number = detect_win10_build();
    load_dynamic_apis();
    g_dark_palette = k_dark_palette_template;  /* colors only; brushes via set_mode */
    g_api.initialized = true;
}

void theme_core_cleanup(void) {
    release_palette_brushes(&g_dark_palette);
    release_palette_brushes(&g_light_palette);
    drop_menu_theme();
}

/* ============================================================================
 * §8  Mode management
 * ========================================================================== */

void theme_core_set_mode(theme_mode_t mode) {
    if (!g_api.initialized) theme_core_init();
    g_mode = mode;

    /* CRITICAL ORDERING: SetPreferredAppMode must run BEFORE we query
     * ShouldAppsUseDarkMode in resolve_effective_mode(). The query reflects
     * the CURRENT preferred app mode - if we resolve first and the previous
     * mode was FORCE_LIGHT, ShouldAppsUseDarkMode returns false even when
     * the OS user-preference is dark, breaking SYSTEM's follow-OS path. */
    apply_os_preferred_mode();
    resolve_effective_mode();
    refresh_palettes();
    drop_menu_theme();          /* Re-opened next paint with new theme */
    if (g_api.flush_menu_themes) g_api.flush_menu_themes();
}

theme_mode_t            theme_core_get_mode(void) { return g_mode; }
bool                    theme_core_is_dark(void)  { return g_is_dark; }
const theme_palette_t * theme_core_palette(void)  {
    return g_is_dark ? &g_dark_palette : &g_light_palette;
}

/* ============================================================================
 * §9  Per-control appliers
 * ========================================================================== */

void theme_core_apply_to_window(HWND hwnd) {
    if (!hwnd || !g_api.initialized) return;
    apply_dark_attr(hwnd);
    set_dwm_dark_titlebar(hwnd, g_is_dark);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void theme_core_apply_to_treeview(HWND hwnd) {
    if (!hwnd) return;
    apply_dark_attr(hwnd);
    SetWindowTheme(hwnd, g_is_dark ? L"DarkMode_Explorer" : NULL, NULL);

    const theme_palette_t *p = theme_core_palette();
    if (g_is_dark) {
        TreeView_SetBkColor  (hwnd, p->ctrl_bg);
        TreeView_SetTextColor(hwnd, p->text);
        TreeView_SetLineColor(hwnd, p->edge);
    } else {
        TreeView_SetBkColor  (hwnd, (COLORREF)-1);
        TreeView_SetTextColor(hwnd, (COLORREF)-1);
        TreeView_SetLineColor(hwnd, CLR_DEFAULT);
    }
}

void theme_core_apply_to_listview(HWND hwnd) {
    if (!hwnd) return;
    apply_dark_attr(hwnd);
    SetWindowTheme(hwnd, g_is_dark ? L"DarkMode_Explorer" : NULL, NULL);

    HWND header = ListView_GetHeader(hwnd);
    if (header) {
        apply_dark_attr(header);
        SetWindowTheme(header, g_is_dark ? L"ItemsView" : NULL, NULL);
        InvalidateRect(header, NULL, TRUE);
    }

    /* Enable LVS_EX_DOUBLEBUFFER so the listview renders WM_PAINT to an
     * offscreen DC and BitBlts the result to the screen each frame. The
     * BitBlt unconditionally overwrites whatever pixels are currently on
     * screen — including XOR residue left by the OS-drawn column-divider
     * tracking line during a column resize, which is otherwise highly
     * visible as a "ghost" vertical line on the dark background. The flag
     * is harmless in light mode and incidentally reduces flicker during
     * any normal repaint, so it is applied unconditionally. The Ex variant
     * with a mask preserves any other extended styles already set by the
     * caller (e.g. LVS_EX_FULLROWSELECT, LVS_EX_GRIDLINES). */
    ListView_SetExtendedListViewStyleEx(hwnd, LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);

    const theme_palette_t *p = theme_core_palette();
    if (g_is_dark) {
        ListView_SetBkColor    (hwnd, p->ctrl_bg);
        ListView_SetTextBkColor(hwnd, p->ctrl_bg);
        ListView_SetTextColor  (hwnd, p->text);
    } else {
        ListView_SetBkColor    (hwnd, CLR_DEFAULT);
        ListView_SetTextBkColor(hwnd, CLR_DEFAULT);
        ListView_SetTextColor  (hwnd, CLR_DEFAULT);
    }
    /* Subclass intercepts the header's NM_CUSTOMDRAW (which goes to the
     * ListView, not the dialog) so we can paint header text in our color. */
    SetWindowSubclass(hwnd, listview_subclass_proc, LISTVIEW_SUBCLASS_ID, 0);
}

void theme_core_apply_to_combobox(HWND hwnd) {
    if (!hwnd) return;
    apply_dark_attr(hwnd);
    SetWindowTheme(hwnd, g_is_dark ? L"DarkMode_CFD" : NULL, NULL);
}

void theme_core_apply_to_statusbar(HWND hwnd) {
    if (!hwnd) return;
    apply_dark_attr(hwnd);
    SetWindowTheme(hwnd, g_is_dark ? L"" : NULL, NULL);
    SetWindowSubclass(hwnd, statusbar_subclass_proc, STATUSBAR_SUBCLASS_ID, 0);
    InvalidateRect(hwnd, NULL, TRUE);
}

void theme_core_apply_to_button(HWND hwnd) {
    if (!hwnd) return;
    apply_dark_attr(hwnd);

    LONG btn_type = GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    bool needs_subclass = is_label_button(btn_type);

    if (needs_subclass) {
        SetWindowSubclass(hwnd, button_subclass_proc, BUTTON_SUBCLASS_ID, 0);
    }

    /* Theme name selection:
     *   light:           NULL          - restore system theme
     *   dark + label:    L""           - subclass owns all painting
     *   dark + push:     L"Explorer"   - visual style draws dark variant */
    const wchar_t *theme_name;
    if (!g_is_dark)          theme_name = NULL;
    else if (needs_subclass) theme_name = L"";
    else                     theme_name = L"Explorer";
    SetWindowTheme(hwnd, theme_name, NULL);

    if (needs_subclass) InvalidateRect(hwnd, NULL, TRUE);
}

/* EnumChildWindows callback - dispatch each child to the right applier. */
static BOOL CALLBACK theme_child_proc(HWND hwnd, LPARAM lparam) {
    (void)lparam;
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) == 0) return TRUE;

    if      (lstrcmpiW(cls, WC_TREEVIEWW)        == 0) theme_core_apply_to_treeview(hwnd);
    else if (lstrcmpiW(cls, WC_LISTVIEWW)        == 0) theme_core_apply_to_listview(hwnd);
    else if (lstrcmpiW(cls, WC_COMBOBOXW)        == 0) theme_core_apply_to_combobox(hwnd);
    else if (lstrcmpiW(cls, STATUSCLASSNAMEW)    == 0) theme_core_apply_to_statusbar(hwnd);
    else if (lstrcmpiW(cls, L"BUTTON")           == 0) theme_core_apply_to_button(hwnd);
    else if (lstrcmpiW(cls, L"msctls_hotkey32")  == 0) {
        /* HOTKEY ignores SetWindowTheme + WM_CTLCOLOR* - subclass owns paint. */
        apply_dark_attr(hwnd);
        SetWindowSubclass(hwnd, hotkey_subclass_proc, HOTKEY_SUBCLASS_ID, 0);
    } else if (lstrcmpiW(cls, L"EDIT") == 0) {
        /* EDIT bg/text via WM_CTLCOLOREDIT in parent dialog. The subclass
         * overrides WM_NCPAINT for a flat 1px border. RDW_FRAME triggers a
         * non-client repaint without WM_NCCALCSIZE (which would reset the
         * EDIT's vertical text alignment). */
        apply_dark_attr(hwnd);
        SetWindowSubclass(hwnd, edit_subclass_proc, EDIT_SUBCLASS_ID, 0);
        RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_NOCHILDREN);
    } else if (lstrcmpiW(cls, L"STATIC")  == 0
            || lstrcmpiW(cls, L"LISTBOX") == 0) {
        /* Pure WM_CTLCOLOR* clients - just opt in. */
        apply_dark_attr(hwnd);
    }
    InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

void theme_core_apply_to_window_and_children(HWND hwnd) {
    if (!hwnd) return;
    /* Top-level + WS_POPUP windows get the DWM titlebar treatment. */
    if (!GetParent(hwnd) || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP)) {
        theme_core_apply_to_window(hwnd);
    }
    EnumChildWindows(hwnd, theme_child_proc, 0);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

/* ============================================================================
 * §10  Message handlers (WM_CTLCOLOR*, NM_CUSTOMDRAW, WM_ERASEBKGND)
 * ========================================================================== */

HBRUSH theme_core_on_ctlcolor(HDC hdc, UINT msg) {
    if (!g_is_dark || !hdc) return NULL;
    const theme_palette_t *p = &g_dark_palette;
    SetTextColor(hdc, p->text);
    SetBkMode(hdc, OPAQUE);
    switch (msg) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor(hdc, p->dlg_bg);
        return p->brush_dlg_bg;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor(hdc, p->ctrl_bg);
        return p->brush_ctrl_bg;
    default:
        SetBkColor(hdc, p->bg);
        return p->brush_bg;
    }
}

INT_PTR theme_core_dlg_ctlcolor(HDC hdc, UINT msg) {
    if (!g_is_dark || !hdc) return 0;
    return (INT_PTR)theme_core_on_ctlcolor(hdc, msg);
}

bool theme_core_on_erasebkgnd(HWND hwnd, HDC hdc) {
    if (!hwnd || !hdc) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, theme_core_palette()->brush_dlg_bg);
    return true;
}

LRESULT theme_core_on_treeview_customdraw(LPNMTVCUSTOMDRAW nmtv) {
    if (!g_is_dark || !nmtv) return CDRF_DODEFAULT;
    const theme_palette_t *p = &g_dark_palette;
    switch (nmtv->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        nmtv->clrText = p->text;
        if (nmtv->nmcd.uItemState & CDIS_SELECTED) {
            nmtv->clrTextBk = p->hot_bg;
            FillRect(nmtv->nmcd.hdc, &nmtv->nmcd.rc, p->brush_hot_bg);
        } else if (nmtv->nmcd.uItemState & CDIS_HOT) {
            nmtv->clrTextBk = p->hot_bg;
        } else {
            nmtv->clrTextBk = p->ctrl_bg;
        }
        return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}

LRESULT theme_core_on_listview_customdraw(LPNMLVCUSTOMDRAW nmlv) {
    if (!g_is_dark || !nmlv) return CDRF_DODEFAULT;
    const theme_palette_t *p = &g_dark_palette;
    switch (nmlv->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        bool selected = false;
        if (nmlv->nmcd.hdr.hwndFrom) {
            UINT state = ListView_GetItemState(nmlv->nmcd.hdr.hwndFrom,
                                               (int)nmlv->nmcd.dwItemSpec, LVIS_SELECTED);
            selected = (state & LVIS_SELECTED) != 0;
        }
        nmlv->clrText   = p->text;
        nmlv->clrTextBk = selected ? p->hot_bg : p->ctrl_bg;
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

LRESULT theme_core_on_toolbar_customdraw(LPNMTBCUSTOMDRAW nmtb) {
    if (!g_is_dark || !nmtb) return CDRF_DODEFAULT;
    const theme_palette_t *p = &g_dark_palette;
    switch (nmtb->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        FillRect(nmtb->nmcd.hdc, &nmtb->nmcd.rc, p->brush_dlg_bg);
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        nmtb->clrText              = p->text;
        nmtb->clrTextHighlight     = p->text;
        nmtb->clrBtnFace           = p->dlg_bg;
        nmtb->clrBtnHighlight      = p->ctrl_bg;
        nmtb->clrHighlightHotTrack = p->hot_bg;
        nmtb->nStringBkMode        = TRANSPARENT;
        if (nmtb->nmcd.uItemState & (CDIS_HOT | CDIS_SELECTED)) {
            FillRect(nmtb->nmcd.hdc, &nmtb->nmcd.rc, p->brush_hot_bg);
        }
        return TBCDRF_USECDCOLORS | CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}

/* ============================================================================
 * §11  Subclasses
 *
 *  Common pattern (factored into SUBCLASS_PROLOGUE): on WM_NCDESTROY remove
 *  the subclass; in light mode pass every message through unchanged.
 * ========================================================================== */

#define SUBCLASS_PROLOGUE(proc, id)                                             \
    do {                                                                        \
        if (msg == WM_NCDESTROY) {                                              \
            RemoveWindowSubclass(hwnd, (proc), (id));                           \
            return DefSubclassProc(hwnd, msg, wp, lp);                          \
        }                                                                       \
        if (!g_is_dark) return DefSubclassProc(hwnd, msg, wp, lp);              \
    } while (0)

/* --- Status bar ----------------------------------------------------------- */

static LRESULT CALLBACK statusbar_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    SUBCLASS_PROLOGUE(statusbar_subclass_proc, STATUSBAR_SUBCLASS_ID);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  /* WM_PAINT fills */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc) return 0;

        const theme_palette_t *p = &g_dark_palette;
        RECT client_rc;
        GetClientRect(hwnd, &client_rc);
        FillRect(hdc, &client_rc, p->brush_dlg_bg);

        int parts = (int)DefSubclassProc(hwnd, SB_GETPARTS, 0, 0);
        if (parts <= 0) parts = 1;

        HFONT hf = (HFONT)DefSubclassProc(hwnd, WM_GETFONT, 0, 0);
        HFONT old_font = hf ? (HFONT)SelectObject(hdc, hf) : NULL;
        SetTextColor(hdc, p->text);
        SetBkMode(hdc, TRANSPARENT);

        for (int i = 0; i < parts; i++) {
            RECT pr;
            DefSubclassProc(hwnd, SB_GETRECT, (WPARAM)i, (LPARAM)&pr);
            wchar_t buf[256] = { 0 };
            LRESULT len_word = DefSubclassProc(hwnd, SB_GETTEXTLENGTHW, (WPARAM)i, 0);
            int text_len = (int)LOWORD(len_word);
            if (text_len > 0 && text_len < 256) {
                DefSubclassProc(hwnd, SB_GETTEXTW, (WPARAM)i, (LPARAM)buf);
            }
            if (buf[0] != L'\0') {
                RECT text_rc = pr;
                InflateRect(&text_rc, -2, 0);
                DrawTextW(hdc, buf, -1, &text_rc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        if (old_font) SelectObject(hdc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* --- ListView (header NM_CUSTOMDRAW only) --------------------------------- */

static LRESULT CALLBACK listview_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                               UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    SUBCLASS_PROLOGUE(listview_subclass_proc, LISTVIEW_SUBCLASS_ID);

    if (msg == WM_NOTIFY) {
        NMHDR *nmhdr = (NMHDR *)lp;
        if (nmhdr && nmhdr->code == NM_CUSTOMDRAW) {
            HWND header = (HWND)SendMessageW(hwnd, LVM_GETHEADER, 0, 0);
            if (header && nmhdr->hwndFrom == header) {
                LPNMCUSTOMDRAW nmcd = (LPNMCUSTOMDRAW)lp;
                switch (nmcd->dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    SetTextColor(nmcd->hdc, g_dark_palette.text);
                    SetBkMode(nmcd->hdc, TRANSPARENT);
                    return CDRF_DODEFAULT;
                }
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* --- HOTKEY (msctls_hotkey32) --------------------------------------------- */

/* Format an HKM_GETHOTKEY value (vk in low byte, HOTKEYF_* mods in high byte)
 * into "Ctrl+Shift+F5" etc. Returns character count (0 when vk == 0).
 * WM_GETTEXT forwarding is not reliable on real comctl32, so we format here. */
static int format_hotkey_value(WORD packed, wchar_t *out, int out_chars) {
    if (!out || out_chars < 2) return 0;
    out[0] = L'\0';
    BYTE vk   = LOBYTE(packed);
    BYTE mods = HIBYTE(packed);
    if (vk == 0) return 0;

    static const struct { BYTE flag; const wchar_t *prefix; int len; } k_mods[] = {
        { HOTKEYF_CONTROL, L"Ctrl+",  5 },
        { HOTKEYF_SHIFT,   L"Shift+", 6 },
        { HOTKEYF_ALT,     L"Alt+",   4 },
    };
    int len = 0;
    for (size_t i = 0; i < sizeof(k_mods) / sizeof(k_mods[0]); i++) {
        if ((mods & k_mods[i].flag) && (out_chars - len) > k_mods[i].len + 1) {
            lstrcpyW(out + len, k_mods[i].prefix);
            len += k_mods[i].len;
        }
    }

    /* lParam encodes scan code (high word) + bit 24 for extended keys
     * (navigation cluster, numpad division, etc.) */
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lparam = (LONG)(scan << 16);
    if ((mods & HOTKEYF_EXT)
        || vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME  || vk == VK_END
        || vk == VK_PRIOR  || vk == VK_NEXT
        || vk == VK_LEFT   || vk == VK_RIGHT  || vk == VK_UP    || vk == VK_DOWN
        || vk == VK_DIVIDE || vk == VK_NUMLOCK) {
        lparam |= 0x01000000L;
    }
    wchar_t key_name[64];
    int key_len = GetKeyNameTextW(lparam, key_name, 64);
    if (key_len <= 0) {
        wsprintfW(key_name, L"VK_%02X", vk);
        key_len = lstrlenW(key_name);
    }
    if ((out_chars - len) > key_len) {
        lstrcpyW(out + len, key_name);
        len += key_len;
    }
    return len;
}

/* Show held modifiers as in-progress feedback when the control has focus
 * but no committed value (HKM_GETHOTKEY returns 0). The OS HOTKEY does this
 * via undocumented internal state, so we read GetKeyState directly. */
static int format_held_modifiers(wchar_t *out, int out_chars) {
    if (!out || out_chars < 2) return 0;
    out[0] = L'\0';
    static const struct { int vk; const wchar_t *prefix; int len; } k_keys[] = {
        { VK_CONTROL, L"Ctrl+",  5 },
        { VK_SHIFT,   L"Shift+", 6 },
        { VK_MENU,    L"Alt+",   4 },
    };
    int len = 0;
    for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if ((GetKeyState(k_keys[i].vk) & 0x8000) && (out_chars - len) > k_keys[i].len + 1) {
            lstrcpyW(out + len, k_keys[i].prefix);
            len += k_keys[i].len;
        }
    }
    return len;
}

static LRESULT CALLBACK hotkey_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    SUBCLASS_PROLOGUE(hotkey_subclass_proc, HOTKEY_SUBCLASS_ID);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        /* Repaint on every key event so held-modifier feedback is live. */
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc) return 0;

        const theme_palette_t *p = &g_dark_palette;
        RECT client_rc;
        GetClientRect(hwnd, &client_rc);
        FillRect(hdc, &client_rc, p->brush_ctrl_bg);

        HFONT hf = (HFONT)DefSubclassProc(hwnd, WM_GETFONT, 0, 0);
        HFONT old_font = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

        WORD packed = (WORD)DefSubclassProc(hwnd, HKM_GETHOTKEY, 0, 0);
        wchar_t buf[128];
        int len = format_hotkey_value(packed, buf, 128);
        if (len == 0 && GetFocus() == hwnd) {
            len = format_held_modifiers(buf, 128);
        }

        SetTextColor(hdc, p->text);
        SetBkMode(hdc, TRANSPARENT);
        RECT text_rc = client_rc;
        InflateRect(&text_rc, -3, 0);
        SIZE drawn = { 0, 0 };
        if (len > 0) {
            DrawTextW(hdc, buf, len, &text_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            GetTextExtentPoint32W(hdc, buf, len, &drawn);
        }

        /* Position the (default-proc-created) caret at our text end so it
         * lines up with what we render. */
        if (GetFocus() == hwnd) {
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            SetCaretPos(text_rc.left + drawn.cx,
                        (client_rc.top + client_rc.bottom - tm.tmHeight) / 2);
        }

        FrameRect(hdc, &client_rc, p->brush_edge);

        if (old_font) SelectObject(hdc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* --- EDIT (flat 1px modern border) ---------------------------------------- */

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    SUBCLASS_PROLOGUE(edit_subclass_proc, EDIT_SUBCLASS_ID);

    if (msg != WM_NCPAINT) return DefSubclassProc(hwnd, msg, wp, lp);

    /* Fully override WM_NCPAINT. The earlier text-alignment regression came
     * from triggering WM_NCCALCSIZE (via SWP_FRAMECHANGED), not from
     * skipping the default NC paint. The install path now uses
     * RedrawWindow(RDW_FRAME) which avoids WM_NCCALCSIZE. */
    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) return 0;

    const theme_palette_t *p = &g_dark_palette;
    RECT win_rc;
    GetWindowRect(hwnd, &win_rc);
    OffsetRect(&win_rc, -win_rc.left, -win_rc.top);

    /* Find client area in window-local coords. */
    POINT client_origin = { 0, 0 };
    MapWindowPoints(hwnd, NULL, &client_origin, 1);
    RECT win_screen_rc;
    GetWindowRect(hwnd, &win_screen_rc);
    int border_l = client_origin.x - win_screen_rc.left;
    int border_t = client_origin.y - win_screen_rc.top;
    RECT client_in_win;
    GetClientRect(hwnd, &client_in_win);
    OffsetRect(&client_in_win, border_l, border_t);

    /* Erase the NC ring (excluding client) with the EDIT's interior bg so
     * no sunken-edge pixels leak through. WM_CTLCOLOREDIT in the parent
     * dialog uses brush_ctrl_bg, so border + interior match seamlessly. */
    HRGN nc_clip = CreateRectRgnIndirect(&client_in_win);
    ExtSelectClipRgn(hdc, nc_clip, RGN_DIFF);
    FillRect(hdc, &win_rc, p->brush_ctrl_bg);
    SelectClipRgn(hdc, NULL);
    DeleteObject(nc_clip);

    FrameRect(hdc, &win_rc, p->brush_edge);

    ReleaseDC(hwnd, hdc);
    return 0;
}

/* --- BUTTON (groupbox / radio / checkbox) --------------------------------- */

static void paint_groupbox_dark(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;

    const theme_palette_t *p = &g_dark_palette;
    RECT rc;
    GetClientRect(hwnd, &rc);

    HFONT hf = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HFONT old_font = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

    wchar_t text[128];
    int text_len = GetWindowTextW(hwnd, text, 128);
    SIZE text_size = { 0, 0 };
    if (text_len > 0) GetTextExtentPoint32W(hdc, text, text_len, &text_size);

    /* Erase, then draw frame with top edge offset down so label sits on it. */
    FillRect(hdc, &rc, p->brush_dlg_bg);
    RECT frame_rc = rc;
    frame_rc.top += text_size.cy / 2;
    FrameRect(hdc, &frame_rc, p->brush_edge);

    if (text_len > 0) {
        const int LABEL_X = 8, LABEL_PAD = 4;
        RECT erase_rc = { LABEL_X, 0,
                          LABEL_X + text_size.cx + LABEL_PAD * 2,
                          text_size.cy + 1 };
        FillRect(hdc, &erase_rc, p->brush_dlg_bg);

        SetTextColor(hdc, p->text);
        SetBkMode(hdc, TRANSPARENT);
        RECT text_rc = { LABEL_X + LABEL_PAD, 0,
                         LABEL_X + LABEL_PAD + text_size.cx,
                         text_size.cy };
        DrawTextW(hdc, text, text_len, &text_rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (old_font) SelectObject(hdc, old_font);
    EndPaint(hwnd, &ps);
}

static void paint_radio_check_dark(HWND hwnd, BOOL is_radio) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;

    const theme_palette_t *p = &g_dark_palette;
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, p->brush_dlg_bg);

    BOOL checked  = (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL disabled = !IsWindowEnabled(hwnd);
    UINT button_state = (UINT)SendMessageW(hwnd, BM_GETSTATE, 0, 0);
    BOOL pressed = (button_state & BST_PUSHED) != 0;
    BOOL hot = FALSE;
    if (!disabled) {
        POINT cursor;
        if (GetCursorPos(&cursor)) {
            hot = (WindowFromPoint(cursor) == hwnd);
        }
    }

    int ind_size = GetSystemMetrics(SM_CXMENUCHECK);
    if (ind_size <= 0) ind_size = 13;
    RECT ind_rc = {
        1,
        (rc.top + rc.bottom - ind_size) / 2,
        1 + ind_size,
        (rc.top + rc.bottom + ind_size) / 2,
    };

    HTHEME ht = OpenThemeData(hwnd, L"Button");
    if (ht) {
        int part = is_radio ? BP_RADIOBUTTON : BP_CHECKBOX;
        int state;
        if (is_radio) {
            if (checked) {
                state = disabled ? RBS_CHECKEDDISABLED
                      : pressed  ? RBS_CHECKEDPRESSED
                      : hot      ? RBS_CHECKEDHOT
                                 : RBS_CHECKEDNORMAL;
            } else {
                state = disabled ? RBS_UNCHECKEDDISABLED
                      : pressed  ? RBS_UNCHECKEDPRESSED
                      : hot      ? RBS_UNCHECKEDHOT
                                 : RBS_UNCHECKEDNORMAL;
            }
        } else {
            if (checked) {
                state = disabled ? CBS_CHECKEDDISABLED
                      : pressed  ? CBS_CHECKEDPRESSED
                      : hot      ? CBS_CHECKEDHOT
                                 : CBS_CHECKEDNORMAL;
            } else {
                state = disabled ? CBS_UNCHECKEDDISABLED
                      : pressed  ? CBS_UNCHECKEDPRESSED
                      : hot      ? CBS_UNCHECKEDHOT
                                 : CBS_UNCHECKEDNORMAL;
            }
        }
        DrawThemeBackground(ht, hdc, part, state, &ind_rc, NULL);
        CloseThemeData(ht);
    } else {
        UINT df_state = is_radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK;
        if (checked)  df_state |= DFCS_CHECKED;
        if (disabled) df_state |= DFCS_INACTIVE;
        DrawFrameControl(hdc, &ind_rc, DFC_BUTTON, df_state);
    }

    wchar_t text[128];
    int len = GetWindowTextW(hwnd, text, 128);
    if (len > 0) {
        HFONT hf = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
        HFONT old_font = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

        RECT text_rc = { ind_rc.right + 4, rc.top, rc.right, rc.bottom };
        SetTextColor(hdc, disabled ? p->text_disabled : p->text);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, text, len, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (!disabled && GetFocus() == hwnd) {
            SIZE text_size = { 0, 0 };
            RECT focus_rc;

            GetTextExtentPoint32W(hdc, text, len, &text_size);
            focus_rc.left = text_rc.left - 2;
            focus_rc.top = (rc.top + rc.bottom - text_size.cy) / 2 - 1;
            focus_rc.right = text_rc.left + text_size.cx + 2;
            focus_rc.bottom = focus_rc.top + text_size.cy + 2;
            if (focus_rc.right > text_rc.right) {
                focus_rc.right = text_rc.right;
            }
            DrawFocusRect(hdc, &focus_rc);
        }

        if (old_font) SelectObject(hdc, old_font);
    }

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK button_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR uid, DWORD_PTR ref) {
    (void)uid; (void)ref;
    SUBCLASS_PROLOGUE(button_subclass_proc, BUTTON_SUBCLASS_ID);

    LONG btn_type = GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    if (!is_label_button(btn_type)) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    case WM_MOUSELEAVE:
        InvalidateRect(hwnd, NULL, FALSE);
        return DefSubclassProc(hwnd, msg, wp, lp);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case BM_SETCHECK:
    case BM_SETSTATE:
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
    }
    case WM_PAINT:
        if (btn_type == BS_GROUPBOX) {
            paint_groupbox_dark(hwnd);
        } else {
            BOOL is_radio = (btn_type == BS_RADIOBUTTON || btn_type == BS_AUTORADIOBUTTON);
            paint_radio_check_dark(hwnd, is_radio);
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ============================================================================
 * §12  UAH menu bar painter
 *
 *  Rendering follows Notepad++'s NppDarkMode.cpp:
 *    - WM_UAHDRAWMENU      -> fill menu bar with dlg_bg
 *    - WM_UAHDRAWMENUITEM  -> per-item bg + DrawThemeTextEx for label text
 *    - WM_UAHMEASUREMENUITEM is intentionally NOT handled: DefWindowProc
 *      must run to fill mis.itemWidth/itemHeight. Intercepting it produces
 *      zero-sized items and the menu visually disappears.
 * ========================================================================== */

bool theme_core_on_uah_drawmenu(HWND hwnd, LPARAM lp) {
    if (!g_is_dark) return false;
    uah_menu_t *pudm = (uah_menu_t *)lp;
    if (!pudm || !pudm->hdc) return false;

    MENUBARINFO mbi = { sizeof(mbi), 0 };
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return false;
    RECT rcwin;
    if (!GetWindowRect(hwnd, &rcwin)) return false;
    RECT rcbar = mbi.rcBar;
    OffsetRect(&rcbar, -rcwin.left, -rcwin.top);

    FillRect(pudm->hdc, &rcbar, g_dark_palette.brush_dlg_bg);
    return true;
}

bool theme_core_on_uah_drawmenuitem(HWND hwnd, LPARAM lp) {
    if (!g_is_dark) return false;
    uah_drawmenuitem_t *pudmi = (uah_drawmenuitem_t *)lp;
    if (!pudmi || !pudmi->um.hdc) return false;

    /* Fetch caption. */
    wchar_t menu_string[256] = { 0 };
    MENUITEMINFOW mii = { 0 };
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = menu_string;
    mii.cch = (sizeof(menu_string) / sizeof(menu_string[0])) - 1;
    if (!GetMenuItemInfoW(pudmi->um.hmenu, (UINT)pudmi->umi.iPosition, TRUE, &mii)) {
        return false;
    }

    /* Map ODS_* state -> MENU_BARITEM theme state. */
    DWORD st = pudmi->dis.itemState;
    DWORD dt_flags  = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    int   text_state = MBI_NORMAL;
    int   bg_state   = MBI_NORMAL;
    if (st & ODS_HOTLIGHT) { text_state = bg_state = MBI_HOT; }
    if (st & ODS_SELECTED) { text_state = bg_state = MBI_PUSHED; }
    if (st & (ODS_GRAYED | ODS_DISABLED)) { text_state = bg_state = MBI_DISABLED; }
    if (st & ODS_NOACCEL) dt_flags |= DT_HIDEPREFIX;

    if (!g_menu_theme) g_menu_theme = OpenThemeData(hwnd, THEME_CLASS_MENU);

    /* Background. */
    const theme_palette_t *p = &g_dark_palette;
    HBRUSH bg_brush = NULL;
    switch (bg_state) {
    case MBI_NORMAL: case MBI_DISABLED:        bg_brush = p->brush_dlg_bg;  break;
    case MBI_HOT:    case MBI_DISABLEDHOT:     bg_brush = p->brush_hot_bg;  break;
    case MBI_PUSHED: case MBI_DISABLEDPUSHED:  bg_brush = p->brush_ctrl_bg; break;
    }
    if (bg_brush) {
        FillRect(pudmi->um.hdc, &pudmi->dis.rcItem, bg_brush);
    } else if (g_menu_theme) {
        DrawThemeBackground(g_menu_theme, pudmi->um.hdc, MENU_BARITEM, bg_state,
                            &pudmi->dis.rcItem, NULL);
    }

    /* Text via DrawThemeTextEx + DTT_TEXTCOLOR. DrawTextW + manual SetTextColor
     * produces invisible text on some Windows builds, so the theme path is
     * required when available. */
    DTTOPTS dtto = { 0 };
    dtto.dwSize = sizeof(dtto);
    dtto.dwFlags = DTT_TEXTCOLOR;
    bool active = (text_state == MBI_NORMAL || text_state == MBI_HOT || text_state == MBI_PUSHED);
    dtto.crText = active ? p->text : p->text_disabled;

    if (g_menu_theme) {
        DrawThemeTextEx(g_menu_theme, pudmi->um.hdc, MENU_BARITEM, text_state,
                        menu_string, -1, dt_flags, &pudmi->dis.rcItem, &dtto);
    } else {
        SetTextColor(pudmi->um.hdc, dtto.crText);
        SetBkMode(pudmi->um.hdc, TRANSPARENT);
        DrawTextW(pudmi->um.hdc, menu_string, -1, &pudmi->dis.rcItem, dt_flags);
    }
    return true;
}

bool theme_core_on_uah_measureitem(HWND hwnd, LPARAM lp) {
    /* DefWindowProcW must run to populate mis.itemWidth/itemHeight.
     * Intercepting this leaves items zero-sized and the menu disappears.
     * Always returns false for that reason. */
    (void)hwnd; (void)lp;
    return false;
}

void theme_core_paint_uah_menu_underline(HWND hwnd) {
    if (!g_is_dark) return;

    /* DefWindowProc paints a 1px light separator between the menu bar and
     * client area. Cover it with our dlg_bg for a seamless transition. */
    MENUBARINFO mbi = { sizeof(mbi), 0 };
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
    RECT rcwin;
    if (!GetWindowRect(hwnd, &rcwin)) return;
    RECT line_rect = mbi.rcBar;
    OffsetRect(&line_rect, -rcwin.left, -rcwin.top);
    line_rect.top = line_rect.bottom;
    line_rect.bottom += 1;

    HDC hdc = GetWindowDC(hwnd);
    if (hdc) {
        FillRect(hdc, &line_rect, g_dark_palette.brush_dlg_bg);
        ReleaseDC(hwnd, hdc);
    }
}

/* ============================================================================
 * §13  WM_SETTINGCHANGE handler
 * ========================================================================== */

bool theme_core_is_relevant_setting_change(WPARAM wparam, LPARAM lparam) {
    const wchar_t *param = (const wchar_t *)lparam;

    if (wparam == SPI_SETHIGHCONTRAST) return true;
    return param && lstrcmpiW(param, L"ImmersiveColorSet") == 0;
}

bool theme_core_on_setting_change(WPARAM wparam, LPARAM lparam) {
    if (!theme_core_is_relevant_setting_change(wparam, lparam)) return false;
    if (!g_api.initialized) theme_core_init();

    if (g_api.refresh_immersive_color_policy_state) {
        g_api.refresh_immersive_color_policy_state();
    }
    apply_os_preferred_mode();
    resolve_effective_mode();

    refresh_palettes();
    drop_menu_theme();
    if (g_api.flush_menu_themes) g_api.flush_menu_themes();
    return true;
}

bool theme_core_on_syscolor_change(void) {
    if (!g_api.initialized) theme_core_init();
    apply_os_preferred_mode();
    resolve_effective_mode();
    refresh_palettes();
    drop_menu_theme();
    if (g_api.flush_menu_themes) g_api.flush_menu_themes();
    return true;
}
