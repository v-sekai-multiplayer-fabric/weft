#ifndef THREAD_H_
#define THREAD_H_

#include <h2o.h>
#include <pthread.h>

#include "global_data.h"

int init_fdb_global(config_t *config, size_t num_threads);
void cleanup_fdb_global(void);

void cleanup_thread_data(thread_context_t *ctx);
void initialize_thread_data(global_thread_data_t *global_data, thread_context_t *ctx);
void *event_loop_thread(void *arg);

#endif
