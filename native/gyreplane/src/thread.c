#include <h2o.h>
#include <stdlib.h>
#include <string.h>

#include "fdb_database.h"
#include "global_data.h"
#include "thread.h"

static fdb_global_t fdb_global;

void cleanup_thread_data(thread_context_t *ctx)
{
    fdb_thread_cleanup(&ctx->request_handler_data.fdb_state);
    h2o_context_dispose(&ctx->h2o_ctx);
}

void initialize_thread_data(global_thread_data_t *global_data, thread_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->global_thread_data = global_data;
    ctx->random_seed = (unsigned int)(uintptr_t)ctx;
    h2o_context_init(&ctx->h2o_ctx, h2o_evloop_create(), &global_data->config->h2o_config);
    ctx->loop = ctx->h2o_ctx.loop;

    fdb_thread_init(&fdb_global, ctx->loop, &ctx->request_handler_data.fdb_state);
}

void *event_loop_thread(void *arg)
{
    thread_context_t *ctx = arg;
    while (h2o_evloop_run(ctx->loop, INT32_MAX) == 0)
        ;
    return NULL;
}

int init_fdb_global(config_t *config, size_t num_threads)
{
    return fdb_global_init(&fdb_global, config->fdb_cluster_file, num_threads);
}

void cleanup_fdb_global(void)
{
    fdb_global_cleanup(&fdb_global);
}
