
idf_component_register(SRCS "TFT_eSPI.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__arduino-esp32)

# Third-party library: don't let IDF's -Werror fire on its warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)