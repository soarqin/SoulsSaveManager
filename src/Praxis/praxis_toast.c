/**
 * @file praxis_toast.c
 * @brief Centered, auto-fading toast panel implementation.
 * @details The toast lives in a small custom child window that paints a
 *          rounded, dark, opaque panel with centered text. It uses
 *          HTTRANSPARENT in WM_NCHITTEST so mouse interaction passes through
 *          to siblings (typically the save tree). Auto-hide is driven by a
 *          per-instance Win32 timer; re-arming on a fresh show() resets the
 *          countdown so successive hotkeys never feel "stuck".
 */

#include "praxis_toast.h"

#include "../common/theme_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include <windows.h>

#define PRAXIS_TOAST_CLASS_NAME L"PRAXIS_TOAST"
#define PRAXIS_TOAST_TIMER_ID 1
#define PRAXIS_TOAST_PADDING_X 32
#define PRAXIS_TOAST_PADDING_Y 18
#define PRAXIS_TOAST_CORNER_RADIUS 12
#define PRAXIS_TOAST_MAX_TEXT 255

/* Material-style success greens, picked for readable contrast against the
 * theme-aware panel background:
 *   - Dark mode: bright green (green 400) over the dark dlg_bg
 *   - Light mode: deeper green (green 800) over the light dlg_bg
 * Tuned so the user-perceived "success" cue is consistent across themes. */
#define PRAXIS_TOAST_GREEN_DARK  RGB(0x4C, 0xD8, 0x50)
#define PRAXIS_TOAST_GREEN_LIGHT RGB(0x2E, 0x7D, 0x32)

/* Material-style error reds for failure toast border:
 *   - Dark mode: red 400
 *   - Light mode: red 800 */
#define PRAXIS_TOAST_RED_DARK  RGB(0xEF, 0x53, 0x50)
#define PRAXIS_TOAST_RED_LIGHT RGB(0xC6, 0x28, 0x28)

struct praxis_toast_s {
    HWND hwnd;                              /* Toast window handle. */
    HWND parent;                            /* Parent (main) window handle. */
    HFONT font;                             /* Custom font owned by this toast. */
    wchar_t text[PRAXIS_TOAST_MAX_TEXT + 1];
    COLORREF text_color;
    COLORREF border_color;                  /* 0 = use theme edge color (1px). */
    int width;                              /* Computed panel width in pixels. */
    int height;                             /* Computed panel height in pixels. */
};

static volatile LONG g_class_registered = 0;

