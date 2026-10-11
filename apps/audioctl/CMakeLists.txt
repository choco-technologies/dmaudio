# ======================================================================
#               audioctl - lists the audio devices, sets their volume through libdmaudio
# ======================================================================
set(DMOD_MODULE_NAME        audioctl)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           1)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/audioctl.dmr)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/main.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.7
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmaudio_if
)
