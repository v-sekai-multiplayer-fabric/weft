# picoquic + picotls transport, vendored from the same sources the Godot
# fork's picoquic backend uses (V-Sekai-fire/multiplayer-fabric-build,
# godot/modules/http3), so the client (WebTransportPeer) and this server
# share one QUIC implementation.
#
# TLS 1.3 backend: OpenSSL, not mbedtls. This project first standardized
# on mbedtls to match that Godot module exactly, byte-for-byte. That
# decision got revisited once the real build (task #17) showed h2o's own
# CMakeLists.txt already hard-requires OpenSSL for libh2o-evloop itself
# (FIND_PACKAGE(OpenSSL REQUIRED), no option to disable, confirmed by
# reading h2o's CMakeLists.txt at its pinned commit directly) -- so
# OpenSSL was always a linked dependency of this binary regardless of
# what picoquic used. Switching picoquic to OpenSSL too removes the
# second TLS library (mbedtls) instead of carrying two. TLS 1.3 is a
# standard wire protocol: a server using picotls's OpenSSL backend still
# interoperates with the Godot client's mbedtls backend during the
# handshake, so this does not break client compatibility. What is lost
# is sharing thirdparty/picoquic-godot-patches/0002-godot-fixes.patch's
# mbedtls-specific glue fixes (buffer-based private key loading, ECDSA
# raw-to-DER signature conversion, packet-loop atomics) -- those patch
# picoquic_mbedtls.c/ptls_mbedtls.c specifically, not the OpenSSL
# backend, and are simply not needed for a build that does not compile
# that glue in.

set(PICOQUIC_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/picoquic)
set(PICOTLS_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/picotls)

file(GLOB PICOQUIC_CORE_SOURCES "${PICOQUIC_ROOT}/picoquic/*.c")
file(GLOB PICOHTTP_SOURCES "${PICOQUIC_ROOT}/picohttp/*.c")

# Fusion is unused (AES-GCM engine, x86-only optimization, not required
# for correctness) and has its own #if !defined(PTLS_WITHOUT_FUSION)
# guard in tls_api.c, so leaving fusion's .c files out of the build is
# safe. winsockloop.c is Windows-only.
#
# minicrypto is different: tls_api.c's picoquic_tls_api_init_providers()
# calls picoquic_ptls_minicrypto_load() with no compile-time guard at all
# (only a runtime flag check, TLS_API_INIT_FLAGS_NO_MINICRYPTO, which
# defaults to unset). Excluding picoquic_ptls_minicrypto.c left that call
# an undefined symbol at link time -- confirmed by the real build's own
# linker error ("undefined reference to picoquic_ptls_minicrypto_load"),
# not guessed. OpenSSL now wins as the active provider instead: per
# tls_api.c's own comment, "the latest registration wins," and with
# mbedtls's glue no longer compiled in, OpenSSL is the last provider
# picoquic_tls_api_init_providers() registers (minicrypto, then OpenSSL,
# with fusion and mbedtls both compiled out).
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_fusion\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*winsockloop\\.c$")

set(PICOTLS_SOURCES
    ${PICOTLS_ROOT}/lib/picotls.c
    ${PICOTLS_ROOT}/lib/pembase64.c
    ${PICOTLS_ROOT}/lib/hpke.c
    ${PICOTLS_ROOT}/lib/asn1.c
    ${PICOTLS_ROOT}/lib/openssl.c
)

# picoquic_ptls_minicrypto.c (kept in the build, see the exclusion-list
# comment above) links against picotls's own minicrypto backend, which is
# a separate library target (picotls-minicrypto) in picotls's own
# CMakeLists.txt, not part of core picotls.c. Mirroring that file list
# exactly here rather than guessing which subset is needed.
set(PICOTLS_MINICRYPTO_SOURCES
    ${PICOTLS_ROOT}/deps/micro-ecc/uECC.c
    ${PICOTLS_ROOT}/deps/cifra/src/aes.c
    ${PICOTLS_ROOT}/deps/cifra/src/blockwise.c
    ${PICOTLS_ROOT}/deps/cifra/src/chacha20.c
    ${PICOTLS_ROOT}/deps/cifra/src/chash.c
    ${PICOTLS_ROOT}/deps/cifra/src/curve25519.c
    ${PICOTLS_ROOT}/deps/cifra/src/drbg.c
    ${PICOTLS_ROOT}/deps/cifra/src/hmac.c
    ${PICOTLS_ROOT}/deps/cifra/src/gcm.c
    ${PICOTLS_ROOT}/deps/cifra/src/gf128.c
    ${PICOTLS_ROOT}/deps/cifra/src/modes.c
    ${PICOTLS_ROOT}/deps/cifra/src/poly1305.c
    ${PICOTLS_ROOT}/deps/cifra/src/sha256.c
    ${PICOTLS_ROOT}/deps/cifra/src/sha512.c
    ${PICOTLS_ROOT}/lib/cifra.c
    ${PICOTLS_ROOT}/lib/cifra/x25519.c
    ${PICOTLS_ROOT}/lib/cifra/chacha20.c
    ${PICOTLS_ROOT}/lib/cifra/aes128.c
    ${PICOTLS_ROOT}/lib/cifra/aes256.c
    ${PICOTLS_ROOT}/lib/cifra/random.c
    ${PICOTLS_ROOT}/lib/minicrypto-pem.c
    ${PICOTLS_ROOT}/lib/uecc.c
    ${PICOTLS_ROOT}/lib/ffx.c
)

