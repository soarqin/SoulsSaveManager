/**
 * @file toolbar.h
 * @brief Two-row toolbar widget: backup profile combobox (top) + action buttons (bottom).
 * @details Provides two fixed-height child container windows:
 *          - Top container (30 px): backup profile combobox + add ("+"),
 *            delete ("-"), and file sort combobox controls.
 *          - Bottom container (38 px): five action buttons — Backup Full,
 *            Backup Slot, Backup & Replace, Restore, Undo Last Restore.
 *          Both containers are purely UI hosts; WM_COMMAND routing for the
 *          buttons and combobox is handled by the main window procedure via
 *          the toolbar container WndProc forwarding.
 */

#pragma once

#include <stdbool.h>
#include <windows.h>

#include "profile_store.h"
#include "save_tree.h"

/* Opaque toolbar handle. */
typedef struct toolbar_s toolbar_t;

/**
 * @brief Create the toolbar (top + bottom containers) as children of the parent window.
 * @param parent Parent window handle.
 * @param hinst  Application instance handle.
 * @return Heap-allocated toolbar_t on success, NULL on failure.
 */
toolbar_t *toolbar_create(HWND parent, HINSTANCE hinst);

/**
 * @brief Destroy the toolbar and free resources (both containers).
 * @param t Toolbar handle (may be NULL).
 */
void toolbar_destroy(toolbar_t *t);

/**
 * @brief Get the top container window handle (combobox + add/del buttons).
 * @param t Toolbar handle.
 * @return Top container window handle, or NULL if t is NULL.
 */
HWND toolbar_get_hwnd_top(const toolbar_t *t);

/**
 * @brief Get the bottom container window handle (action buttons).
 * @param t Toolbar handle.
 * @return Bottom container window handle, or NULL if t is NULL.
 */
HWND toolbar_get_hwnd_bottom(const toolbar_t *t);

/**
 * @brief Get the fixed height of the top toolbar in pixels.
 * @param t Toolbar handle.
 * @return Top toolbar height in pixels (30), or 0 if t is NULL.
 */
int toolbar_get_top_height(const toolbar_t *t);

/**
 * @brief Get the fixed height of the bottom toolbar in pixels.
 * @param t Toolbar handle.
 * @return Bottom toolbar height in pixels (38), or 0 if t is NULL.
 */
int toolbar_get_bottom_height(const toolbar_t *t);

/**
 * @brief Reflow the top toolbar layout for a new parent width.
 * @details Combobox stretches to fill available space; "+" and "-" buttons
 *          stay right-aligned. Minimum combobox width is clamped to 120 px.
 *          The container itself is resized to span @p parent_width at y=0.
 * @param t Toolbar handle.
 * @param parent_width Width of parent client area in pixels.
 */
void toolbar_layout_top(toolbar_t *t, int parent_width);

/**
 * @brief Reflow the bottom toolbar layout for a new parent width and y position.
 * @details All action buttons are left-aligned with equal width. The container
 *          is repositioned to (0, @p y_top) and resized to span @p parent_width.
 * @param t Toolbar handle.
 * @param parent_width Width of parent client area in pixels.
 * @param y_top        Y coordinate (in parent client space) for the top edge
 *                     of the bottom container.
 */
void toolbar_layout_bottom(toolbar_t *t, int parent_width, int y_top);

/**
 * @brief Repopulate the backup profile combobox from the profile store.
 * @details Clears existing items then appends backup profiles for the active
 *          game with the backup_id stored as item data via CB_SETITEMDATA.
 * @param t     Toolbar handle.
 * @param store Profile store to read from (may be NULL — clears combobox).
 */
void toolbar_populate_profiles(toolbar_t *t, const profile_store_t *store);

/**
 * @brief Get the currently selected backup profile ID from the combobox.
 * @param t Toolbar handle.
 * @return Backup profile ID, or 0 if no selection or t is NULL.
 */
int toolbar_get_selected_backup_id(const toolbar_t *t);

/**
 * @brief Select a backup profile in the combobox by its ID.
 * @details If no item with the given ID exists, the selection is cleared.
 * @param t         Toolbar handle.
 * @param backup_id Backup profile ID to select.
 */
void toolbar_set_selected_backup_id(toolbar_t *t, int backup_id);

/**
 * @brief Get the current file sort mode selected in the sort combobox.
 * @param t Toolbar handle.
 * @return Selected sort mode, or SAVE_TREE_SORT_NAME_ASC if unavailable.
 */
save_tree_sort_mode_t toolbar_get_selected_sort_mode(const toolbar_t *t);

/**
 * @brief Select a file sort mode in the sort combobox.
 * @param t Toolbar handle.
 * @param mode Sort mode to select.
 */
void toolbar_set_selected_sort_mode(toolbar_t *t, save_tree_sort_mode_t mode);

/**
 * @brief Enable or disable all action buttons.
 * @details When disabled, only the combobox and the "+" (add backup) button
 *          remain enabled so users can still create their first profile.
 *          The "-", Backup Full, Backup Slot, Backup & Replace, Restore, and
 *          Undo buttons are gated by this flag.
 * @param t       Toolbar handle.
 * @param enabled true to enable action buttons, false to disable.
 */
void toolbar_set_actions_enabled(toolbar_t *t, bool enabled);

/**
 * @brief Enable or disable only the Backup & Replace action button.
 * @param t Toolbar handle.
 * @param enabled true to enable the button, false to disable it.
 */
void toolbar_set_backup_replace_enabled(toolbar_t *t, bool enabled);

/**
 * @brief Re-apply localized strings to all toolbar buttons.
 * @details Call after praxis_locale_set_current() so buttons reflect the
 *          newly-selected language without requiring an application restart.
 *          The combobox content (profile labels) is unaffected — those come
 *          from the profile store, not the locale catalog.
 * @param t Toolbar handle (may be NULL — no-op).
 */
void toolbar_apply_locale_strings(toolbar_t *t);
