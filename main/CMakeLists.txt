file(GLOB_RECURSE SRCS "*.c")
idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "." "ui"
                       EMBED_FILES "ui/icons/mdi.ttf"   # Material Design Icons (Apache-2.0), see tools/gen_icons.py
                       # Inter (OFL-1.1) is compiled in as bitmap fonts: ui/fonts/*.c from tools/gen_fonts.py
                       WHOLE_ARCHIVE)   # lv_mem_psram.c provides symbols LVGL needs
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# esp_hosted SDIO data buffers go to PSRAM (main/hosted_rx_psram.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_aligned_alloc")
