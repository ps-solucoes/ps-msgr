# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# Single source of truth for the version: the macros in psmsgr.h.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/psmsgr/psmsgr.h" _psmsgr_version_lines
     REGEX "^#define PSMSGR_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_part MAJOR MINOR PATCH)
    string(REGEX REPLACE ".*#define PSMSGR_VERSION_${_part} ([0-9]+).*" "\\1"
           _psmsgr_${_part} "${_psmsgr_version_lines}")
endforeach()

project(psmsgr
    VERSION "${_psmsgr_MAJOR}.${_psmsgr_MINOR}.${_psmsgr_PATCH}"
    DESCRIPTION "Inter-process state exchange over shared memory"
    HOMEPAGE_URL "https://github.com/ps-solucoes/ps-msgr"
    LANGUAGES C)

include(CheckCCompilerFlag)
include(CheckLinkerFlag)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PSMSGR_BUILD_TESTS "Build the unit and torture tests" ${PROJECT_IS_TOP_LEVEL})
option(PSMSGR_BUILD_TOOLS "Build psmsgr-dump" ON)
option(PSMSGR_BUILD_BENCH "Build psmsgr-bench (shipped in the tools package)" OFF)
option(PSMSGR_BUILD_EXAMPLES "Build the C examples (examples/c; not installed)" OFF)
option(PSMSGR_WERROR "Treat compiler warnings as errors" OFF)
set(PSMSGR_SANITIZE "" CACHE STRING
    "Sanitizers to enable, as passed to -fsanitize= (e.g. address,undefined or thread)")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ps-msgr supports Linux only")
endif()
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    message(FATAL_ERROR "ps-msgr channel files are little-endian; big-endian targets are unsupported")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- flags ------------------------------------------------------------------

add_library(psmsgr_flags INTERFACE)
target_compile_options(psmsgr_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wstrict-prototypes -Wmissing-prototypes
    $<$<BOOL:${PSMSGR_WERROR}>:-Werror>)

# Hardening, matching Debian's dpkg-buildflags. _FORTIFY_SOURCE needs
# optimization, so it is left out of Debug builds.
target_compile_options(psmsgr_flags INTERFACE
    -fstack-protector-strong
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>)
check_c_compiler_flag(-fstack-clash-protection PSMSGR_HAVE_STACK_CLASH_PROTECTION)
if(PSMSGR_HAVE_STACK_CLASH_PROTECTION)
    target_compile_options(psmsgr_flags INTERFACE -fstack-clash-protection)
endif()
target_link_options(psmsgr_flags INTERFACE LINKER:-z,relro LINKER:-z,now)

if(PSMSGR_SANITIZE)
    add_compile_options(-fsanitize=${PSMSGR_SANITIZE} -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${PSMSGR_SANITIZE})
endif()

# ---- library ----------------------------------------------------------------

set(PSMSGR_SOURCES
    src/common.c
    src/seqcopy.c
    src/state.c
    src/waitset.c)

add_library(psmsgr_objects OBJECT ${PSMSGR_SOURCES})
target_include_directories(psmsgr_objects PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(psmsgr_objects PRIVATE _GNU_SOURCE)
target_link_libraries(psmsgr_objects PRIVATE psmsgr_flags)

add_library(psmsgr SHARED $<TARGET_OBJECTS:psmsgr_objects>)
add_library(psmsgr_static STATIC $<TARGET_OBJECTS:psmsgr_objects>)
add_library(psmsgr::psmsgr ALIAS psmsgr)
add_library(psmsgr::psmsgr_static ALIAS psmsgr_static)

set(_psmsgr_map "${CMAKE_CURRENT_SOURCE_DIR}/src/libpsmsgr.map")
set_target_properties(psmsgr PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    LINK_DEPENDS "${_psmsgr_map}")
set_target_properties(psmsgr_static PROPERTIES OUTPUT_NAME psmsgr)
target_link_libraries(psmsgr PRIVATE psmsgr_flags)
target_link_options(psmsgr PRIVATE LINKER:--version-script=${_psmsgr_map})
# Fails the link when the map lists a symbol that is not defined (binutils 2.40).
check_linker_flag(C "LINKER:--no-undefined-version" PSMSGR_HAVE_NO_UNDEFINED_VERSION)
if(PSMSGR_HAVE_NO_UNDEFINED_VERSION)
    target_link_options(psmsgr PRIVATE LINKER:--no-undefined-version)
endif()
# Sanitizer runtimes leave undefined symbols in shared objects (clang).
if(NOT PSMSGR_SANITIZE)
    target_link_options(psmsgr PRIVATE LINKER:--no-undefined)
endif()

foreach(_target psmsgr psmsgr_static)
    target_include_directories(${_target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endforeach()

# ---- install / package ------------------------------------------------------

install(TARGETS psmsgr EXPORT psmsgrTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT runtime NAMELINK_COMPONENT dev)
install(TARGETS psmsgr_static EXPORT psmsgrTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev)
install(DIRECTORY include/psmsgr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)

install(EXPORT psmsgrTargets NAMESPACE psmsgr::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psmsgr COMPONENT dev)
configure_package_config_file(cmake/psmsgrConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/psmsgrConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psmsgr)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/psmsgrConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/psmsgrConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/psmsgrConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/psmsgr COMPONENT dev)

# Relocatable: prefix is derived from the .pc file's own location.
file(RELATIVE_PATH PSMSGR_PC_PREFIX_REL "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
string(REGEX REPLACE "/$" "" PSMSGR_PC_PREFIX_REL "${PSMSGR_PC_PREFIX_REL}")
configure_file(cmake/psmsgr.pc.in "${CMAKE_CURRENT_BINARY_DIR}/psmsgr.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/psmsgr.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT dev)

if(PSMSGR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(PSMSGR_BUILD_BENCH)
    add_subdirectory(bench)
endif()
if(PSMSGR_BUILD_EXAMPLES)
    add_subdirectory(examples/c)
endif()

set(CPACK_PACKAGE_NAME psmsgr)
set(CPACK_PACKAGE_VENDOR "PS Soluções")
set(CPACK_PACKAGE_CONTACT "ps-msgr maintainers")
set(CPACK_GENERATOR DEB)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime dev)
if(PSMSGR_BUILD_TOOLS OR PSMSGR_BUILD_BENCH)
    list(APPEND CPACK_COMPONENTS_ALL tools)
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME libpsmsgr${PROJECT_VERSION_MAJOR})
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION libs)
set(CPACK_DEBIAN_DEV_PACKAGE_NAME libpsmsgr-dev)
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION libdevel)
set(CPACK_COMPONENT_DEV_DEPENDS runtime)
set(CPACK_DEBIAN_TOOLS_PACKAGE_NAME psmsgr-tools)
set(CPACK_DEBIAN_TOOLS_PACKAGE_SECTION utils)
set(CPACK_COMPONENT_TOOLS_DEPENDS runtime)
# Lets dpkg-shlibdeps find the not yet installed libpsmsgr.so.1 that the
# tools link; the dependency on it is the component dependency above.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_CURRENT_BINARY_DIR}")
foreach(comp IN LISTS CPACK_COMPONENTS_ALL)
    string(TOUPPER ${comp} COMP)
    install(FILES cmake/debian-copyright RENAME copyright
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${CPACK_DEBIAN_${COMP}_PACKAGE_NAME}
        COMPONENT ${comp})
endforeach()
include(CPack)

# ---- tests ------------------------------------------------------------------

if(PSMSGR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
