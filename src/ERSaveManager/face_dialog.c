/**
 * @file face_dialog.c
 * @brief Face data management dialog implementation
 * @details Implements the face data management modal dialog, including
 *          the ListView for face slots, context menu, and import/export operations.
 */
#include "face_dialog.h"
#include "ersave.h"
#include "locale.h"
#include "embedded_face_data.h"
#include "file_dialog.h"
#include "resource.h"
#include "theme.h"
#include "theme_core.h"
#include "ui_layout.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

/* Globals declared in main.c */
extern er_save_data_t *save_data;
extern HFONT default_font;
extern HMENU embedded_face_data_menu;

/* ListView handle for the face data dialog — local to this module */
static HWND list_view_faces = NULL;

/* Button handles for the face data dialog */
static HWND button_import_face = NULL;
static HWND button_export_face = NULL;
static HWND button_npc_face = NULL;

/* Update enabled state of face action buttons based on ListView selection */
static void update_face_buttons(void) {
    int sel = ListView_GetNextItem(list_view_faces, -1, LVNI_SELECTED);
    BOOL enabled = (sel >= 0);
    EnableWindow(button_import_face, enabled);
    EnableWindow(button_export_face, enabled);
    EnableWindow(button_npc_face, enabled);
}

/* Lay out ListView and buttons to fit the dialog client area */
static void layout_face_dialog(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int list_w = rc.right  - UI_MARGIN * 2;
    int list_h = rc.bottom - UI_MARGIN - UI_GAP_SMALL - UI_BTN_HEIGHT - UI_MARGIN;
    int list_y = UI_MARGIN;
    int btn_w  = (list_w - UI_GAP_SMALL * 2) / 3;
    int btn_y  = UI_MARGIN + list_h + UI_GAP_SMALL;

    MoveWindow(list_view_faces,   UI_MARGIN, list_y, list_w, list_h, TRUE);
    MoveWindow(button_import_face, UI_MARGIN, btn_y, btn_w, UI_BTN_HEIGHT, TRUE);
    MoveWindow(button_export_face, UI_MARGIN + btn_w + UI_GAP_SMALL, btn_y, btn_w, UI_BTN_HEIGHT, TRUE);
    MoveWindow(button_npc_face,    UI_MARGIN + (btn_w + UI_GAP_SMALL) * 2, btn_y, btn_w, UI_BTN_HEIGHT, TRUE);
}

