# The data plane, and nothing else.
#
# weft is the control plane. It keeps two native things and no more:
#
#   dataplane   the seqlock ring a plane writes and the BEAM samples
#   nif         the NIF the BEAM loads to read that ring, built by ../Makefile
#
# Every other plane and edge is its own repository, its own process, and its own
# container. weft supervises them as black boxes and reaches them only through the data
# plane. See ../lib/weft.ex.
#
#   cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/build -j
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(weft-native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(dataplane)
