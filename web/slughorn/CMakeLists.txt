# Harness: build SlugHorn's core static library against the Emscripten
# toolchain as a *subdirectory* (not standalone), so SlugHorn's own
# CMakeLists.txt does not auto-configure the pybind11 Python bindings
# that only apply when SlugHorn is the top-level project.
#
# This is a check that thirdparty/slughorn's core `slughorn` CMake
# target really compiles under Emscripten. The shipped artifact
# (mud/web/slughorn.js + .wasm) is produced by build.sh, which drives
# em++ directly over the same two translation units -- CMake here, one
# em++ call there, both against the same vendored source.
#
#   emcmake cmake -S mud/web/slughorn -B <build-dir>
#   cmake --build <build-dir>
cmake_minimum_required(VERSION 3.15)
project(slughorn_wasm_harness LANGUAGES C CXX)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/slughorn" slughorn)
