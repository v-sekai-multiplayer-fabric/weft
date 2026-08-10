cmake_minimum_required(VERSION 3.18.0)
project(zone-server-h2o)

# Fil-C toolchain: build with -DCMAKE_TOOLCHAIN_FILE=cmake/fil-c-toolchain.cmake
# once the Fil-C build is vendored (task #3 in the consolidation plan).
# Stock clang/gcc works for the pre-Fil-C spike (transport + basic ZoneTick).

find_library(H2O_LIB h2o-evloop REQUIRED)
find_library(Z_LIB z REQUIRED)
find_library(SSL_LIB ssl REQUIRED)
find_library(CRYPTO_LIB crypto REQUIRED)
find_library(FDB_C_LIB fdb_c REQUIRED)
# src/utility.c (ported as-is from h2o-bench-tpcc) genuinely uses yajl
# for JSON generation -- this was wrongly assumed TFB-only and dropped
# when this CMakeLists.txt was first written; the real CI build (task
# #17) caught the missing dependency.
find_library(YAJL_LIB yajl REQUIRED)

find_path(H2O_INCLUDE h2o.h REQUIRED)
find_path(OPENSSL_INCLUDE openssl/ssl.h REQUIRED)
find_path(FDB_INCLUDE foundationdb/fdb_c.h REQUIRED)
find_path(YAJL_INCLUDE yajl/yajl_gen.h REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COMMON_OPTIONS -flto=auto -pthread -DFDB_API_VERSION=730)
set(WARNING_OPTIONS -pedantic -Wall -Wextra)

# NOTE: include_directories()/add_compile_options() were global here
# until the first real CI build (task #17) surfaced why that is wrong:
# they leak into every target added afterward in this scope, including
# the vendored picoquic_vendored (same-scope include()) target in
# cmake/picoquic.cmake. Include dirs and warning flags are now
# scoped to our own target only, below. (mujoco was vendored here too
# for a while, for task #8's entity/prop contact physics -- dropped in
# favor of Godot's own Jolt physics, already available without a
# second physics engine vendored into this process.)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_FORTIFY_SOURCE=2")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O3")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/picoquic.cmake)
include(cmake/qcbor.cmake)

# The guest sandbox (src/sandbox/, RFD 0092/0094): libriscv built
# in-tree, C++20 (libriscv's own floor). RISCV_BINARY_TRANSLATION off
# for the same reason fly/Containerfile documented before the MUD
# removal: the default links libtcc, a full embedded C compiler, whose
# memory cost OOM-killed a 256 MB Fly machine -- and RFD 0092 gates
# execution budgets via ReBAC, not via a JIT racing them.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RISCV_BINARY_TRANSLATION OFF CACHE BOOL "" FORCE)
# libriscv's syscall handler table defaults to 512 entries, which is
# below src/sandbox/zone_abi.h's 600 block. Found by the kv_smoke
# guest, not predicted: the ecall threw
# "array::at: __n (which is 600) >= _Nm (which is 512)". 1024 keeps
# 600 valid and leaves godot-sandbox's own 500 block untouched, so a
# guest can speak both ABIs.
add_compile_definitions(RISCV_SYSCALLS_MAX=1024)
add_subdirectory(thirdparty/libriscv/lib)

file(GLOB_RECURSE HANDLER_SOURCES "src/handlers/*.c")
file(GLOB SOURCES "src/*.c")
file(GLOB TRANSPORT_SOURCES "src/transport/*.c")
file(GLOB GEN_SOURCES "src/gen/*.c")
file(GLOB PHYSICS_SOURCES "src/physics/*.c")
file(GLOB SANDBOX_SOURCES "src/sandbox/*.c" "src/sandbox/*.cpp")

add_executable(${PROJECT_NAME} ${HANDLER_SOURCES} ${SOURCES} ${TRANSPORT_SOURCES} ${GEN_SOURCES} ${PHYSICS_SOURCES} ${SANDBOX_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    src ${CMAKE_BINARY_DIR} ${H2O_INCLUDE} ${OPENSSL_INCLUDE} ${FDB_INCLUDE} ${YAJL_INCLUDE})
target_compile_definitions(${PROJECT_NAME} PRIVATE H2O_USE_LIBUV=0)
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS} ${WARNING_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${H2O_LIB} ${FDB_C_LIB} ${SSL_LIB} ${CRYPTO_LIB} ${Z_LIB} ${YAJL_LIB} picoquic_vendored qcbor_vendored riscv m)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
