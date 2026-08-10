// The message a plane publishes to another plane.
//
// One entity snapshot, in the fixed point the data plane already uses. This is the
// smallest payload that proves the bus, and it is the shape the ring carries.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_HARNESS_SNAPSHOT_HPP
#define WEFT_HARNESS_SNAPSHOT_HPP

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

// The limits below are `Weft.Limits`, transcribed. That module holds every number weft
// promises or measures, and it says where each one comes from. A number that appears
// here and not there is a guess about a workload, which weft does not keep.
//
// A C++ plane cannot call Elixir, so these are copies. `Weft.LimitsTest` asserts the
// same values, so a change on one side fails on the other.

// Entities in one bus message. This is rivet's max keys per operation, which weft already
// copies along with the rest of its limits. A message is the same shape of thing as a
// batch put: many items, one operation. data_plane_logbook.md checks it against the batch
// sweep: the bus fails below 7, a message stops being mostly overhead at 336, and 128
// carries 214.68 M snapshots each second on one core.
inline constexpr int SNAPSHOT_BATCH = 128;

// The limit on one action, in milliseconds. A loop that waits longer than this has lost
// the far end, whatever it is waiting for.
inline constexpr int ACTION_MS = 60000;

} // namespace weft

#endif
