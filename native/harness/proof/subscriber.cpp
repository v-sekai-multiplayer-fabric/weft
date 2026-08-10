// The other half of the proof. It receives Snapshots and checks each one.
//
// A message that arrives is not proof on its own. This end checks the tick order and the
// payload it derives from the tick, so a bus that delivers garbage fails here rather
// than printing a count.
//
// Exit 0 means every expected message arrived, in order, intact.
//
// SPDX-License-Identifier: Apache-2.0
#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/snapshot.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    const int expect = (argc > 1) ? std::atoi(argv[1]) : 8;

    if (!weft::load_bus()) {
        return 1;
    }
    iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

    iox2_node_h node = nullptr;
    if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
                                 iox2_service_type_e_IPC, &node)
        != IOX2_OK) {
        std::fprintf(stderr, "subscriber: no node\n");
        return 1;
    }

    iox2_service_name_h name = nullptr;
    if (iox2_service_name_new(nullptr, weft::SERVICE_NAME, std::strlen(weft::SERVICE_NAME),
                              &name)
        != IOX2_OK) {
        std::fprintf(stderr, "subscriber: no service name\n");
        return 1;
    }

    auto builder = iox2_service_builder_pub_sub(
        iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(name)));

    if (iox2_service_builder_pub_sub_set_payload_type_details(
            &builder, iox2_type_variant_e_FIXED_SIZE, weft::PAYLOAD_TYPE,
            std::strlen(weft::PAYLOAD_TYPE), sizeof(weft::Snapshot), alignof(weft::Snapshot))
        != IOX2_OK) {
        std::fprintf(stderr, "subscriber: type details rejected\n");
        return 1;
    }

    iox2_port_factory_pub_sub_h service = nullptr;
    if (iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service) != IOX2_OK) {
        std::fprintf(stderr, "subscriber: no service\n");
        return 1;
    }

    iox2_subscriber_h subscriber = nullptr;
    if (iox2_port_factory_subscriber_builder_create(
            iox2_port_factory_pub_sub_subscriber_builder(&service, nullptr), nullptr,
            &subscriber)
        != IOX2_OK) {
        std::fprintf(stderr, "subscriber: no subscriber\n");
        return 1;
    }

    int seen = 0;
    int idle = 0;

    // Give up after the action limit, and not after a number chosen here. `Weft.Limits`
    // holds 60 s for one action, and this loop polls at 10 ms, so the count is that
    // limit divided by that period. A publisher that is alive answers in 20 ms, which is
    // three orders of magnitude sooner.
    const int give_up_after = weft::limits::ACTION_MS / 10;

    while (seen < expect && idle < give_up_after) {
        iox2_sample_h sample = nullptr;
        if (iox2_subscriber_receive(&subscriber, nullptr, &sample) != IOX2_OK) {
            std::fprintf(stderr, "subscriber: receive failed\n");
            return 1;
        }

        if (sample == nullptr) {
            ++idle;
            (void)iox2_node_wait(&node, 0, 10 * 1000 * 1000);
            continue;
        }

        idle = 0;
        const void* payload = nullptr;
        size_t elements = 0;
        iox2_sample_payload(&sample, &payload, &elements);

        weft::Snapshot got {};
        std::memcpy(&got, payload, sizeof(got));
        iox2_sample_drop(sample);
        ++seen;

        if (got.tick != static_cast<std::uint64_t>(seen)) {
            std::fprintf(stderr, "subscriber: out of order, want %d got %llu\n", seen,
                         static_cast<unsigned long long>(got.tick));
            return 1;
        }
        if (got.entity != 42 || got.x_um != static_cast<std::int64_t>(seen) * 1000
            || got.y_um != 0 || got.z_um != static_cast<std::int64_t>(seen) * -250) {
            std::fprintf(stderr, "subscriber: payload wrong at tick %d\n", seen);
            return 1;
        }

        std::printf("subscriber: tick %llu entity %llu x %lld z %lld\n",
                    static_cast<unsigned long long>(got.tick),
                    static_cast<unsigned long long>(got.entity),
                    static_cast<long long>(got.x_um), static_cast<long long>(got.z_um));
        std::fflush(stdout);
    }

    iox2_subscriber_drop(subscriber);
    iox2_port_factory_pub_sub_drop(service);
    iox2_service_name_drop(name);
    iox2_node_drop(node);

    if (seen != expect) {
        std::fprintf(stderr, "subscriber: wanted %d, got %d\n", expect, seen);
        return 1;
    }

    std::printf("subscriber: received %d, in order, intact\n", seen);
    return 0;
}
