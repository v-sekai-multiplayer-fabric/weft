cmake_minimum_required(VERSION 3.18.0)
project(zone-server-h2o)

# Fil-C toolchain: build with -DCMAKE_TOOLCHAIN_FILE=cmake/fil-c-toolchain.cmake
# once the Fil-C build is vendored (task #3 in the consolidation plan).
# Stock clang/gcc works for the pre-Fil-C spike (basic ZoneTick).

find_library(H2O_LIB h2o-evloop REQUIRED)
find_library(Z_LIB z REQUIRED)
find_library(SSL_LIB ssl REQUIRED)
find_library(CRYPTO_LIB crypto REQUIRED)
find_library(FDB_C_LIB fdb_c REQUIRED)
# yajl is gone with src/utility.c. It generated JSON response bodies for
# h2o_req_t, and a plane answers no request. h2o itself is down to
# h2o_evloop_create/run, so that fdb_future_set_callback has a loop. The
# thread-per-core harness over iceoryx takes that job, and h2o leaves.

find_path(H2O_INCLUDE h2o.h REQUIRED)
find_path(OPENSSL_INCLUDE openssl/ssl.h REQUIRED)
find_path(FDB_INCLUDE foundationdb/fdb_c.h REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(COMMON_OPTIONS -flto=auto -pthread -DFDB_API_VERSION=730)
set(WARNING_OPTIONS -pedantic -Wall -Wextra)

# NOTE: include_directories()/add_compile_options() were global here
# until the first real CI build (task #17) surfaced why that is wrong:
# they leak into every target added afterward in this scope. Include
# dirs and warning flags are now
# scoped to our own target only, below. (mujoco was vendored here too
# for a while, for task #8's entity/prop contact physics -- dropped in
# favor of Godot's own Jolt physics, already available without a
# second physics engine vendored into this process.)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_FORTIFY_SOURCE=2")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O3")

# A plane has no networking. The QUIC transport and its libraries moved to
# native/gyreedge, which is the edge. This build must not gain picoquic,
# picotls, or any HTTP/3 code again. See ../../docs/reference/gyre_plane.md.
#
# thirdparty/ is empty and gone. QCBOR had no caller left once upstream
# removed src/mud/mud_cbor.c, and thirdparty/taskweft-nif had none at
# all. libriscv went with the sandbox. picoquic and picotls went to the
# edge. What remains links against system libraries only.

# The libriscv guest sandbox (src/sandbox/, RFD 0092/0094) is gone, and
# with it enable_language(CXX). A plane runs one runtime model, the
# thread-per-core harness, and a second sandbox inside it is not that
# model. This build is C only again.

file(GLOB_RECURSE HANDLER_SOURCES "src/handlers/*.c")
file(GLOB SOURCES "src/*.c")
file(GLOB GEN_SOURCES "src/gen/*.c")
file(GLOB PHYSICS_SOURCES "src/physics/*.c")

add_executable(${PROJECT_NAME} ${HANDLER_SOURCES} ${SOURCES} ${GEN_SOURCES} ${PHYSICS_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    src ${CMAKE_BINARY_DIR} ${H2O_INCLUDE} ${OPENSSL_INCLUDE} ${FDB_INCLUDE})
target_compile_definitions(${PROJECT_NAME} PRIVATE H2O_USE_LIBUV=0)
target_compile_options(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS} ${WARNING_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_OPTIONS})
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${H2O_LIB} ${FDB_C_LIB} ${SSL_LIB} ${CRYPTO_LIB} ${Z_LIB} m)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
