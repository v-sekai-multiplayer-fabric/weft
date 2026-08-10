/* The iceoryx2 C ABI types the harness names, declared here rather than included.
 *
 * The generated dispatch table must compile with no iceoryx2 headers present. So every
 * type in ../iceoryx2.sigs is declared below, transcribed from iceoryx2.h v0.9.3.
 *
 * Two kinds of type, and the difference is the whole safety argument.
 *
 * A handle, `iox2_..._h`, is a pointer to an opaque struct. Its size never matters here,
 * so an incomplete struct is exact.
 *
 * A storage struct, `iox2_..._t`, is a real sized struct that a caller may allocate to
 * keep an object off the heap. Its size does matter, and transcribing it would be a
 * silent memory bug the day upstream adds a field. So it stays incomplete, and the
 * harness passes NULL for every one. The C API allocates on the heap when it gets NULL.
 * That is the documented contract, and it costs one allocation for each object.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WEFT_HARNESS_IOX2_DECLS_H
#define WEFT_HARNESS_IOX2_DECLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Storage structs. Incomplete on purpose. Pass NULL. */
struct iox2_node_builder_t;
struct iox2_node_t;
struct iox2_service_name_t;
struct iox2_service_builder_t;
struct iox2_port_factory_pub_sub_t;
struct iox2_port_factory_publisher_builder_t;
struct iox2_port_factory_subscriber_builder_t;
struct iox2_publisher_t;
struct iox2_subscriber_t;
struct iox2_sample_t;
struct iox2_sample_mut_t;

/* Handles. Opaque pointers, and a _ref is a const pointer to one. */
struct iox2_name_h_t;
typedef struct iox2_name_h_t* iox2_node_h;
typedef const iox2_node_h* iox2_node_h_ref;

struct iox2_node_builder_h_t;
typedef struct iox2_node_builder_h_t* iox2_node_builder_h;

struct iox2_service_name_h_t;
typedef struct iox2_service_name_h_t* iox2_service_name_h;

struct iox2_service_name_ptr_t;
typedef const struct iox2_service_name_ptr_t* iox2_service_name_ptr;

struct iox2_service_builder_h_t;
typedef struct iox2_service_builder_h_t* iox2_service_builder_h;

struct iox2_service_builder_pub_sub_h_t;
typedef struct iox2_service_builder_pub_sub_h_t* iox2_service_builder_pub_sub_h;
typedef const iox2_service_builder_pub_sub_h* iox2_service_builder_pub_sub_h_ref;

struct iox2_port_factory_pub_sub_h_t;
typedef struct iox2_port_factory_pub_sub_h_t* iox2_port_factory_pub_sub_h;
typedef const iox2_port_factory_pub_sub_h* iox2_port_factory_pub_sub_h_ref;

struct iox2_port_factory_publisher_builder_h_t;
typedef struct iox2_port_factory_publisher_builder_h_t* iox2_port_factory_publisher_builder_h;
typedef const iox2_port_factory_publisher_builder_h* iox2_port_factory_publisher_builder_h_ref;

struct iox2_port_factory_subscriber_builder_h_t;
typedef struct iox2_port_factory_subscriber_builder_h_t* iox2_port_factory_subscriber_builder_h;

struct iox2_publisher_h_t;
typedef struct iox2_publisher_h_t* iox2_publisher_h;
typedef const iox2_publisher_h* iox2_publisher_h_ref;

struct iox2_subscriber_h_t;
typedef struct iox2_subscriber_h_t* iox2_subscriber_h;
typedef const iox2_subscriber_h* iox2_subscriber_h_ref;

struct iox2_sample_h_t;
typedef struct iox2_sample_h_t* iox2_sample_h;
typedef const iox2_sample_h* iox2_sample_h_ref;

struct iox2_sample_mut_h_t;
typedef struct iox2_sample_mut_h_t* iox2_sample_mut_h;
typedef const iox2_sample_mut_h* iox2_sample_mut_h_ref;

/* Enums. A value here is part of the ABI, so each one is transcribed exactly. */
typedef enum iox2_service_type_e {
    iox2_service_type_e_LOCAL,
    iox2_service_type_e_IPC,
} iox2_service_type_e;

typedef enum iox2_type_variant_e {
    iox2_type_variant_e_FIXED_SIZE,
    iox2_type_variant_e_DYNAMIC,
} iox2_type_variant_e;

typedef enum iox2_log_level_e {
    iox2_log_level_e_TRACE = 0,
    iox2_log_level_e_DEBUG = 1,
    iox2_log_level_e_INFO = 2,
    iox2_log_level_e_WARN = 3,
    iox2_log_level_e_ERROR = 4,
    iox2_log_level_e_FATAL = 5,
} iox2_log_level_e;

/* Success. Every int-returning call below returns this or an error code. */
#define IOX2_OK 0

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEFT_HARNESS_IOX2_DECLS_H */
