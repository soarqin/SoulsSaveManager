/**
 * @file theme_core.h
 * @brief Generic Win32 dark/light theme infrastructure shared between applications.
 * @details Provides system theme detection, color palette management, and helpers for
 *          handling WM_CTLCOLOR*, NM_CUSTOMDRAW (TreeView/ListView/Toolbar), and the
 *          undocumented WM_UAH* menu-bar messages. Dynamically loads uxtheme.dll
 *          ordinals (132/133/135/136/137) and dwmapi.dll's DwmSetWindowAttribute on
 *          Windows 10 1809+, so calls are no-ops on older systems.
 *
 *          References:
 *            - Notepad++ DarkMode.cpp / NppDarkMode.cpp
 *            - ysc3839/win32-darkmode
 *            - System Informer phlib/theme.c
 *
 *          Each application owns its own theme.{c,h} glue module that calls into
 *          theme_core to apply theming to its specific UI surfaces. theme_core itself
 *          owns no application state beyond the global theme mode and palette.
 */

#pragma once

#include <stdbool.h>
#include <windows.h>
#include <commctrl.h>

/* Theme mode persisted in INI as 0/1/2. */
typedef enum theme_mode_e {
    THEME_MODE_SYSTEM = 0,  /* Follow OS preference, react to WM_SETTINGCHANGE */
    THEME_MODE_LIGHT  = 1,  /* Force light mode */
    THEME_MODE_DARK   = 2,  /* Force dark mode */
} theme_mode_t;

/* Color palette for one mode (light or dark). */
typedef struct theme_palette_s {
    /* Colors */
    COLORREF bg;             /* Main window background */
    COLORREF ctrl_bg;        /* Edit/listbox/treeview/listview background */
    COLORREF hot_bg;         /* Hover/highlight background */
    COLORREF text;           /* Primary text */
    COLORREF text_disabled;  /* Disabled text */
    COLORREF edge;           /* Borders and separators */
    COLORREF dlg_bg;         /* Dialog client area background */
    /* Cached GDI brushes (lazily created, freed on theme_core_cleanup).
     * brush_edge is used by FrameRect for our flat 1px borders. */
    HBRUSH brush_bg;
    HBRUSH brush_ctrl_bg;
    HBRUSH brush_hot_bg;
    HBRUSH brush_dlg_bg;
    HBRUSH brush_edge;
} theme_palette_t;

/* === Lifecycle === */

/**
 * @brief Load dynamic Win32 APIs and detect the OS build number.
 * @details Idempotent. Safe to call from any application thread before window creation.
 *          Loads uxtheme.dll, user32.dll's SetWindowCompositionAttribute, and
 *          dwmapi.dll's DwmSetWindowAttribute. Pointers stay NULL on Windows < 1809.
 */
void theme_core_init(void);

/**
 * @brief Free cached brushes/pens. Call once before app exit.
 */
void theme_core_cleanup(void);

/* === Mode management === */

/**
 * @brief Set the current theme mode and apply process-level OS state.
 * @details Calls SetPreferredAppMode + FlushMenuThemes (uxtheme ordinals 135/136)
 *          to make popup menus dark. After this returns, the caller MUST call
 *          theme_core_apply_to_window() (or _and_children()) on each top-level
 *          window to apply per-window state (DWM dark titlebar, control themes).
 * @param mode One of THEME_MODE_SYSTEM/LIGHT/DARK.
 */
void theme_core_set_mode(theme_mode_t mode);

/**
 * @brief Return the currently configured theme mode (last value passed to set_mode).
 */
theme_mode_t theme_core_get_mode(void);

/**
 * @brief Return whether dark mode is currently effectively active.
 * @details If mode is SYSTEM, queries the OS via ShouldAppsUseDarkMode + IsHighContrast.
 *          If mode is LIGHT, always false. If mode is DARK, always true (subject to
 *          high-contrast override which forces light).
 */
bool theme_core_is_dark(void);

/**
 * @brief Return the active color palette (light or dark, whichever is currently in use).
 */
const theme_palette_t *theme_core_palette(void);

/* === Per-window application === */

/**
 * @brief Apply theme to a top-level window.
 * @details Sets DWM dark titlebar (Win10 2004+ via documented API; falls back to
 *          1903-1909 SetWindowCompositionAttribute and 1809 SetProp). Calls
 *          AllowDarkModeForWindow on supported builds. Forces a non-client redraw.
 */
void theme_core_apply_to_window(HWND hwnd);

/**
 * @brief Apply theme to a SysTreeView32 control.
 * @details Calls AllowDarkModeForWindow + SetWindowTheme(L"DarkMode_Explorer", NULL).
 *          Sets text/background/line colors via TreeView_Set*Color macros.
 */
void theme_core_apply_to_treeview(HWND hwnd);

/**
 * @brief Apply theme to a SysListView32 control (and its header).
 * @details Calls AllowDarkModeForWindow + SetWindowTheme(L"DarkMode_Explorer") on
 *          the listview and SetWindowTheme(L"ItemsView") on the header. Sets
 *          ListView_SetBkColor / TextBkColor / TextColor.
 */
void theme_core_apply_to_listview(HWND hwnd);

/**
 * @brief Apply theme to a ComboBox control's dropdown list.
 * @details Calls SetWindowTheme(L"DarkMode_CFD", NULL).
 */
void theme_core_apply_to_combobox(HWND hwnd);

/**
 * @brief Apply theme to a status bar (msctls_statusbar32).
 * @details Empties the visual style so WM_CTLCOLOR* / custom paint take effect.
 */
void theme_core_apply_to_statusbar(HWND hwnd);

/**
 * @brief Apply theme to a button (BUTTON class), choosing the right strategy
 *        based on whether it is a checkbox, radio, group box, or push button.
 */
