#!/bin/sh
# Runs the already-built-and-tested zone plane binary.
#
# The plane linked libh2o-evloop for an event loop that drove nothing, and it links none
# now, so /opt/h2o is gone from this path. What is left on LD_LIBRARY_PATH is the build
# tree itself. The system libraries the binary still needs, libssl, libcrypto, libz, and
# libfdb_c, are already on the default loader path from their own packages.
set -eu

exec env LD_LIBRARY_PATH="/work/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  /work/build/zone-server-h2o "$@"
