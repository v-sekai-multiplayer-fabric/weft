# The store plane: SQLite with a VFS whose pages live in FoundationDB.
# See docs/reference/native_store_plane.md.
cmake_minimum_required(VERSION 3.16)
project(weft_storeplane C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# clangd reads compile_commands.json for the real include paths and flags. Without it
# clangd guesses, and every FoundationDB and SQLite header looks missing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_path(FDB_INCLUDE_DIR foundationdb/fdb_c.h REQUIRED)
find_library(FDB_LIBRARY NAMES fdb_c REQUIRED)
find_path(SQLITE_INCLUDE_DIR sqlite3.h REQUIRED)
find_library(SQLITE_LIBRARY NAMES sqlite3 REQUIRED)

add_library(weft_fdb_vfs STATIC fdb_vfs.c)
target_include_directories(weft_fdb_vfs PUBLIC
  ${FDB_INCLUDE_DIR}/foundationdb ${SQLITE_INCLUDE_DIR})
target_compile_definitions(weft_fdb_vfs PUBLIC FDB_API_VERSION=730)
target_link_libraries(weft_fdb_vfs PUBLIC ${FDB_LIBRARY} ${SQLITE_LIBRARY} Threads::Threads)

add_executable(prove_handoff prove_handoff.c)
target_link_libraries(prove_handoff PRIVATE weft_fdb_vfs)
