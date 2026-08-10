# QCBOR: vendored per multiplayer-fabric-manuals RFD 0001's own
# decision (QCBOR over zcbor, plain-CBOR RFC 8949 decoding inside the
# host process, no hand-written codec). Used by src/mud/mud_cbor.c,
# the host side of the mud-sandbox-orchestrator boundary.
#
# QCBOR is small (5 .c files, no external deps beyond libm for its
# float-conversion helpers), so this vendors it as a plain source list
# rather than add_subdirectory()-ing QCBOR's own CMakeLists.txt --
# consistent with how cmake/picoquic.cmake already treats
# thirdparty/picoquic/picotls as a source list, not a nested project.

set(QCBOR_DIR "${CMAKE_SOURCE_DIR}/thirdparty/QCBOR")

# v2 (dev branch, real commit pinned via git subtree add's own squash
# commit) splits the old monolithic qcbor_encode.c/qcbor_decode.c into
# several files (qcbor_main_encode.c, qcbor_number_decode.c, ...) --
# globbed rather than hardcoded so a future QCBOR update does not
# silently drop a new file the way a fixed list would.
file(GLOB QCBOR_SOURCES "${QCBOR_DIR}/src/*.c")

add_library(qcbor_vendored STATIC ${QCBOR_SOURCES})
target_include_directories(qcbor_vendored PUBLIC ${QCBOR_DIR}/inc)
# QCBOR's own ieee754.c/qcbor_decode.c use feclearexcept/lround/pow/
# exp2 etc. for its float-conversion helpers (used even though this
# project's own CBOR messages carry no floats) -- confirmed by a real
# link failure the first time this was built without -lm.
target_link_libraries(qcbor_vendored PUBLIC m)
