# np2 espresso — PC-98 emulator (np2kai i386c) on ESP32-S3 — ESP-IDF project (Path B).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Building a known-good project on a newer IDF/GCC. Two defenses so newer-compiler
# warnings in dependencies don't break the build:
#  - `-Wno-error`: don't promote warnings to errors globally.
#  - `-Wno-stringop-truncation`/`-Wno-stringop-overflow`: fully disable these GCC
#    warnings so they can't fire even if a component appends its own bare `-Werror`
#    after this (a disabled warning is not re-enabled by `-Werror`). esp-modbus does this.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error;-Wno-stringop-truncation;-Wno-stringop-overflow" APPEND)
project(np2_espresso)
