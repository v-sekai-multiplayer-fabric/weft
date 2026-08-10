// The message a plane publishes to another plane.
//
// One entity snapshot, in the fixed point the data plane already uses. This is the
// smallest payload that proves the bus, and it is the shape the ring carries.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_SNAPSHOT_HPP
#define WEFT_SNAPSHOT_HPP

#include "weft/limits.hpp"

#include <cstdint>

namespace weft {

struct Snapshot {
    std::uint64_t tick;
    std::uint64_t entity;
    std::int64_t x_um;
    std::int64_t y_um;
    std::int64_t z_um;
};

// The service both ends open. One name for one concept.
inline constexpr const char* SERVICE_NAME = "weft/harness/snapshot";

// The payload type name iceoryx2 records with the service. Both ends must give the same
// name, size, and alignment, and iceoryx2 rejects the second port when they differ. That
// check is what stops one process reading another process's layout as its own, so this
// string is part of the contract and not a label.
inline constexpr const char* PAYLOAD_TYPE = "weft::Snapshot";

} // namespace weft

#endif
