/**
 * @file file_dialog.c
 * @brief Win32 IFileDialog helper wrappers
 * @details Implements thin wrappers around the COM IFileDialog interface for
 *          opening files, saving files, and picking folders.
 */

#include "file_dialog.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

wchar_t *file_dialog_open(HWND hwnd, const wchar_t *title,
                          const COMDLG_FILTERSPEC *filters, UINT filter_count) {
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
        &IID_IFileDialog, (void **)&pfd);
    if (!SUCCEEDED(hr)) {
        return NULL;
    }

    if (title) {
        pfd->lpVtbl->SetTitle(pfd, title);
    }
    DWORD dwOptions;
    pfd->lpVtbl->GetOptions(pfd, &dwOptions);
    pfd->lpVtbl->SetOptions(pfd, dwOptions | FOS_FILEMUSTEXIST);
    if (filters && filter_count > 0) {
        pfd->lpVtbl->SetFileTypes(pfd, filter_count, filters);
    }

    wchar_t *result = NULL;
    hr = pfd->lpVtbl->Show(pfd, hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem *psiResult = NULL;
        hr = pfd->lpVtbl->GetResult(pfd, &psiResult);
        if (SUCCEEDED(hr)) {
            hr = psiResult->lpVtbl->GetDisplayName(psiResult, SIGDN_FILESYSPATH, &result);
            if (!SUCCEEDED(hr)) {
                result = NULL;
            }
            psiResult->lpVtbl->Release(psiResult);
        }
    }

    pfd->lpVtbl->Release(pfd);
    return result;
}

wchar_t *file_dialog_open_at(HWND hwnd, const wchar_t *title, const wchar_t *initial_path,
                             const COMDLG_FILTERSPEC *filters, UINT filter_count) {
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
        &IID_IFileDialog, (void **)&pfd);
    if (!SUCCEEDED(hr)) {
        return NULL;
    }

    if (title) {
        pfd->lpVtbl->SetTitle(pfd, title);
    }
    DWORD dwOptions;
    pfd->lpVtbl->GetOptions(pfd, &dwOptions);
    pfd->lpVtbl->SetOptions(pfd, dwOptions | FOS_FILEMUSTEXIST);
    if (filters && filter_count > 0) {
        pfd->lpVtbl->SetFileTypes(pfd, filter_count, filters);
    }

    if (initial_path && initial_path[0]) {
        wchar_t folder[MAX_PATH];
        lstrcpynW(folder, initial_path, MAX_PATH);
        if (!PathIsDirectoryW(folder)) {
            PathRemoveFileSpecW(folder);
        }
        if (folder[0]) {
            IShellItem *psi = NULL;
            hr = SHCreateItemFromParsingName(folder, NULL, &IID_IShellItem, (void **)&psi);
            if (SUCCEEDED(hr)) {
                pfd->lpVtbl->SetFolder(pfd, psi);
                psi->lpVtbl->Release(psi);
            }
        }
    }

    wchar_t *result = NULL;
    hr = pfd->lpVtbl->Show(pfd, hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem *psiResult = NULL;
        hr = pfd->lpVtbl->GetResult(pfd, &psiResult);
        if (SUCCEEDED(hr)) {
            hr = psiResult->lpVtbl->GetDisplayName(psiResult, SIGDN_FILESYSPATH, &result);
            if (!SUCCEEDED(hr)) {
                result = NULL;
            }
            psiResult->lpVtbl->Release(psiResult);
        }
    }

    pfd->lpVtbl->Release(pfd);
    return result;
}

wchar_t *file_dialog_save(HWND hwnd, const wchar_t *title,
                          const COMDLG_FILTERSPEC *filters, UINT filter_count) {
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
        &IID_IFileDialog, (void **)&pfd);
    if (!SUCCEEDED(hr)) {
        return NULL;
    }

    if (title) {
        pfd->lpVtbl->SetTitle(pfd, title);
    }
    DWORD dwOptions;
    pfd->lpVtbl->GetOptions(pfd, &dwOptions);
    pfd->lpVtbl->SetOptions(pfd, dwOptions | FOS_OVERWRITEPROMPT);
    if (filters && filter_count > 0) {
        pfd->lpVtbl->SetFileTypes(pfd, filter_count, filters);
    }

    wchar_t *result = NULL;
    hr = pfd->lpVtbl->Show(pfd, hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem *psiResult = NULL;
        hr = pfd->lpVtbl->GetResult(pfd, &psiResult);
        if (SUCCEEDED(hr)) {
            hr = psiResult->lpVtbl->GetDisplayName(psiResult, SIGDN_FILESYSPATH, &result);
            if (!SUCCEEDED(hr)) {
                result = NULL;
            }
            psiResult->lpVtbl->Release(psiResult);
        }
    }

    pfd->lpVtbl->Release(pfd);
    return result;
}

wchar_t *file_dialog_open_folder(HWND hwnd, const wchar_t *initial_path) {
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
        &IID_IFileDialog, (void **)&pfd);
    if (!SUCCEEDED(hr)) {
        return NULL;
    }

    DWORD dwOptions;
    pfd->lpVtbl->GetOptions(pfd, &dwOptions);
    pfd->lpVtbl->SetOptions(pfd, dwOptions | FOS_PICKFOLDERS);

    if (initial_path) {
        IShellItem *psi = NULL;
        hr = SHCreateItemFromParsingName(initial_path, NULL, &IID_IShellItem, (void **)&psi);
        if (SUCCEEDED(hr)) {
            pfd->lpVtbl->SetFolder(pfd, psi);
            psi->lpVtbl->Release(psi);
        }
    }

    wchar_t *result = NULL;
    hr = pfd->lpVtbl->Show(pfd, hwnd);
    if (SUCCEEDED(hr)) {
        IShellItem *psiResult = NULL;
        hr = pfd->lpVtbl->GetResult(pfd, &psiResult);
        if (SUCCEEDED(hr)) {
            hr = psiResult->lpVtbl->GetDisplayName(psiResult, SIGDN_FILESYSPATH, &result);
            if (!SUCCEEDED(hr)) {
                result = NULL;
            }
            psiResult->lpVtbl->Release(psiResult);
        }
    }

    pfd->lpVtbl->Release(pfd);
    return result;
}
