file(GLOB_RECURSE SRCS "*.c")
idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "." "ui"
                       EMBED_FILES "ui/icons/mdi.ttf"   # Material Design Icons (Apache-2.0), see tools/gen_icons.py
                       WHOLE_ARCHIVE)   # lv_mem_psram.c provides symbols LVGL needs
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
