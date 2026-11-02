# ======================================================================
#               test_libdmaudio
# ======================================================================
# The mixer without a node: a device registered by the test, its streams
# mixed with libdmaudio_device_mix().
set(DMOD_MODULE_NAME        test_libdmaudio)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         4096)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    libdmaudio_test.c
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.7
)

target_link_libraries(${DMOD_MODULE_NAME}
    libdmaudio_if
)

# Register the test module with ctest - the same two steps as CI: install its
# dependencies with dmf-get, then execute it through dmod_loader. The test's
# exit code equals its number of failed steps (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()

# Host regression tests exercise the real service loop with a scripted node.
# No module/runtime code is linked into the native test executable.
if(DMOD_TOOLS_NAME STREQUAL "arch/x86_64")
    add_executable(test_serve_recovery serve_recovery_test.c)
    target_compile_definitions(test_serve_recovery PRIVATE
        DMOD_libdmaudio DMOD_CURRENT_ALLOCATOR="test_serve_recovery")
    get_target_property(LIBDMAUDIO_INCLUDES libdmaudio INCLUDE_DIRECTORIES)
    target_include_directories(test_serve_recovery PRIVATE ${LIBDMAUDIO_INCLUDES})
    target_link_libraries(test_serve_recovery PRIVATE libdmaudio_if dmod_inc)
    add_test(NAME test_serve_recovery COMMAND test_serve_recovery)
endif()