add_library(picoquic_vendored STATIC
    ${PICOQUIC_CORE_SOURCES}
    ${PICOHTTP_SOURCES}
    ${PICOTLS_SOURCES}
    ${PICOTLS_MINICRYPTO_SOURCES}
)

# BEFORE matters here: h2o's own CMakeLists.txt installs its bundled,
# different-version deps/picotls/include/*.h into /opt/h2o/include (the
# main CMakeLists.txt's global include_directories(... ${H2O_INCLUDE})
# picks that up for every target, including this one). Without BEFORE,
# the compiler found h2o's older picotls.h ahead of this vendored one
# when compiling thirdparty/picotls/lib/picotls.c against it --
# "conflicting types for ptls_build_v4_mapped_v6_address", "unknown
# type name ptls_log_getsni_t" -- confirmed by reading h2o's own
# CMakeLists.txt install rules directly, not guessed. This ensures our
# own picotls headers are found first for this target regardless of
# global include-directory ordering.
#
# OPENSSL_INCLUDE here is the same variable the top-level CMakeLists.txt
# already found via find_path(OPENSSL_INCLUDE openssl/ssl.h REQUIRED) for
# h2o's own OpenSSL requirement -- this file is include()d into that
# scope, not add_subdirectory()d, so the variable is already visible.
target_include_directories(picoquic_vendored BEFORE PUBLIC
    ${PICOTLS_ROOT}/include
    ${PICOQUIC_ROOT}/picoquic
    ${PICOQUIC_ROOT}/picohttp
    ${OPENSSL_INCLUDE}
)

# picotls-minicrypto's own include path, per picotls's CMakeLists.txt's
# INCLUDE_DIRECTORIES() call -- cifra's headers use "ext/..." and
# "bitops.h"-style relative includes that only resolve with these two
# directories on the path, and deps/micro-ecc/uECC.h needs its own dir too.
target_include_directories(picoquic_vendored PRIVATE
    ${PICOTLS_ROOT}/deps/cifra/src/ext
    ${PICOTLS_ROOT}/deps/cifra/src
    ${PICOTLS_ROOT}/deps/micro-ecc
)

target_compile_definitions(picoquic_vendored PUBLIC
    PTLS_WITHOUT_FUSION
    DISABLE_DEBUG_PRINTF
)

# picotls's own CMakeLists.txt links picotls-openssl against
# OPENSSL_CRYPTO_LIBRARIES (libcrypto only -- picotls implements the TLS
# state machine itself, it only needs OpenSSL's crypto primitives, not
# libssl) plus CMAKE_DL_LIBS. CRYPTO_LIB here is the same variable the
# top-level CMakeLists.txt already found via find_library(CRYPTO_LIB
# crypto REQUIRED) for h2o's own requirement.
#
# target_link_libraries, not a plain list appended at the top-level scope:
# picoquic_vendored is a STATIC archive, and static archives only pull
# symbols from libraries positioned *after* them on the final link
# command line. Using target_link_libraries lets CMake's dependency graph
# place CRYPTO_LIB correctly relative to picoquic_vendored regardless of
# the order libraries are listed when linking the main executable --
# this is the same category of bug the earlier missing mbedtls
# link-directory fix addressed, applied proactively here rather than
# waiting to hit it again.
target_link_libraries(picoquic_vendored PUBLIC ${CRYPTO_LIB} ${CMAKE_DL_LIBS})

# NOTE: the three godot_patches/*.patch files (thirdparty/picoquic-godot-patches/)
# are mostly Windows/MinGW path-separator fixes, plus mbedtls-specific
# glue this build no longer compiles in (see the top-of-file note on the
# OpenSSL switch). Not yet confirmed which, if any, still apply to the
# OpenSSL backend on Linux; tracked as a follow-up, not silently assumed
# irrelevant.
