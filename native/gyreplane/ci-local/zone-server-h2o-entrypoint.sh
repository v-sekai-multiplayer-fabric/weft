#!/bin/sh
# Runs the already-built-and-tested zone-server-h2o binary. Its shared
# library dependencies are not on the default linker search path:
# libh2o-evloop.so was installed to /opt/h2o/lib (this image's own
# cmake --install prefix, see ci-local/Containerfile), and libmujoco.so
# was built to build/lib (relative to /work, matching the same
# CMAKE_LIBRARY_OUTPUT_DIRECTORY reasoning recorded in
# .github/workflows/real-build.yml's own test step). System libs
# (libssl, libcrypto, libyajl, libz, libbrotli, libfdb_c) are already
# on the default path via apt/dpkg.
exec env LD_LIBRARY_PATH="/opt/h2o/lib:/work/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  /work/build/zone-server-h2o "$@"
