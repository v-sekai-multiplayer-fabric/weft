// What the dlsym dispatch table costs, measured rather than argued.
//
// The same loop builds two ways. WEFT_ICEORYX2_DIRECT links libiceoryx2_ffi_c and
// includes its real header. The default goes through the generated table in ../gen, so
// every iox2_ call is one extra indirect call.
//
// The loop is loan, write, send, in one process with one subscriber attached. It reports
// nanoseconds for each message. Run both builds on the same machine, back to back, and
// put the pair in ../../../docs/logbook/data_plane.md with its conditions.
//
// SPDX-License-Identifier: Apache-2.0
#ifdef WEFT_ICEORYX2_DIRECT
#include "iox2/iceoryx2.h"
#else
#include "iox2_api.h"
#include "weft/bus.hpp"
#endif

#include "weft/snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// A run reports the median of several passes, not one pass. One pass picks up whatever
// the scheduler did during it.
double median(std::vector<double>& xs) {
    std::sort(xs.begin(), xs.end());
    return xs[xs.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    const int messages = (argc > 1) ? std::atoi(argv[1]) : 200000;
    const int passes = (argc > 2) ? std::atoi(argv[2]) : 7;

    // Entities in one message. iceoryx2 loans a slice, so a batch is one loan and one
    // send whatever its length. This is the knob that decides whether the bus can carry
    // the 15 M snapshots each second target at all.
    const int batch = (argc > 3) ? std::atoi(argv[3]) : 1;

#ifndef WEFT_ICEORYX2_DIRECT
    if (!weft::load_bus()) {
        return 1;
    }
#endif
    iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

    iox2_node_h node = nullptr;
    if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
                                 iox2_service_type_e_IPC, &node)
        != IOX2_OK) {
        std::fprintf(stderr, "bench: no node\n");
        return 1;
    }

    // A distinct service for each batch size. iceoryx2 records the payload layout with
    // the service and keeps it, so reusing one name across batch sizes reopens the layout
    // the first run wrote and every loan then fails.
    char service_name[64];
    std::snprintf(service_name, sizeof(service_name), "weft/harness/bench/%d", batch);
    iox2_service_name_h name = nullptr;
    if (iox2_service_name_new(nullptr, service_name, std::strlen(service_name), &name)
        != IOX2_OK) {
        std::fprintf(stderr, "bench: no service name\n");
        return 1;
    }

    auto builder = iox2_service_builder_pub_sub(
        iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(name)));
    if (iox2_service_builder_pub_sub_set_payload_type_details(
            &builder,
            batch > 1 ? iox2_type_variant_e_DYNAMIC : iox2_type_variant_e_FIXED_SIZE,
            weft::PAYLOAD_TYPE,
            std::strlen(weft::PAYLOAD_TYPE), sizeof(weft::Snapshot), alignof(weft::Snapshot))
        != IOX2_OK) {
        std::fprintf(stderr, "bench: type details rejected\n");
        return 1;
    }

    iox2_port_factory_pub_sub_h service = nullptr;
    if (iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service) != IOX2_OK) {
        std::fprintf(stderr, "bench: no service\n");
        return 1;
    }

    auto publisher_builder = iox2_port_factory_pub_sub_publisher_builder(&service, nullptr);
    iox2_port_factory_publisher_builder_set_initial_max_slice_len(
        &publisher_builder, static_cast<size_t>(batch));

    iox2_publisher_h publisher = nullptr;
    if (iox2_port_factory_publisher_builder_create(publisher_builder, nullptr, &publisher)
        != IOX2_OK) {
        std::fprintf(stderr, "bench: no publisher\n");
        return 1;
    }

    // A subscriber in the same process. Without one the publisher has no receiver and the
    // send path is not the send path.
    iox2_subscriber_h subscriber = nullptr;
    if (iox2_port_factory_subscriber_builder_create(
            iox2_port_factory_pub_sub_subscriber_builder(&service, nullptr), nullptr,
            &subscriber)
        != IOX2_OK) {
        std::fprintf(stderr, "bench: no subscriber\n");
        return 1;
    }

    std::vector<double> results;
    results.reserve(static_cast<size_t>(passes));

    for (int pass = 0; pass < passes; ++pass) {
        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < messages; ++i) {
            iox2_sample_mut_h sample = nullptr;
            if (iox2_publisher_loan_slice_uninit(&publisher, nullptr, &sample,
                                                 static_cast<size_t>(batch))
                != IOX2_OK) {
                std::fprintf(stderr, "bench: loan failed at %d\n", i);
                return 1;
            }

            void* payload = nullptr;
            size_t elements = 0;
            iox2_sample_mut_payload_mut(&sample, &payload, &elements);

            auto* out = static_cast<weft::Snapshot*>(payload);
            for (int e = 0; e < batch; ++e) {
                out[e] = weft::Snapshot { static_cast<std::uint64_t>(i),
                                          static_cast<std::uint64_t>(e), i * 1000, 0,
                                          -i * 250 };
            }

            if (iox2_sample_mut_send(sample, nullptr) != IOX2_OK) {
                std::fprintf(stderr, "bench: send failed at %d\n", i);
                return 1;
            }

            // Drain, so the queue never fills and the loan never blocks. This is part of
            // the measured loop on purpose: a send with nobody draining is not a send.
            iox2_sample_h got = nullptr;
            if (iox2_subscriber_receive(&subscriber, nullptr, &got) == IOX2_OK
                && got != nullptr) {
                iox2_sample_drop(got);
            }
        }

        const auto end = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(ns / messages);
    }

    const double best = *std::min_element(results.begin(), results.end());
    const double mid = median(results);

#ifdef WEFT_ICEORYX2_DIRECT
    const char* build = "direct";
#else
    const char* build = "stubs";
#endif
    // A snapshot rate, because that is the target. 15 M snapshots each second on one
    // core is what a plane has to carry, and a message carries `batch` of them.
    const double snapshots_per_s = (1e9 / mid) * batch;
    std::printf("%s: batch %4d, %.1f ns for each message, %.2f M snapshots/s\n", build,
                batch, mid, snapshots_per_s / 1e6);

    iox2_subscriber_drop(subscriber);
    iox2_publisher_drop(publisher);
    iox2_port_factory_pub_sub_drop(service);
    iox2_service_name_drop(name);
    iox2_node_drop(node);
    return 0;
}
