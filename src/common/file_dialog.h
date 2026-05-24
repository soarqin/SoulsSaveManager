/**
 * @file file_dialog.h
 * @brief Win32 IFileDialog helper wrappers
 * @details Thin wrappers around the IFileDialog COM interface for opening,
 *          saving, and selecting folders. All functions return a CoTaskMem-
 *          allocated path string or NULL on cancel/error; caller must call
 *          CoTaskMemFree() on the returned pointer.
 */
#pragma once

#include <windows.h>
#include <shobjidl.h>

/**
 * @brief Show an open-file dialog
 * @param hwnd Parent window handle
 * @param title Dialog title string (or NULL for default)
 * @param filters Array of COMDLG_FILTERSPEC file type filters
 * @param filter_count Number of entries in filters
 * @return CoTaskMem-allocated path string, or NULL if cancelled/error
 */
wchar_t *file_dialog_open(HWND hwnd, const wchar_t *title,
                          const COMDLG_FILTERSPEC *filters, UINT filter_count);

/**
 * @brief Show an open-file dialog with an optional initial folder.
 * @param hwnd Parent window handle
 * @param title Dialog title string (or NULL for default)
 * @param initial_path Initial folder or file path to show (or NULL for default)
 * @param filters Array of COMDLG_FILTERSPEC file type filters
 * @param filter_count Number of entries in filters
 * @return CoTaskMem-allocated path string, or NULL if cancelled/error
 */
wchar_t *file_dialog_open_at(HWND hwnd, const wchar_t *title, const wchar_t *initial_path,
                             const COMDLG_FILTERSPEC *filters, UINT filter_count);

/**
 * @brief Show a save-file dialog
 * @param hwnd Parent window handle
 * @param title Dialog title string (or NULL for default)
 * @param filters Array of COMDLG_FILTERSPEC file type filters
 * @param filter_count Number of entries in filters
 * @return CoTaskMem-allocated path string, or NULL if cancelled/error
 */
wchar_t *file_dialog_save(HWND hwnd, const wchar_t *title,
                          const COMDLG_FILTERSPEC *filters, UINT filter_count);

/**
 * @brief Show a folder-picker dialog
 * @param hwnd Parent window handle
 * @param initial_path Initial folder to show (or NULL for default)
 * @return CoTaskMem-allocated path string, or NULL if cancelled/error
 */
wchar_t *file_dialog_open_folder(HWND hwnd, const wchar_t *initial_path);
