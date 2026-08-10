#!/usr/bin/env bash
# Write compile_commands.json for the store plane, so clangd uses the flags the build
# uses instead of guessing. Run it from the repository root.
#
#   native/storeplane/compiledb.sh
set -euo pipefail

docker compose -f deploy/compose.yaml run --rm --build --no-deps --entrypoint sh weft -c '
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y -qq libsqlite3-dev cmake >/dev/null 2>&1
  cmake -S /app/native/storeplane -B /tmp/b -DCMAKE_BUILD_TYPE=Release >/dev/null
  cat /tmp/b/compile_commands.json
' | sed -n '/^\[/,$p' > compile_commands.json

echo "wrote compile_commands.json ($(wc -l < compile_commands.json) lines)"
