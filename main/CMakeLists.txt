idf_component_register(
    SRCS "main.cpp" "dosio_sd.cpp" "platform_stubs.cpp" "lcd_st7789.cpp" "usb_kbd.cpp" "audio_i2s.cpp" "menu_disk.cpp" "bt_hid.cpp"
    INCLUDE_DIRS "."
    REQUIRES np2kai espressif__arduino-esp32 fatfs sdmmc TFT_eSPI usb espressif__usb_host_hid esp_driver_i2s driver nvs_flash bt esp_hid
)

# Platform files include the np2kai core headers, so they need the SAME target
# defines (so shared structs like SCRNMNG/FILEH match the precompiled core) + GCC14 flags.
# MUST match the np2kai core's define set (struct layouts like NP2CFG depend on
# SUPPORT_IDEIO etc.) — i286c/V30 build, no IA-32/IDEIO/FPU.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    __LIBRETRO__ NP2_ESP32 NP2KAI_GIT_TAG="esp32" NP2KAI_GIT_HASH="0000000"
    BIOS_IO_EMULATION OSLANG_UTF8 OSLINEBREAK_LF
    SUPPORT_8BPP SUPPORT_16BPP SUPPORT_24BPP SUPPORT_32BPP
    SUPPORT_CRT15KHZ SUPPORT_FAST_MEMORYCHECK
    SUPPORT_HRTIMER SUPPORT_KAI_IMAGES SUPPORT_LARGE_HDD SUPPORT_NORMALDISP
    SUPPORT_RESUME
    SUPPORT_SCSI SUPPORT_SMPU98 SUPPORT_SOFTKBD=0
    SUPPORT_STATSAVE=10 SUPPORT_UTF8 SUPPORT_V30EXT SUPPORT_V30ORIGINAL
    VAEG_FIX
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types
    -Wno-error=int-conversion -Wno-error=implicit-function-declaration
    -Wno-error=format -Wno-error=format-truncation -Wno-error=return-type -fno-strict-aliasing)
