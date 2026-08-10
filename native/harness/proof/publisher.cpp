// Proof that a message crosses the bus with no copy, and with nothing linked.
//
// This publisher writes a Snapshot into shared memory and sends the loan. It does not
// serialize, and it does not copy the payload. `loan_slice_uninit` returns memory the
// subscriber already maps, so a send is a pointer handoff.
//
// Every iox2_ call below goes through the dlsym table in ../gen. Nothing here links
// iceoryx2, and no iceoryx2 header is on the include path. See ../iceoryx2.sigs.
//
// This is not the harness. It is the one thing that has to work before a harness is
// worth writing. See ../README.md.
//
// SPDX-License-Identifier: Apache-2.0
#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/snapshot.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    const int count = (argc > 1) ? std::atoi(argv[1]) : 8;

    if (!weft::load_bus()) {
        return 1;
    }
    iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

    iox2_node_h node = nullptr;
    if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
                                 iox2_service_type_e_IPC, &node)
        != IOX2_OK) {
        std::fprintf(stderr, "publisher: no node\n");
        return 1;
    }

    iox2_service_name_h name = nullptr;
    if (iox2_service_name_new(nullptr, weft::SERVICE_NAME, std::strlen(weft::SERVICE_NAME),
                              &name)
        != IOX2_OK) {
        std::fprintf(stderr, "publisher: no service name\n");
        return 1;
    }

    auto builder = iox2_service_builder_pub_sub(
        iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(name)));

    // The type name and the size must match on both ends. iceoryx2 rejects a mismatch
    // when the second port opens, which is what stops one process reading another's
    // layout as its own.
    if (iox2_service_builder_pub_sub_set_payload_type_details(
            &builder, iox2_type_variant_e_FIXED_SIZE, weft::PAYLOAD_TYPE,
            std::strlen(weft::PAYLOAD_TYPE), sizeof(weft::Snapshot), alignof(weft::Snapshot))
        != IOX2_OK) {
        std::fprintf(stderr, "publisher: type details rejected\n");
        return 1;
    }

    iox2_port_factory_pub_sub_h service = nullptr;
    if (iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service) != IOX2_OK) {
        std::fprintf(stderr, "publisher: no service\n");
        return 1;
    }

    iox2_publisher_h publisher = nullptr;
    if (iox2_port_factory_publisher_builder_create(
            iox2_port_factory_pub_sub_publisher_builder(&service, nullptr), nullptr, &publisher)
        != IOX2_OK) {
        std::fprintf(stderr, "publisher: no publisher\n");
        return 1;
    }

    for (int tick = 1; tick <= count; ++tick) {
        iox2_sample_mut_h sample = nullptr;
        if (iox2_publisher_loan_slice_uninit(&publisher, nullptr, &sample, 1) != IOX2_OK) {
            std::fprintf(stderr, "publisher: no loan at tick %d\n", tick);
            return 1;
        }

        void* payload = nullptr;
        size_t elements = 0;
        iox2_sample_mut_payload_mut(&sample, &payload, &elements);

        const weft::Snapshot snapshot {
            static_cast<std::uint64_t>(tick),
            42,
            static_cast<std::int64_t>(tick) * 1000,
            0,
            static_cast<std::int64_t>(tick) * -250,
        };
        std::memcpy(payload, &snapshot, sizeof(snapshot));

        if (iox2_sample_mut_send(sample, nullptr) != IOX2_OK) {
            std::fprintf(stderr, "publisher: send failed at tick %d\n", tick);
            return 1;
        }

        // 20 ms, which is slow enough that the other end is never starved and fast
        // enough that the run finishes. It is not a rate, and nothing here measures one.
        (void)iox2_node_wait(&node, 0, 20 * 1000 * 1000);
    }

    std::printf("publisher: sent %d\n", count);

    iox2_publisher_drop(publisher);
    iox2_port_factory_pub_sub_drop(service);
    iox2_service_name_drop(name);
    iox2_node_drop(node);
    return 0;
}
