add_project(ersave_common STATIC
    ersave.c ersave.h
    ds3save.c ds3save.h
    file_dialog.c file_dialog.h
    save_compress.c save_compress.h
    locale_core.c locale_core.h
    config_core.c config_core.h
    theme_core.c theme_core.h
    FOLDER "common"
)

target_include_directories(ersave_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ersave_common PRIVATE UNICODE _UNICODE)
target_link_libraries(ersave_common PUBLIC md5 inih lzma shlwapi comctl32 ole32 oleaut32 uxtheme dwmapi bcrypt)
