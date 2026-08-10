# The harness. Every plane and every edge links this, and none of them links iceoryx2.
#
# `weft::harness` gives three things:
#
#   - The bus. iceoryx2.sigs lists the C ABI, and Chromium's generate_stubs.py turns it
#     into a dlsym dispatch table. So a plane builds on a machine that has never seen
#     iceoryx2, and it fails at start rather than at link when the library is absent.
#     weft writes no Rust, and this keeps the Rust artifact out of the build graph too.
#   - The limits, in include/weft/limits.hpp. Every value is `Weft.Limits`, which is
#     rivet's. A plane includes them rather than choosing for itself.
#   - The payload, in include/weft/snapshot.hpp.
#
# `Weft.PlaneNetworkingTest` enforces the first of those. A plane that names an iceoryx2
# symbol and does not link this target fails.
#
# proof/ holds the two programs that show the bus works, and the bench that measured it.
# They are not the harness. See ../../docs/reference/harness.md.
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
set(API_H ${CMAKE_CURRENT_BINARY_DIR}/gen/iox2_api.h)

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
# Chromium's callers include the real library headers and weft has none on the path. A
# prototype that disagrees with the table it calls through is a crash with no diagnostic.
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

add_library(weft-harness STATIC ${STUB_CC} ${API_H})
add_library(weft::harness ALIAS weft-harness)
target_include_directories(weft-harness
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${CMAKE_CURRENT_SOURCE_DIR}/src
           ${CMAKE_CURRENT_BINARY_DIR}/gen
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(weft-harness PUBLIC ${CMAKE_DL_LIBS})

# The proof, and the bench that measured it. Off by default, because a plane that links
# the harness has no use for them.
option(WEFT_HARNESS_PROOF "Build the publish and subscribe proof, and the bench" ON)
if(WEFT_HARNESS_PROOF)
    foreach(prog publisher subscriber bench_send)
        add_executable(weft-harness-${prog} proof/${prog}.cpp)
        target_link_libraries(weft-harness-${prog} PRIVATE weft::harness)
        target_compile_options(weft-harness-${prog} PRIVATE -Wall -Wextra)
    endforeach()
endif()

# The same bench, linked straight against iceoryx2, so the dlsym table has something to be
# compared with. It needs an iceoryx2 install, so it is off unless asked for:
#   cmake -S native -B build -DWEFT_ICEORYX2_DIRECT=ON -DCMAKE_PREFIX_PATH=$PWD/.iceoryx
option(WEFT_ICEORYX2_DIRECT "Also build a directly linked bench, for comparison" OFF)
if(WEFT_ICEORYX2_DIRECT)
    find_package(iceoryx2-c REQUIRED)
    # Two baselines, because they bound the answer from both sides. A static link is the
    # strictest: it is the only build where the compiler could inline across the call.
    # A shared link is the realistic one: it already pays an indirect jump through the
    # PLT, which is most of what the dlsym table costs.
    foreach(link static shared)
        add_executable(weft-harness-bench-${link} proof/bench_send.cpp)
        target_include_directories(weft-harness-bench-${link}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(weft-harness-bench-${link} PRIVATE WEFT_ICEORYX2_DIRECT)
        target_link_libraries(weft-harness-bench-${link} PRIVATE iceoryx2-c::${link}-lib)
        target_compile_options(weft-harness-bench-${link} PRIVATE -Wall -Wextra)
    endforeach()
endif()
