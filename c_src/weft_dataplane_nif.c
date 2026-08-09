// Native data-plane worker NIF: an OS thread writes snapshots into a native-owned
// lock-free ring (seqlock); the BEAM samples the latest. This is the real producer
// the Elixir stub stands in for. No exceptions: returns tagged tuples / badarg.
#include <erl_nif.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define MAX_E 64

typedef struct {
  _Atomic int64_t gen;               // seqlock: odd = writing, even = stable
  int64_t tick;                      // guarded by gen
  int64_t coord[MAX_E * 3];          // guarded by gen
  int entities;
  _Atomic int running;
  pthread_t thread;
  int has_thread;
} worker_t;

static ErlNifResourceType *WORKER_RES = NULL;

static void *producer(void *arg) {
  worker_t *w = (worker_t *)arg;
  int n = w->entities * 3;
  int64_t tick = 0;
  while (atomic_load_explicit(&w->running, memory_order_acquire)) {
    int64_t g = atomic_load_explicit(&w->gen, memory_order_relaxed);
    atomic_store_explicit(&w->gen, g + 1, memory_order_release);
    tick++;
    w->tick = tick;
    for (int i = 0; i < n; i++) w->coord[i] = tick + i;
    atomic_store_explicit(&w->gen, g + 2, memory_order_release);
  }
  return NULL;
}

static void stop_worker(worker_t *w) {
  if (atomic_exchange(&w->running, 0) && w->has_thread) {
    pthread_join(w->thread, NULL);
    w->has_thread = 0;
  }
}

static ERL_NIF_TERM err(ErlNifEnv *env, const char *reason) {
  return enif_make_tuple2(env, enif_make_atom(env, "error"), enif_make_atom(env, reason));
}

static ERL_NIF_TERM start_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  int entities;
  if (!enif_get_int(env, argv[0], &entities) || entities < 1 || entities > MAX_E)
    return enif_make_badarg(env);
  worker_t *w = enif_alloc_resource(WORKER_RES, sizeof(worker_t));
  if (!w) return err(env, "alloc_failed");
  memset(w, 0, sizeof(*w));
  w->entities = entities;
  atomic_store(&w->running, 1);
  if (pthread_create(&w->thread, NULL, producer, w) != 0) {
    enif_release_resource(w);
    return err(env, "thread_failed");
  }
  w->has_thread = 1;
  ERL_NIF_TERM term = enif_make_resource(env, w);
  enif_release_resource(w);
  return enif_make_tuple2(env, enif_make_atom(env, "ok"), term);
}

static ERL_NIF_TERM sample_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  worker_t *w;
  if (!enif_get_resource(env, argv[0], WORKER_RES, (void **)&w))
    return enif_make_badarg(env);
  int n = w->entities * 3;
  int64_t tick = 0;
  int64_t coords[MAX_E * 3];
  for (;;) {
    int64_t g1 = atomic_load_explicit(&w->gen, memory_order_acquire);
    if (g1 & 1) continue;
    tick = w->tick;
    memcpy(coords, w->coord, (size_t)n * sizeof(int64_t));
    int64_t g2 = atomic_load_explicit(&w->gen, memory_order_acquire);
    if (g1 == g2) break;
  }
  ERL_NIF_TERM list = enif_make_list(env, 0);
  for (int i = n - 1; i >= 0; i--)
    list = enif_make_list_cell(env, enif_make_int64(env, coords[i]), list);
  return enif_make_tuple2(env, enif_make_int64(env, tick), list);
}

static ERL_NIF_TERM stop_nif(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  worker_t *w;
  if (!enif_get_resource(env, argv[0], WORKER_RES, (void **)&w))
    return enif_make_badarg(env);
  stop_worker(w);
  return enif_make_atom(env, "ok");
}

static void worker_dtor(ErlNifEnv *env, void *obj) { stop_worker((worker_t *)obj); }

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
  WORKER_RES = enif_open_resource_type(env, NULL, "weft_worker", worker_dtor,
                                       ERL_NIF_RT_CREATE, NULL);
  return WORKER_RES ? 0 : -1;
}

static ErlNifFunc funcs[] = {
    {"start", 1, start_nif, 0},
    {"sample", 1, sample_nif, 0},
    {"stop", 1, stop_nif, 0},
};

ERL_NIF_INIT(Elixir.Weft.DataPlane.Native, funcs, load, NULL, NULL, NULL)
