# The store plane: SQLite with a VFS whose pages live in FoundationDB.
# See native/storeplane/README.md.
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

# One program for each property the store plane must hold, plus the load harness.
foreach(prog prove_handoff prove_concurrency prove_crash prove_big_commit integrity bench_vfs)
  add_executable(${prog} ${prog}.c)
  target_link_libraries(${prog} PRIVATE weft_fdb_vfs)
endforeach()
