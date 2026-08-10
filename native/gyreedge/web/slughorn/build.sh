#!/usr/bin/env bash
# Builds binding.cpp + SlugHorn's core (thirdparty/slughorn) into
# mud/web/slughorn.js + mud/web/slughorn.wasm -- the artifacts
# mud/web/mud3d.js loads at runtime.
#
# Requires an activated Emscripten SDK in the current shell
# (`source emsdk_env.sh`), or set EMCC to a full path to em++.
# Verified with Emscripten 6.0.6.
#
# The built .js/.wasm are committed under mud/web/ on purpose: h2o
# serves mud/web/ straight off MUD_WEB_DOCROOT (fly/entrypoint.sh),
# and fly/Containerfile carries no Emscripten toolchain -- adding a
# ~300MB emsdk download to that image just to regenerate two small
# files would cost far more than it buys.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SLUGHORN_ROOT="$HERE/../../../thirdparty/slughorn"
OUT_DIR="$HERE/.."

EMCC="${EMCC:-em++}"

"$EMCC" \
	-std=c++20 -O2 \
	-I "$SLUGHORN_ROOT" \
	"$HERE/binding.cpp" \
	"$SLUGHORN_ROOT/slughorn/slughorn.cpp" \
	-sMODULARIZE=1 \
	-sEXPORT_ES6=1 \
	-sEXPORT_NAME=SlugHornModule \
	-sENVIRONMENT=web,node \
	-sALLOW_MEMORY_GROWTH=1 \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8 \
	-sEXPORTED_FUNCTIONS=_slughorn_buildBatchAtlas,_slughorn_shapeCount,_slughorn_curveTexPtr,_slughorn_curveTexLen,_slughorn_curveTexWidth,_slughorn_curveTexHeight,_slughorn_bandTexPtr,_slughorn_bandTexLen,_slughorn_bandTexWidth,_slughorn_bandTexHeight,_slughorn_atlasTexWidthLog2,_slughorn_shapeField \
	-o "$OUT_DIR/slughorn.js"

echo "Built $OUT_DIR/slughorn.js + slughorn.wasm"
