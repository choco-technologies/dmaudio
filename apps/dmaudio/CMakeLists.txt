# ======================================================================
#               dmaudio - the audio service
# ======================================================================
# One instance per audio device, started by the device rule in
# configs/dmaudio.rules from configs/dmaudio@.ini. All it does is
# libdmaudio_serve(): its thread mixes and writes the periods, so it runs
# above the applications.
set(DMOD_MODULE_NAME        dmaudio)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           3)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmaudio.dmr)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/main.c
)

# dmosi:      (its headers, libsystemd's types).
# libsystemd: graceful stop.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.7
    dmosi
    libsystemd
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmaudio_if
)
