# Every native plane and edge, in one build.
#
# Each directory was its own CMake project, so each configured on its own and none could
# share anything. A plane that needs the bus would then carry its own copy of the dlsym
# table, and a plane that needs a limit would choose its own number.
#
# This builds `weft::harness` first, and every plane and edge links it. There is one
# iceoryx2.sigs, one dispatch table, and one copy of the limits.
#
#   cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/build -j
#
# The subdirectories keep their own CMakeLists.txt, so each still builds alone.
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(weft-native LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# clangd reads compile_commands.json for the real include paths and flags. Without it
# clangd guesses, and every FoundationDB, SQLite, and generated header looks missing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The harness first. Everything below may link it.
add_subdirectory(harness)

# The planes. A plane has no networking.
add_subdirectory(dataplane)

# storeplane needs libfdb_c and sqlite3, and gyreplane needs libh2o and libfdb_c. Neither
# is in every environment, so each is opt-in rather than a configure failure for a person
# who only wants the bus.
option(WEFT_BUILD_STOREPLANE "Build the store plane. Needs libfdb_c and sqlite3." OFF)
if(WEFT_BUILD_STOREPLANE)
    add_subdirectory(storeplane)
endif()

option(WEFT_BUILD_GYREPLANE "Build the zone plane. Needs libh2o and libfdb_c." OFF)
if(WEFT_BUILD_GYREPLANE)
    add_subdirectory(gyreplane)
endif()

# The edges. An edge is a plane with networking. Neither has code yet, so neither has a
# CMakeLists.txt. Adding one that cannot build is worse than none, because CI then reports
# a failure that says nothing about the code. See edge/README.md.
