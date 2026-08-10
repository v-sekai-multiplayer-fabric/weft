#!/usr/bin/env bash
# Write compile_commands.json for the store plane, so clangd uses the flags the build
# uses instead of guessing. Run it from the repository root.
#
#   native/storeplane/compiledb.sh
#
# It configures with CMake on the host first, because a host build gives clangd paths
# that exist on the host. A container build gives container paths, and clangd then
# reports every system header as missing even though the file is correct.
#
# The host needs the FoundationDB and SQLite headers. It falls back to the container when
# CMake cannot find them, which still fixes the flags even though the paths are the
# container's.
set -euo pipefail

out=compile_commands.json
build=${BUILD_DIR:-.cmake-host}

# Homebrew and pkg-config are where a host most often keeps SQLite. Neither is required.
prefix="${CMAKE_PREFIX_PATH:-}"
for p in /home/linuxbrew/.linuxbrew /usr/local /opt/homebrew; do
	[ -d "$p/include" ] && prefix="$prefix${prefix:+;}$p"
done

if cmake -S native/storeplane -B "$build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="$prefix" >/dev/null 2>&1; then
	cp "$build/compile_commands.json" "$out"
	echo "wrote $out from the host build ($(wc -l < "$out") lines)"
	exit 0
fi

echo "the host has no FoundationDB or SQLite headers, using the container" >&2

docker compose -f deploy/compose.yaml run --rm -T --build --no-deps --entrypoint sh weft -c '
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y -qq libsqlite3-dev cmake >/dev/null 2>&1
  cmake -S /app/native/storeplane -B /tmp/b -DCMAKE_BUILD_TYPE=Release >/dev/null
  cat /tmp/b/compile_commands.json
' | sed -n '/^\[/,$p' > "$out"

echo "wrote $out from the container ($(wc -l < "$out") lines)"