static LRESULT CALLBACK praxis_toast_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Register the toast window class once per process. */
static bool ensure_class_registered(HINSTANCE instance) {
    WNDCLASSEXW wc;

    if (InterlockedCompareExchange(&g_class_registered, 1, 0) != 0) return true;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = praxis_toast_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    /* No background brush: WM_PAINT fills the entire client area, so any
     * default brush would just cause flicker on resize. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = PRAXIS_TOAST_CLASS_NAME;
    if (!RegisterClassExW(&wc)) {
        InterlockedExchange(&g_class_registered, 0);
        return false;
    }
    return true;
}

/* Build the message font. Uses Segoe UI at a slightly larger size than the
 * default UI font so the toast text reads as a deliberate notification. */
static HFONT create_toast_font(void) {
    LOGFONTW lf;
    HDC screen_dc;
    int dpi;
    int point_size = 12;
    int height;

    ZeroMemory(&lf, sizeof(lf));
    screen_dc = GetDC(NULL);
    dpi = screen_dc ? GetDeviceCaps(screen_dc, LOGPIXELSY) : 96;
    if (screen_dc) ReleaseDC(NULL, screen_dc);
    height = -MulDiv(point_size, dpi, 72);
    lf.lfHeight = height;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

/* Compute panel dimensions from the current text + font, then update the
 * window region so the layered window is physically clipped to a rounded
 * shape. Without the region, WS_EX_LAYERED + LWA_ALPHA leaves the four
 * corners as opaque rectangles (visible as ugly squares against light
 * themes); the region makes those pixels not part of the window so the
 * parent shows through. */
static void compute_size(praxis_toast_t *t) {
    HDC dc;
    HFONT old_font;
    RECT r;
    HRGN rgn;

    if (!t || !t->hwnd) return;
    dc = GetDC(t->hwnd);
    if (!dc) {
        t->width = 240;
        t->height = 60;
    } else {
        old_font = (HFONT)SelectObject(dc, t->font);
        SetRect(&r, 0, 0, 0, 0);
        DrawTextW(dc, t->text, -1, &r,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        if (old_font) SelectObject(dc, old_font);
        ReleaseDC(t->hwnd, dc);
        t->width = (r.right - r.left) + 2 * PRAXIS_TOAST_PADDING_X;
        t->height = (r.bottom - r.top) + 2 * PRAXIS_TOAST_PADDING_Y;
        if (t->width < 160) t->width = 160;
        if (t->height < 48) t->height = 48;
    }
    /* CreateRoundRectRgn uses an exclusive-style x2/y2; pass width+1/height+1
     * so the region exactly covers the [0..width-1] x [0..height-1] pixel
     * range that BeginPaint/BitBlt will write to. */
    rgn = CreateRoundRectRgn(0, 0, t->width + 1, t->height + 1,
                             PRAXIS_TOAST_CORNER_RADIUS * 2,
                             PRAXIS_TOAST_CORNER_RADIUS * 2);
    if (rgn) {
        /* SetWindowRgn takes ownership on success; on failure the caller
         * is responsible for the region. */
        if (!SetWindowRgn(t->hwnd, rgn, FALSE)) DeleteObject(rgn);
    }
}

/* Position the toast over the center of the parent's client area. */
static void center_over_parent(praxis_toast_t *t) {
    RECT cr;
    int x, y;

    if (!t || !t->parent || !IsWindow(t->hwnd)) return;
    if (!GetClientRect(t->parent, &cr)) return;
    x = (cr.right - cr.left - t->width) / 2;
    y = (cr.bottom - cr.top - t->height) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    SetWindowPos(t->hwnd, HWND_TOP, x, y, t->width, t->height,
                 SWP_NOACTIVATE);
}

/* Paint the rounded panel with centered colored text. Background and
 * border colors are pulled from the active theme palette so the toast
 * matches both light and dark modes. The rounded shape is provided by
 * the SetWindowRgn applied in compute_size(); paint just fills the
 * client rect and strokes a 1px rounded border. */
static void paint_toast(praxis_toast_t *t, HDC dc, const RECT *client) {
    HDC mem_dc;
    HBITMAP mem_bmp;
    HBITMAP old_bmp;
    HBRUSH bg_brush;
    bool bg_owned;
    HPEN border_pen;
    HPEN old_pen;
    HBRUSH old_brush;
    HFONT old_font;
    int w, h;
    RECT text_rect;
    const theme_palette_t *pal;

    w = client->right - client->left;
    h = client->bottom - client->top;
    if (w <= 0 || h <= 0) return;

    pal = theme_core_palette();

    /* Double-buffer to avoid any flicker on rapid re-show. */
    mem_dc = CreateCompatibleDC(dc);
    if (!mem_dc) return;
    mem_bmp = CreateCompatibleBitmap(dc, w, h);
    if (!mem_bmp) {
        DeleteDC(mem_dc);
        return;
    }
    old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);

    /* Fill the panel with the theme dialog color. Reuse the palette's
     * cached brush when available; otherwise create a transient one. */
    if (pal && pal->brush_dlg_bg) {
        bg_brush = pal->brush_dlg_bg;
        bg_owned = false;
    } else {
        bg_brush = CreateSolidBrush(pal ? pal->dlg_bg : GetSysColor(COLOR_BTNFACE));
        bg_owned = true;
    }
    FillRect(mem_dc, client, bg_brush);
    if (bg_owned) DeleteObject(bg_brush);

    /* Draw a rounded border. Default (border_color == 0) uses the 1px
     * theme edge color; an explicit border_color paints a 2px emphasized
     * border (used to flag failure toasts in red). NULL_BRUSH makes
     * RoundRect stroke only (no fill), which matches the rounded window
     * region applied in compute_size(). */
    {
        COLORREF bc = t->border_color
            ? t->border_color
            : (pal ? pal->edge : GetSysColor(COLOR_BTNSHADOW));
        border_pen = CreatePen(PS_SOLID, t->border_color ? 2 : 1, bc);
    }
    old_pen = (HPEN)SelectObject(mem_dc, border_pen);
    old_brush = (HBRUSH)SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
    RoundRect(mem_dc, 0, 0, w, h,
              PRAXIS_TOAST_CORNER_RADIUS * 2,
              PRAXIS_TOAST_CORNER_RADIUS * 2);
    SelectObject(mem_dc, old_pen);
    SelectObject(mem_dc, old_brush);
    DeleteObject(border_pen);

    /* Draw the centered, colored text. */
    old_font = (HFONT)SelectObject(mem_dc, t->font);
    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, t->text_color);
    text_rect = *client;
    DrawTextW(mem_dc, t->text, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font) SelectObject(mem_dc, old_font);

    BitBlt(dc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
}

static LRESULT CALLBACK praxis_toast_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    praxis_toast_t *t = (praxis_toast_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_NCHITTEST:
        /* Click-through: clicks land on whatever is underneath. */
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        /* WM_PAINT does the full fill via double-buffer; suppressing the
         * background erase eliminates the white-flash on first show. */
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc;
        RECT client;

        dc = BeginPaint(hwnd, &ps);
        if (t && GetClientRect(hwnd, &client)) {
            paint_toast(t, dc, &client);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == PRAXIS_TOAST_TIMER_ID) {
            KillTimer(hwnd, PRAXIS_TOAST_TIMER_ID);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, PRAXIS_TOAST_TIMER_ID);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

praxis_toast_t *praxis_toast_create(HWND parent, HINSTANCE instance) {
    praxis_toast_t *t;

    if (!parent || !ensure_class_registered(instance)) return NULL;
    t = (praxis_toast_t *)LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, sizeof(*t));
    if (!t) return NULL;
    t->parent = parent;
    /* Default text color is overwritten on every show(), so the initial
     * value just needs to be a valid COLORREF. */
    t->text_color = PRAXIS_TOAST_GREEN_DARK;
    t->width = 240;
    t->height = 60;
    t->font = create_toast_font();
    if (!t->font) {
        LocalFree(t);
        return NULL;
    }
    /* WS_EX_LAYERED is critical: the parent window class lacks
     * CS_CLIPCHILDREN and the sibling tree view lacks WS_CLIPSIBLINGS, so a
     * plain child window would be overdrawn whenever the tree repaints
     * (most notably the 200ms watcher-debounce refresh after a backup).
     * Layered child windows (Win8+) are composited by DWM and remain
     * visible regardless of sibling paint cycles. */
    t->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED,
                              PRAXIS_TOAST_CLASS_NAME, L"",
                              WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, t->width, t->height,
                              parent, NULL, instance, t);
    if (!t->hwnd) {
        DeleteObject(t->font);
        LocalFree(t);
        return NULL;
    }
    /* SetLayeredWindowAttributes is REQUIRED for a layered window to
     * become visible. Alpha 255 means fully opaque; we only need DWM
     * compositing here, not transparency. */
    SetLayeredWindowAttributes(t->hwnd, 0, 255, LWA_ALPHA);
    return t;
}

void praxis_toast_destroy(praxis_toast_t *toast) {
    if (!toast) return;
    if (toast->hwnd && IsWindow(toast->hwnd)) {
        KillTimer(toast->hwnd, PRAXIS_TOAST_TIMER_ID);
        DestroyWindow(toast->hwnd);
    }
    if (toast->font) DeleteObject(toast->font);
    LocalFree(toast);
}

void praxis_toast_show(praxis_toast_t *toast, const wchar_t *message,
                       COLORREF text_color, COLORREF border_color, int duration_ms) {
    UINT duration;

    if (!toast || !toast->hwnd) return;
    if (!message || !message[0]) {
        praxis_toast_hide(toast);
        return;
    }
    lstrcpynW(toast->text, message, PRAXIS_TOAST_MAX_TEXT + 1);
    toast->text_color = text_color;
    toast->border_color = border_color;
    compute_size(toast);
    center_over_parent(toast);
    /* InvalidateRect ensures the new text/color paints on this same show
     * even when the panel was already visible from a previous toast. */
    InvalidateRect(toast->hwnd, NULL, FALSE);
    if (!IsWindowVisible(toast->hwnd)) {
        ShowWindow(toast->hwnd, SW_SHOWNA);
    }
    /* Force the paint to happen synchronously so the toast pixels reach
     * the screen before any pending sibling repaint can run. Without this
     * the user briefly sees the toast flash before the tree's debounced
     * refresh can compose over it. */
    UpdateWindow(toast->hwnd);
    /* Re-arm hide timer (replaces any prior pending timer). */
    KillTimer(toast->hwnd, PRAXIS_TOAST_TIMER_ID);
    duration = (duration_ms > 0) ? (UINT)duration_ms : (UINT)PRAXIS_TOAST_DEFAULT_DURATION_MS;
    SetTimer(toast->hwnd, PRAXIS_TOAST_TIMER_ID, duration, NULL);
}

void praxis_toast_recenter(praxis_toast_t *toast) {
    if (!toast || !toast->hwnd) return;
    if (!IsWindowVisible(toast->hwnd)) return;
    center_over_parent(toast);
}

void praxis_toast_hide(praxis_toast_t *toast) {
    if (!toast || !toast->hwnd) return;
    KillTimer(toast->hwnd, PRAXIS_TOAST_TIMER_ID);
    ShowWindow(toast->hwnd, SW_HIDE);
}

COLORREF praxis_toast_color_success(void) {
    return theme_core_is_dark() ? PRAXIS_TOAST_GREEN_DARK : PRAXIS_TOAST_GREEN_LIGHT;
}

COLORREF praxis_toast_color_error(void) {
    return theme_core_is_dark() ? PRAXIS_TOAST_RED_DARK : PRAXIS_TOAST_RED_LIGHT;
}
