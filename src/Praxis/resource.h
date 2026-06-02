/**
 * @file resource.h
 * @brief Praxis Win32 resource identifiers.
 */

#pragma once

/* Menu IDs */
#define IDR_MAIN_MENU       100
#define IDM_FILE_EXIT       103
#define IDM_FILE_IMPORT_ORIGINAL    104
#define IDM_FILE_IMPORT_SLOT        105
#define IDM_GAME_MANAGE     131
#define IDM_GAME_PROFILE_FIRST 1300
#define IDM_GAME_PROFILE_LAST  1399
#define IDM_OPTIONS_HOTKEYS 140
/* Language submenu ids: dynamically replaced at WM_INITMENUPOPUP. The
 * static "English" entry in IDR_MAIN_MENU keeps IDM_OPTIONS_LANG so its
 * presence doubles as the sentinel that identifies the Language popup. */
#define IDM_OPTIONS_LANG    141
#define IDM_LANG_FIRST      1500
#define IDM_LANG_LAST       1599
/* Theme submenu items - static, no dynamic rebuild. The placeholder
 * IDM_OPTIONS_THEME is the sentinel that identifies the Theme popup
 * during WM_INITMENUPOPUP (mirrors IDM_OPTIONS_LANG behaviour). */
#define IDM_OPTIONS_THEME   142
#define IDM_THEME_SYSTEM    1700
#define IDM_THEME_LIGHT     1701
#define IDM_THEME_DARK      1702

/* Control IDs */
#define IDC_TREE_VIEW       200
#define IDC_STATUS_BAR      201

/* === Toolbar === */
#define IDC_TOOLBAR          210
#define IDC_PROFILE_COMBO    211
#define IDC_BTN_ADD_BACKUP   212
#define IDC_BTN_DEL_BACKUP   213
#define IDC_BTN_BACKUP_FULL  214
#define IDC_BTN_BACKUP_SLOT  215
#define IDC_BTN_RESTORE      216
#define IDC_BTN_UNDO         217
#define IDC_BTN_BACKUP_REPLACE 218
#define IDC_SORT_COMBO       219

/* Dialog IDs */
#define IDD_HOTKEY_SETTINGS 300
#define IDD_GAME_PROFILE_MANAGER 310
#define IDD_EDIT_GAME_PROFILE    311
#define IDD_EDIT_BACKUP_PROFILE  312
#define IDD_IMPORT_DIALOG        320

/* Import Dialog Controls */
#define IDC_IMPORT_LIST             5001
#define IDC_IMPORT_SELECT_ALL       5002
#define IDC_IMPORT_DESELECT_ALL     5003

/* === Game Profile Manager Dialog Controls === */
#define IDC_GPM_LIST   4001
#define IDC_GPM_ADD    4002
#define IDC_GPM_EDIT   4003
#define IDC_GPM_DELETE 4004
#define IDC_GPM_CLOSE  4005

/* === Edit Game Profile Dialog Controls === */
#define IDC_EGP_NAME        4101
#define IDC_EGP_GAME        4102
#define IDC_EGP_SAVE_DIR    4103
#define IDC_EGP_TREE_ROOT   4104
#define IDC_EGP_BROWSE_SAVE 4105
#define IDC_EGP_BROWSE_TREE 4106
/* Labels (need IDs so they can be localized at runtime) */
#define IDC_EGP_LBL_NAME      4111
#define IDC_EGP_LBL_GAME      4112
#define IDC_EGP_LBL_SAVE_DIR  4113
#define IDC_EGP_LBL_TREE_ROOT 4114

/* === Edit Backup Profile Dialog Controls === */
#define IDC_EBP_NAME       4201
#define IDC_EBP_TREE_ROOT  4202
#define IDC_EBP_BROWSE     4203
/* Radio button IDs MUST be sequential and in display order so that
 * CheckRadioButton(NONE, HIGH, ...) covers the full range. */
#define IDC_EBP_COMP_NONE   4204
#define IDC_EBP_COMP_LOW    4205
#define IDC_EBP_COMP_MEDIUM 4206
#define IDC_EBP_COMP_HIGH   4207
/* Labels (need IDs so they can be localized at runtime) */
#define IDC_EBP_LBL_NAME        4211
#define IDC_EBP_LBL_COMPRESSION 4212

/* === Hotkey Settings Dialog Controls === */
#define IDC_HK_LBL_BACKUP_FULL 4301
#define IDC_HK_LBL_BACKUP_SLOT 4302
#define IDC_HK_LBL_RESTORE     4303
#define IDC_HK_LBL_UNDO        4304
#define IDC_HK_LBL_BACKUP_REPLACE 4305
#define IDC_HK_LBL_PREVIOUS_SAVE  4306
#define IDC_HK_LBL_NEXT_SAVE      4307
#define IDC_HK_BACKUP_FULL     4311
#define IDC_HK_BACKUP_SLOT     4312
#define IDC_HK_RESTORE         4313
#define IDC_HK_UNDO            4314
#define IDC_HK_BACKUP_REPLACE  4315
#define IDC_HK_PREVIOUS_SAVE   4316
#define IDC_HK_NEXT_SAVE       4317
#define IDC_HK_RESET           4321

/* Icon */
#define IDI_APPICON         1
