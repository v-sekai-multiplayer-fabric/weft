/* What the generated dispatch table needs that weft does not otherwise provide.
 *
 * generate_stubs.py is Chromium's, so its output expects Chromium's base library.
 * `--macro-include` points it at this file instead. Three things have to be here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The iceoryx2 types every generated declaration names. */
#include "iox2_decls.h" /* IWYU pragma: keep */

/* generate_stubs.py emits this on every dispatched call, to opt out of Chromium's
 * Control Flow Integrity check on an indirect call. weft does not build with CFI on that
 * path, so it stays empty. */
#define DISABLE_CFI_ICALL

/* The umbrella initializer needs a reachable namespace name. `-p native/harness` makes
 * the generator write `native_harness`. The guard keeps this header valid in C, because
 * an editor indexes it on its own. */
#ifdef __cplusplus
namespace native_harness {}
#endif
