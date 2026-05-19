/**
 * @file praxis_toast.h
 * @brief Centered, auto-fading notification panel (toast) for the Praxis main window.
 * @details Provides a transient overlay used to confirm successful backup/restore
 *          actions. The toast is a borderless child window that paints a rounded
 *          dark panel with centered text and auto-hides after a caller-supplied
 *          duration. The panel is click-through (HTTRANSPARENT) so it never
 *          blocks user interaction with the underlying tree view.
 */

#pragma once

#include <stdbool.h>
#include <windows.h>

/**
 * @brief Returns the theme-appropriate "success" green for toast text.
 * @details Resolves at call time so the caller always gets a color that
 *          remains legible against the current toast panel background:
 *            - Dark theme: bright Material-style green
 *            - Light theme: deeper Material-style green
 *          Use this as the @p text_color argument to praxis_toast_show()
 *          for backup/restore success notifications.
 */
COLORREF praxis_toast_color_success(void);

/**
 * @brief Returns the theme-appropriate error red for toast border.
 * @details Use as the @p border_color argument to praxis_toast_show()
 *          for backup/restore failure notifications.
 */
COLORREF praxis_toast_color_error(void);

/** Default visible duration in milliseconds. */
#define PRAXIS_TOAST_DEFAULT_DURATION_MS 2000

/* Opaque toast handle. */
typedef struct praxis_toast_s praxis_toast_t;

/**
 * @brief Create the toast window as a child of @p parent.
 * @details The window is created hidden. Call praxis_toast_show() to display it.
 *          The instance handle is used to register the toast window class on
 *          first creation; subsequent calls reuse the existing class.
 * @param parent Parent (main) window handle.
 * @param instance Module instance handle for class registration.
 * @return Newly allocated toast on success, NULL on failure.
 */
praxis_toast_t *praxis_toast_create(HWND parent, HINSTANCE instance);

/**
 * @brief Destroy the toast window and free all resources.
 * @param toast Toast returned by praxis_toast_create(). NULL is a no-op.
 */
void praxis_toast_destroy(praxis_toast_t *toast);

/**
 * @brief Show a toast message centered in the parent's client area.
 * @details Recomputes the panel size from the text metrics, recenters over the
 *          parent client area, brings the panel to the top of the sibling Z-order,
 *          and arms a single-shot hide timer for @p duration_ms. Calling this while
 *          a previous toast is still visible replaces the message and resets the
 *          timer.
 * @param toast Toast returned by praxis_toast_create(). NULL is a no-op.
 * @param message Wide-string message to display. NULL or empty hides the panel.
 * @param text_color RGB color used for the message text.
 * @param border_color RGB color for an emphasized 2px panel border, or 0 to use
 *                     the theme default 1px edge color.
 * @param duration_ms Visible duration in milliseconds. Non-positive values use
 *                    PRAXIS_TOAST_DEFAULT_DURATION_MS.
 */
void praxis_toast_show(praxis_toast_t *toast, const wchar_t *message,
                       COLORREF text_color, COLORREF border_color, int duration_ms);

/**
 * @brief Recenter the toast over the parent's client area.
 * @details Call from the parent's WM_SIZE handler so the panel stays centered
 *          when the user resizes the main window. No-op when the toast is hidden.
 * @param toast Toast returned by praxis_toast_create(). NULL is a no-op.
 */
void praxis_toast_recenter(praxis_toast_t *toast);

/**
 * @brief Hide the toast immediately, cancelling any pending hide timer.
 * @param toast Toast returned by praxis_toast_create(). NULL is a no-op.
 */
void praxis_toast_hide(praxis_toast_t *toast);
