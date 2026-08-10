# The iceoryx2 proof.
#
# This directory holds the smallest thing that must work before a thread-per-core harness
# is worth writing: one message across the bus, checked at the other end.
#
# Nothing here links iceoryx2 and nothing here includes an iceoryx2 header. iceoryx2.sigs
# lists the C ABI, and Chromium's generate_stubs.py turns it into a dlsym dispatch table.
# So this builds on a machine that has never seen iceoryx2, and it fails at start rather
# than at link when the library is absent. weft writes no Rust, and this keeps the Rust
# artifact out of weft's build graph as well.
#
#   cmake -S native/harness -B native/harness/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/harness/build -j
#
# See ../../docs/reference/harness.md.
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(weft-harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen/native/harness)
set(STUB_CC ${STUB_DIR}/iceoryx2_stubs.cc)
set(STUB_H ${STUB_DIR}/iceoryx2_stubs.h)

# The generator runs when the signature file is newer than the table, and not on every
# build. -p sets the namespace and the include path the generated .cc uses for its own
# header, so it must match the STUB_DIR tail.
add_custom_command(
    OUTPUT ${STUB_CC} ${STUB_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STUB_DIR}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/generate_stubs/generate_stubs.py
            -o ${STUB_DIR} -i ${STUB_DIR}
            -t posix_stubs -s iceoryx2_stubs -p native/harness
            -l "(::std::cerr)" -n iostream
            --macro-include src/stubgen_compat.h
            ${CMAKE_CURRENT_SOURCE_DIR}/iceoryx2.sigs
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/iceoryx2.sigs
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stubgen_compat.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/iox2_decls.h
    COMMENT "Generating the iceoryx2 dlsym dispatch table from iceoryx2.sigs"
    VERBATIM)

# The prototypes, from the same signature file. generate_stubs.py emits none, because
# Chromium's callers include the real library headers and weft has none on the path.
set(API_H ${CMAKE_CURRENT_BINARY_DIR}/gen/iox2_api.h)
add_custom_command(
    OUTPUT ${API_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sigs_to_header.py
            ${CMAKE_CURRENT_SOURCE_DIR}/iceoryx2.sigs ${API_H}
            WEFT_HARNESS_IOX2_API_H iox2_decls.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/iceoryx2.sigs
            ${CMAKE_CURRENT_SOURCE_DIR}/sigs_to_header.py
    COMMENT "Generating the iceoryx2 prototypes from iceoryx2.sigs"
    VERBATIM)

add_library(weft-iceoryx2-stubs STATIC ${STUB_CC} ${API_H})
target_include_directories(weft-iceoryx2-stubs
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/gen
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(weft-iceoryx2-stubs PUBLIC ${CMAKE_DL_LIBS})

foreach(prog publisher subscriber)
    add_executable(weft-harness-${prog} src/${prog}.cpp)
    target_include_directories(weft-harness-${prog}
        PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/gen)
    target_link_libraries(weft-harness-${prog} PRIVATE weft-iceoryx2-stubs)
    target_compile_options(weft-harness-${prog} PRIVATE -Wall -Wextra)
endforeach()
