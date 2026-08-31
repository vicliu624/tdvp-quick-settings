cmake_minimum_required(VERSION 3.10)

project(tdvp_quick_settings VERSION 0.1.2 LANGUAGES C CXX)

option(TDVP_QS_BUILD_WAYLAND "Build the Wayland layer-shell executable" ON)
option(TDVP_QS_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(TDVP_QS_INSTALL_K230_AUTOSTART "Install the TDVP K230 autostart entry" OFF)

include(CTest)

set(TDVP_QS_COMMON_WARNINGS -Wall -Wextra -Wpedantic)
if(TDVP_QS_WARNINGS_AS_ERRORS)
    list(APPEND TDVP_QS_COMMON_WARNINGS -Werror)
endif()

add_library(tdvp-quick-settings-core STATIC
    src/core/controller.cpp
    src/core/layout.cpp
    src/core/orientation.cpp
    src/core/status.cpp
)
target_include_directories(tdvp-quick-settings-core PUBLIC src)
target_compile_features(tdvp-quick-settings-core PUBLIC cxx_std_17)
target_compile_options(tdvp-quick-settings-core PRIVATE ${TDVP_QS_COMMON_WARNINGS})

if(BUILD_TESTING)
    add_executable(tdvp-quick-settings-core-tests tests/core_tests.cpp)
    target_link_libraries(tdvp-quick-settings-core-tests PRIVATE tdvp-quick-settings-core)
    target_compile_features(tdvp-quick-settings-core-tests PRIVATE cxx_std_17)
    target_compile_options(tdvp-quick-settings-core-tests PRIVATE ${TDVP_QS_COMMON_WARNINGS})
    add_test(NAME tdvp-quick-settings-core-tests COMMAND tdvp-quick-settings-core-tests)
    if(UNIX)
        add_executable(tdvp-quick-settings-wifi-tests
            tests/wifi_tests.cpp
            src/wayland/wifi_nmcli.cpp
        )
        target_include_directories(tdvp-quick-settings-wifi-tests PRIVATE src)
        target_compile_features(tdvp-quick-settings-wifi-tests PRIVATE cxx_std_17)
        target_compile_options(tdvp-quick-settings-wifi-tests PRIVATE ${TDVP_QS_COMMON_WARNINGS})
        add_test(NAME tdvp-quick-settings-wifi-tests COMMAND tdvp-quick-settings-wifi-tests)
    endif()
endif()

if(TDVP_QS_BUILD_WAYLAND AND UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
    find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

    set(TDVP_QS_PROTOCOL_XML
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-layer-shell-unstable-v1.xml)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir wayland-protocols
        OUTPUT_VARIABLE TDVP_QS_WAYLAND_PROTOCOLS_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(TDVP_QS_XDG_SHELL_PROTOCOL
        ${TDVP_QS_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
    if(NOT EXISTS ${TDVP_QS_XDG_SHELL_PROTOCOL})
        message(FATAL_ERROR "wayland-protocols did not provide xdg-shell.xml")
    endif()
    set(TDVP_QS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${TDVP_QS_GENERATED_DIR})
    set(TDVP_QS_PROTOCOL_HEADER
        ${TDVP_QS_GENERATED_DIR}/wlr-layer-shell-client-protocol.h)
    set(TDVP_QS_PROTOCOL_SOURCE
        ${TDVP_QS_GENERATED_DIR}/wlr-layer-shell-protocol.c)
    set(TDVP_QS_XDG_SHELL_HEADER
        ${TDVP_QS_GENERATED_DIR}/xdg-shell-client-protocol.h)
    set(TDVP_QS_XDG_SHELL_SOURCE
        ${TDVP_QS_GENERATED_DIR}/xdg-shell-protocol.c)

    add_custom_command(
        OUTPUT ${TDVP_QS_PROTOCOL_HEADER} ${TDVP_QS_PROTOCOL_SOURCE}
               ${TDVP_QS_XDG_SHELL_HEADER} ${TDVP_QS_XDG_SHELL_SOURCE}
        COMMAND ${WAYLAND_SCANNER} client-header ${TDVP_QS_PROTOCOL_XML}
                ${TDVP_QS_PROTOCOL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${TDVP_QS_PROTOCOL_XML}
                ${TDVP_QS_PROTOCOL_SOURCE}
        COMMAND ${WAYLAND_SCANNER} client-header ${TDVP_QS_XDG_SHELL_PROTOCOL}
                ${TDVP_QS_XDG_SHELL_HEADER}
        COMMAND ${WAYLAND_SCANNER} private-code ${TDVP_QS_XDG_SHELL_PROTOCOL}
                ${TDVP_QS_XDG_SHELL_SOURCE}
        DEPENDS ${TDVP_QS_PROTOCOL_XML} ${TDVP_QS_XDG_SHELL_PROTOCOL}
        VERBATIM
    )

    add_executable(tdvp-quick-settings
        src/main.cpp
        src/wayland/provider_client.cpp
        src/wayland/wifi_nmcli.cpp
        src/wayland/wayland_app.cpp
        ${TDVP_QS_PROTOCOL_SOURCE}
        ${TDVP_QS_XDG_SHELL_SOURCE}
    )
    target_include_directories(tdvp-quick-settings PRIVATE
        src
        ${TDVP_QS_GENERATED_DIR}
    )
    target_compile_features(tdvp-quick-settings PRIVATE cxx_std_17)
    target_compile_options(tdvp-quick-settings PRIVATE
        ${TDVP_QS_COMMON_WARNINGS}
        $<$<COMPILE_LANGUAGE:CXX>:-Os>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        -ffunction-sections
        -fdata-sections
    )
    set_property(TARGET tdvp-quick-settings APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
    target_link_libraries(tdvp-quick-settings PRIVATE
        tdvp-quick-settings-core
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::CAIRO
    )
    install(TARGETS tdvp-quick-settings RUNTIME DESTINATION bin)
endif()

if(TDVP_QS_INSTALL_K230_AUTOSTART)
    install(FILES integrations/k230/tdvp-quick-settings.desktop
        DESTINATION /etc/xdg/autostart)
endif()
