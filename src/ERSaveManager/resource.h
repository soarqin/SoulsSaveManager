#pragma once

/* Application version and shared UI constants */
#define STAT_COUNT 8

/* Icon resource ID */
#define IDI_APP_ICON 101

/* Dialog resource IDs */
#define IDD_RENAME_CHARACTER 201
#define IDD_FACE_DATA 202

/* Dialog control IDs — used in .rc dialog templates */
/* NOTE: IDC_STATIC_CHARACTER_NAME and IDM_IMPORT_FACE both use value 1001.
 * This is intentional: they occupy different Win32 ID namespaces
 * (dialog control vs. menu command), so there is no conflict. */
#define IDC_STATIC_CHARACTER_NAME 1001
#define IDC_EDIT_CHARACTER_NAME 1002

/* Menu command IDs and main-window control IDs */
#define IDM_IMPORT_FACE 1001
#define IDM_EXPORT_FACE 1002
#define IDM_IMPORT_CHAR 1003
#define IDM_EXPORT_CHAR 1004
#define IDM_RENAME_CHAR 1005
#define IDM_COMPRESSION_FAST   1401
#define IDM_COMPRESSION_NORMAL 1402
#define IDM_COMPRESSION_MAX    1403
#define IDM_THEME_SYSTEM       1410
#define IDM_THEME_LIGHT        1411
#define IDM_THEME_DARK         1412
#define IDM_TOOLS_DOWNPATCH_1_02_1 1420
#define IDC_BUTTON_CHANGE_FOLDER 1
#define IDC_COMBO_SAVE_FOLDER 2
#define IDC_BUTTON_MANAGE_FACES 3
#define IDC_COMBO_COMPRESSION_LEVEL 5
#define IDC_BUTTON_IMPORT_CHAR 10
#define IDC_BUTTON_EXPORT_CHAR 11
#define IDC_BUTTON_RENAME_CHAR 12
#define IDC_BUTTON_IMPORT_FACE 13
#define IDC_BUTTON_EXPORT_FACE 14
#define IDC_BUTTON_NPC_FACE 15
#define IDM_EMBEDDED_FACE_DATA_START 1100 /* Start ID for embedded face data menu items, range: 1100-1299 */
#define IDM_LOCALE_START 1300  /* Start ID for locale menu items, range: 1300-1399 */