static void on_import_embedded_face_data(HWND hwnd, int idx, int item) {
    if (idx < 0 || idx >= embedded_face_data_count) {
        return;
    }
    const uint8_t *face_data = embedded_face_data[idx].data;
    if (face_data && er_face_data_import(save_data, item, face_data)) {
        uint8_t available, gender;
        er_face_data_info(face_data, &available, &gender);
        wchar_t body_type[32];
        wsprintfW(body_type, L"%s", locale_str(available ? (gender ? STR_TYPE_B : STR_TYPE_A) : STR_EMPTY));
        ListView_SetItemText(list_view_faces, item, 1, body_type);
        MessageBoxW(hwnd, locale_str(STR_IMPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd, locale_str(STR_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
    }
}

/* Function to import face data from a file */
static void import_face_data(HWND hwnd, int item) {
    COMDLG_FILTERSPEC rgSpec[] = {
        { locale_str(STR_ALL_FILES), L"*.*" }
    };
    PWSTR pszPath = file_dialog_open(hwnd, locale_str(STR_IMPORT_FACE_DATA), rgSpec, 1);
    if (pszPath) {
        uint8_t *face_data = er_face_data_from_file(pszPath);
        if (face_data && er_face_data_import(save_data, item, face_data)) {
            uint8_t available, gender;
            er_face_data_info(face_data, &available, &gender);
            wchar_t body_type[32];
            wsprintfW(body_type, L"%s", locale_str(available ? (gender ? STR_TYPE_B : STR_TYPE_A) : STR_EMPTY));
            ListView_SetItemText(list_view_faces, item, 1, body_type);
            MessageBoxW(hwnd, locale_str(STR_IMPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd, locale_str(STR_IMPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        }
        er_face_data_free(face_data);
        CoTaskMemFree(pszPath);
    }
}

/* Function to export face data to a file */
static void export_face_data(HWND hwnd, int item) {
    COMDLG_FILTERSPEC rgSpec[] = {
        { locale_str(STR_ALL_FILES), L"*.*" }
    };
    PWSTR pszPath = file_dialog_save(hwnd, locale_str(STR_EXPORT_FACE_DATA), rgSpec, 1);
    if (pszPath) {
        const uint8_t *face_data = er_face_data_ref(save_data, item);
        if (face_data && er_face_data_to_file(face_data, pszPath)) {
            MessageBoxW(hwnd, locale_str(STR_EXPORT_SUCCESS), locale_str(STR_SUCCESS), MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hwnd, locale_str(STR_EXPORT_FAILED), locale_str(STR_ERROR), MB_OK | MB_ICONERROR);
        }
        CoTaskMemFree(pszPath);
    }
}

/* Function to handle faces ListView popup menu */
static void list_view_faces_popup_menu(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    /* Get the item under the cursor */
    POINT pt;
    pt.x = GET_X_LPARAM(lparam);
    pt.y = GET_Y_LPARAM(lparam);
    ScreenToClient(list_view_faces, &pt);

    /* Get the item under the cursor */
    LVHITTESTINFO lvhti = {0};
    lvhti.pt = pt;
    int item = ListView_HitTest(list_view_faces, &lvhti);

    if (item < 0) {
        return;
    }

    /* Create popup menu */
    HMENU menu = CreatePopupMenu();

    if (menu) {
        /* Add menu items */
        AppendMenuW(menu, MF_BYPOSITION | MF_STRING, IDM_IMPORT_FACE, locale_str(STR_IMPORT_FACE_DATA));
        AppendMenuW(menu, MF_BYPOSITION | MF_STRING, IDM_EXPORT_FACE, locale_str(STR_EXPORT_FACE_DATA));
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)embedded_face_data_menu, locale_str(STR_IMPORT_NPC_FACE_DATA));

        /* Convert window coordinates back to screen coordinates */
        ClientToScreen(list_view_faces, &pt);
        /* Show menu at cursor position */
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        RemoveMenu(menu, (UINT_PTR)embedded_face_data_menu, MF_BYCOMMAND);
        DestroyMenu(menu);
    }
}

/* Face data management modal dialog procedure */
LRESULT CALLBACK face_data_dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        /* Theme: paint dialog body and child controls in dark colors. */
        case WM_ERASEBKGND:
            if (theme_core_on_erasebkgnd(hwnd, (HDC)wparam)) {
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 1);
                return TRUE;
            }
            return FALSE;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            INT_PTR br = theme_core_dlg_ctlcolor((HDC)wparam, msg);
            if (br) {
                return br;
            }
            return FALSE;
        }

        case WM_INITDIALOG: {
            HMODULE module = GetModuleHandle(NULL);

            /* Set localized dialog title */
            SetWindowTextW(hwnd, locale_str(STR_FACES));

            /* Create Faces ListView filling the entire client area */
            list_view_faces = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                WC_LISTVIEW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                0, 0, 100, 100,
                hwnd, (HMENU)3, module, NULL
            );
            ListView_SetExtendedListViewStyleEx(list_view_faces,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT);
            SendMessage(list_view_faces, WM_SETFONT, (WPARAM)default_font, TRUE);

            /* Add columns to Faces ListView */
            LVCOLUMNW lvc;
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

            lvc.iSubItem = 0;
            lvc.cx = 100;
            lvc.pszText = (LPWSTR)locale_str(STR_SLOT);
            ListView_InsertColumn(list_view_faces, 0, &lvc);

            lvc.iSubItem = 1;
            lvc.cx = 100;
            lvc.pszText = (LPWSTR)locale_str(STR_BODY_TYPE);
            ListView_InsertColumn(list_view_faces, 1, &lvc);

            /* Create face action buttons below the ListView (initially disabled) */
            button_import_face = CreateWindowW(
                L"BUTTON", locale_str(STR_IMPORT_FACE_DATA),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                0, 0, 100, 25,
                hwnd, (HMENU)IDC_BUTTON_IMPORT_FACE, module, NULL
            );
            SendMessage(button_import_face, WM_SETFONT, (WPARAM)default_font, TRUE);

            button_export_face = CreateWindowW(
                L"BUTTON", locale_str(STR_EXPORT_FACE_DATA),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                0, 0, 100, 25,
                hwnd, (HMENU)IDC_BUTTON_EXPORT_FACE, module, NULL
            );
            SendMessage(button_export_face, WM_SETFONT, (WPARAM)default_font, TRUE);

            button_npc_face = CreateWindowW(
                L"BUTTON", locale_str(STR_IMPORT_NPC_FACE_DATA),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                0, 0, 100, 25,
                hwnd, (HMENU)IDC_BUTTON_NPC_FACE, module, NULL
            );
            SendMessage(button_npc_face, WM_SETFONT, (WPARAM)default_font, TRUE);

            /* Populate face data from current save */
            if (save_data) {
                for (int i = 0; i < 15; i++) {
                    const uint8_t *face_data = er_face_data_ref(save_data, i);
                    if (!face_data) continue;

                    uint8_t available, gender;
                    er_face_data_info(face_data, &available, &gender);

                    wchar_t slot_text[32];
                    wsprintfW(slot_text, L"%d", i + 1);
                    wchar_t body_type[32];
                    wsprintfW(body_type, L"%s", locale_str(available ? (gender ? STR_TYPE_B : STR_TYPE_A) : STR_EMPTY));

                    LVITEMW lvi = {0};
                    lvi.mask = LVIF_TEXT;
                    lvi.iItem = i;
                    lvi.iSubItem = 0;
                    lvi.pszText = slot_text;
                    lvi.iItem = ListView_InsertItem(list_view_faces, &lvi);
                    ListView_SetItemText(list_view_faces, lvi.iItem, 1, body_type);
                }
            }

            /* Layout ListView and buttons to fit the dialog */
            layout_face_dialog(hwnd);

            /* Apply theme to the dialog and all its controls. */
            theme_apply_to_window(hwnd);

            return TRUE;
        }

        case WM_SETTINGCHANGE:
            if (theme_core_on_setting_change(wparam, lparam)) {
                theme_apply_to_window(hwnd);
            }
            return FALSE;

        case WM_SYSCOLORCHANGE:
            if (theme_core_on_syscolor_change()) {
                theme_apply_to_window(hwnd);
            }
            return FALSE;

        case WM_THEMECHANGED:
            theme_apply_to_window(hwnd);
            return FALSE;

        case WM_SIZE: {
            if (list_view_faces) {
                layout_face_dialog(hwnd);
            }
            return TRUE;
        }

        case WM_NOTIFY: {
            NMHDR *nmhdr = (NMHDR *)lparam;
            if (nmhdr->hwndFrom == list_view_faces && nmhdr->code == NM_CUSTOMDRAW) {
                LRESULT r = theme_core_on_listview_customdraw((LPNMLVCUSTOMDRAW)lparam);
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, r);
                return TRUE;
            }
            if (nmhdr->hwndFrom == list_view_faces && nmhdr->code == LVN_ITEMCHANGED) {
                update_face_buttons();
            }
            return TRUE;
        }

        case WM_CONTEXTMENU: {
            if ((HWND)wparam == list_view_faces) {
                list_view_faces_popup_menu(hwnd, wparam, lparam);
            }
            return TRUE;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case IDC_BUTTON_IMPORT_FACE:
                case IDM_IMPORT_FACE: {
                    int item = ListView_GetNextItem(list_view_faces, -1, LVNI_SELECTED);
                    if (item == -1) return TRUE;
                    import_face_data(hwnd, item);
                    break;
                }

                case IDC_BUTTON_EXPORT_FACE:
                case IDM_EXPORT_FACE: {
                    int item = ListView_GetNextItem(list_view_faces, -1, LVNI_SELECTED);
                    if (item == -1) return TRUE;
                    export_face_data(hwnd, item);
                    break;
                }

                case IDC_BUTTON_NPC_FACE: {
                    /* Show the NPC face data popup menu below the button */
                    int item = ListView_GetNextItem(list_view_faces, -1, LVNI_SELECTED);
                    if (item == -1) break;
                    RECT rc;
                    GetWindowRect(button_npc_face, &rc);
                    TrackPopupMenu(embedded_face_data_menu,
                        TPM_LEFTALIGN | TPM_TOPALIGN,
                        rc.left, rc.bottom, 0, hwnd, NULL);
                    break;
                }

                case IDCANCEL: {
                    /* Handle Escape key */
                    list_view_faces = NULL;
                    button_import_face = NULL;
                    button_export_face = NULL;
                    button_npc_face = NULL;
                    EndDialog(hwnd, 0);
                    return TRUE;
                }

                default: {
                    int id = LOWORD(wparam);
                    if (id >= IDM_EMBEDDED_FACE_DATA_START && id < IDM_EMBEDDED_FACE_DATA_START + 200) {
                        int item = ListView_GetNextItem(list_view_faces, -1, LVNI_SELECTED);
                        if (item == -1) return TRUE;
                        on_import_embedded_face_data(hwnd, id - IDM_EMBEDDED_FACE_DATA_START, item);
                    }
                    break;
                }
            }
            return TRUE;
        }

        case WM_CLOSE: {
            list_view_faces = NULL;
            button_import_face = NULL;
            button_export_face = NULL;
            button_npc_face = NULL;
            EndDialog(hwnd, 0);
            return TRUE;
        }
    }
    return FALSE;
}