void theme_core_apply_to_button(HWND hwnd);

/**
 * @brief Recursively apply the most appropriate theme handler to @p hwnd and all
 *        its descendants (BUTTON/EDIT/COMBOBOX/SysTreeView32/SysListView32/etc).
 * @details Calls theme_core_apply_to_window on the top-level window itself if it
 *          is a top-level window (i.e. no parent), then walks children with
 *          EnumChildWindows.
 */
void theme_core_apply_to_window_and_children(HWND hwnd);

/* === WM_CTLCOLOR* handler === */

/**
 * @brief Generic WM_CTLCOLOR* handler.
 * @details Sets text and background colors on @p hdc and returns the brush to use
 *          for the control background. Returns NULL when dark mode is inactive,
 *          allowing the caller to fall through to DefWindowProc.
 *
 *          Caller pattern (in WndProc):
 *
 *              case WM_CTLCOLORSTATIC:
 *              case WM_CTLCOLOREDIT:
 *              case WM_CTLCOLORLISTBOX:
 *              case WM_CTLCOLORBTN:
 *              case WM_CTLCOLORDLG: {
 *                  HBRUSH br = theme_core_on_ctlcolor((HDC)wp, msg);
 *                  if (br) return (LRESULT)br;
 *                  break;
 *              }
 */
HBRUSH theme_core_on_ctlcolor(HDC hdc, UINT msg);

/* === NM_CUSTOMDRAW handlers === */

/**
 * @brief NM_CUSTOMDRAW handler for SysTreeView32. Returns CDRF_* code, or
 *        CDRF_DODEFAULT when not in dark mode.
 */
LRESULT theme_core_on_treeview_customdraw(LPNMTVCUSTOMDRAW nmtv);

/**
 * @brief NM_CUSTOMDRAW handler for SysListView32.
 */
LRESULT theme_core_on_listview_customdraw(LPNMLVCUSTOMDRAW nmlv);

/**
 * @brief NM_CUSTOMDRAW handler for ToolbarWindow32.
 */
LRESULT theme_core_on_toolbar_customdraw(LPNMTBCUSTOMDRAW nmtb);

/* === UAH menu bar (undocumented WM_UAH* messages) === */

/* These message constants are undocumented but stable since Win10 1809. */
#ifndef WM_UAHDESTROYWINDOW
#define WM_UAHDESTROYWINDOW    0x0090
#define WM_UAHDRAWMENU         0x0091
#define WM_UAHDRAWMENUITEM     0x0092
#define WM_UAHINITMENU         0x0093
#define WM_UAHMEASUREMENUITEM  0x0094
#define WM_UAHNCPAINTMENUPOPUP 0x0095
#endif

/**
 * @brief Handle WM_UAHDRAWMENU. Returns true if consumed (caller should return 0).
 */
bool theme_core_on_uah_drawmenu(HWND hwnd, LPARAM lp);

/**
 * @brief Handle WM_UAHDRAWMENUITEM. Returns true if consumed.
 */
bool theme_core_on_uah_drawmenuitem(HWND hwnd, LPARAM lp);

/**
 * @brief Handle WM_UAHMEASUREMENUITEM. Returns true if consumed.
 */
bool theme_core_on_uah_measureitem(HWND hwnd, LPARAM lp);

/**
 * @brief Paint the dark line that hides the light separator between the menu
 *        bar and the client area after WM_NCPAINT / WM_NCACTIVATE. Call AFTER
 *        DefWindowProcW for those messages when in dark mode.
 */
void theme_core_paint_uah_menu_underline(HWND hwnd);

/* === Erase background helper === */

/**
 * @brief WM_ERASEBKGND helper. Fills the client area with the active palette's
 *        dialog background brush (dark or light).
 * @return true if the message was handled (caller must return 1); false only
 *         if hwnd or hdc is NULL.
 */
bool theme_core_on_erasebkgnd(HWND hwnd, HDC hdc);

/**
 * @brief WM_CTLCOLOR* helper for use inside a DLGPROC.
 * @details Sets HDC colors and returns the brush. For dialog procs the brush
 *          is returned as INT_PTR (the documented convention for DLGPROC).
 *          Returns 0 when not in dark mode so the caller can `return FALSE`.
 *
 *          Caller pattern in dialog procs:
 *
 *              case WM_CTLCOLORDLG:
 *              case WM_CTLCOLOREDIT:
 *              ... etc:
 *                  return theme_core_dlg_ctlcolor((HDC)wp, msg);
 */
INT_PTR theme_core_dlg_ctlcolor(HDC hdc, UINT msg);

/* === System change handler === */

/**
 * @brief Return whether a WM_SETTINGCHANGE payload can affect theme state.
 * @details Recognizes system app-theme changes ("ImmersiveColorSet") and
 *          high-contrast toggles (SPI_SETHIGHCONTRAST).
 */
bool theme_core_is_relevant_setting_change(WPARAM wparam, LPARAM lparam);

/**
 * @brief Handle WM_SETTINGCHANGE.
 * @details Detects the "ImmersiveColorSet" notification (sent when user toggles
 *          the system dark/light preference) and SPI_SETHIGHCONTRAST. Refreshes
 *          internal state for SYSTEM, forced LIGHT, and forced DARK modes so the
 *          high-contrast override is honored immediately.
 * @return true when theme state was refreshed and the caller should re-apply
 *         theme to all windows; false for unrelated setting changes.
 */
bool theme_core_on_setting_change(WPARAM wparam, LPARAM lparam);

/**
 * @brief Handle WM_SYSCOLORCHANGE.
 * @details Refreshes cached light palette system colors and menu theme data.
 * @return true when callers should re-apply the theme to all windows.
 */
bool theme_core_on_syscolor_change(void);
