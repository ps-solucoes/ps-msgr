# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)
find_package(cmocka 2.0 CONFIG)
if(NOT cmocka_FOUND)
    message(FATAL_ERROR "The tests need cmocka >= 2.0 (the build container has it). "
                        "Configure with -DPSMSGR_BUILD_TESTS=OFF to build without them.")
endif()

# cmocka catches SIGSEGV and SIGBUS to fail the test; ASan's report of the
# same crash, with its stack trace, is more useful.
set(_psmsgr_test_env "")
if(PSMSGR_SANITIZE MATCHES "address")
    list(APPEND _psmsgr_test_env "ASAN_OPTIONS=allow_user_segv_handler=0")
endif()

# cmocka test functions take a `void **state` most of them ignore. Linked
# after psmsgr_flags: with clang, the last of -Wextra and this one wins.
add_library(psmsgr_test_flags INTERFACE)
target_compile_options(psmsgr_test_flags INTERFACE -Wno-unused-parameter)

# Tests call the public API through the shared library, exactly like the
# bindings do. With STATIC they link the static library instead, which also
# gives them the internal test hooks (src/internal.h); the .so exports none.
# LABEL defaults to unit. ARGS are passed to the test program.
function(psmsgr_add_test name source)
    cmake_parse_arguments(ARG "STATIC" "LABEL" "ARGS" ${ARGN})
    if(NOT ARG_LABEL)
        set(ARG_LABEL unit)
    endif()
    add_executable(${name} ${source})
    if(ARG_STATIC)
        target_link_libraries(${name} PRIVATE psmsgr_static)
    else()
        target_link_libraries(${name} PRIVATE psmsgr)
    endif()
    target_link_libraries(${name} PRIVATE psmsgr_flags psmsgr_test_flags
                          cmocka::cmocka Threads::Threads)
    # layout.h: tests inspect and corrupt channel files from the outside.
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
    add_test(NAME ${name} COMMAND ${name} ${ARG_ARGS})
    set_tests_properties(${name} PROPERTIES LABELS ${ARG_LABEL} TIMEOUT 300)
    if(_psmsgr_test_env)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_psmsgr_test_env}")
    endif()
endfunction()

psmsgr_add_test(test_common test_common.c)
psmsgr_add_test(test_headers test_headers.c)
psmsgr_add_test(test_state test_state.c)
psmsgr_add_test(test_state_hooks test_state_hooks.c STATIC)
psmsgr_add_test(test_waitset test_waitset.c)
# Seqlock torture: select with ctest -L torture, skip with -LE torture.
psmsgr_add_test(test_torture test_torture.c STATIC LABEL torture)
if(TARGET psmsgr-dump)
    # Under qemu-user the test itself runs emulated, and an armhf child only
    # runs through the emulator too: exec passes straight to the host kernel.
    psmsgr_add_test(test_dump test_dump.c
        ARGS ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:psmsgr-dump>)
endif()

# The C side of the binding tests: struct layouts and an interop writer.
add_executable(interop_helper interop_helper.c)
target_link_libraries(interop_helper PRIVATE psmsgr psmsgr_flags)

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(test_headers_cxx test_headers_cxx.cpp)
    target_link_libraries(test_headers_cxx PRIVATE psmsgr)
    target_compile_options(test_headers_cxx PRIVATE -Wall -Wextra -Wpedantic
                           $<$<BOOL:${PSMSGR_WERROR}>:-Werror>)
    add_test(NAME test_headers_cxx COMMAND test_headers_cxx)
    set_tests_properties(test_headers_cxx PROPERTIES LABELS unit)
endif()

add_test(NAME abi_check
    COMMAND ${CMAKE_COMMAND}
        -DLIB=$<TARGET_FILE:psmsgr>
        -DMAP=${PROJECT_SOURCE_DIR}/src/libpsmsgr.map
        -DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
        -DVERSION=${PROJECT_VERSION}
        -DNM=${CMAKE_NM}
        -DREADELF=${CMAKE_READELF}
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckAbi.cmake)
set_tests_properties(abi_check PROPERTIES LABELS abi)
