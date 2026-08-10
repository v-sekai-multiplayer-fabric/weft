// The limits, for a native plane or edge.
//
// Every value here is `Weft.Limits`, transcribed. That module holds every number weft
// promises or measures, and it says where each one comes from. Every one of them is
// rivet's, at https://rivet.dev/docs/actors/limits/.
//
// A number that appears in a plane and not in `Weft.Limits` is a guess about a workload
// weft has not seen. So a plane includes this header rather than choosing for itself.
//
// A C++ plane cannot call Elixir, so these are copies. `Weft.LimitsTest` reads this file
// and compares each one, so a change on one side fails on the other. A transcription that
// nothing checks goes stale, and the stale copy still reads as authoritative.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_LIMITS_HPP
#define WEFT_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace weft::limits {

// Items in one batch operation. rivet's max keys per operation. A bus message is the same
// shape of thing as a batch put: many items, one operation.
//
// `../logbook/data_plane.md` checks it against the batch sweep. The bus fails below 7, a
// message stops being mostly overhead at 336, and 128 carries 214.68 M snapshots each
// second on one core.
inline constexpr int SNAPSHOT_BATCH = 128;

// One action, in milliseconds. A loop that waits longer than this has lost the far end,
// whatever it is waiting for.
inline constexpr int ACTION_MS = 60000;

// One key and one value, in bytes. A plane that writes to the store obeys the same two
// limits the control plane does.
inline constexpr std::size_t KEY_BYTES = 2 * 1024;
inline constexpr std::size_t VALUE_BYTES = 128 * 1024;

// Messages a queue holds before it refuses a new one. rivet's max queue size. A plane
// that buffers work obeys the same bound the control plane does.
inline constexpr int QUEUE_MESSAGES = 1000;

// Requests in flight for each caller. `../logbook/store_plane.md` sweeps 1 to 512, and the
// latency stays flat to about 64. 32 sits inside the flat part.
inline constexpr int IN_FLIGHT = 32;

} // namespace weft::limits

#endif
