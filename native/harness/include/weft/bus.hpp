// Load libiceoryx2_ffi_c at start, or fail with a message a person can act on.
//
// Nothing links iceoryx2. The generated table in ../gen dlopens it, so a missing library
// is a start-up failure and not a link failure. That is the point: weft's build graph
// holds no Rust artifact, and a plane that cannot find the bus says so in one line.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_BUS_HPP
#define WEFT_BUS_HPP

#include "native/harness/iceoryx2_stubs.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace weft {

// The names to try, in order. A soname first, because that is what an installed package
// gives. The bare name second, so LD_LIBRARY_PATH and a local build prefix work.
inline bool load_bus() {
    std::vector<std::string> paths;

    if (const char* from_env = std::getenv("WEFT_ICEORYX2_PATH")) {
        paths.emplace_back(from_env);
    }
    paths.emplace_back("libiceoryx2_ffi_c.so");
    paths.emplace_back("libiceoryx2_ffi_c.so.0");

    native_harness::StubPathMap map;
    map[native_harness::kModuleIceoryx2] = paths;

    if (!native_harness::InitializeStubs(map)) {
        std::fprintf(stderr,
                     "weft: could not load libiceoryx2_ffi_c.\n"
                     "  Set WEFT_ICEORYX2_PATH to the file, or put its directory on\n"
                     "  LD_LIBRARY_PATH. See native/harness/README.md.\n");
        return false;
    }
    return true;
}

} // namespace weft

#endif
