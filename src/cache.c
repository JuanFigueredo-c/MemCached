#define _GNU_SOURCE // pthread_rwlock_t
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include "stats.h"
#include "cache.h"
#include "ll.h"

struct _Cache {
  List *buckets;
  LRUQueue queue;
  struct Stats text_stats, bin_stats;
  pthread_rwlock_t *row_locks;
  pthread_mutex_t ts_lock, bs_lock;
  uint32_t nregions, size;
};

LRUQueue cache_get_lru_queue(Cache cache) { return cache->queue; }

unsigned long hash_bytes(char *bytes, uint64_t nbytes) {
  unsigned long hashval, i;
  for (i = 0, hashval = 0; i < nbytes; ++i, bytes++)
    hashval = *bytes + 31 * hashval;
  return hashval;
}

/* --------------- CACHE ------------------- */
#define NROW(val, len) (hash_bytes(val, len) % cache->size)
#define NREGION(idx) (idx % cache->nregions)
#define RD_LOCK_ROW(idx) pthread_rwlock_rdlock(cache->row_locks + NREGION(idx))
#define RD_TRYLOCK_ROW(idx) pthread_rwlock_tryrdlock(cache->row_locks + NREGION(idx))
#define WR_LOCK_ROW(idx) pthread_rwlock_wrlock(cache->row_locks + NREGION(idx))
#define WR_TRYLOCK_ROW(idx) pthread_rwlock_trywrlock(cache->row_locks + NREGION(idx))
#define UNLOCK_ROW(idx) pthread_rwlock_unlock(cache->row_locks + NREGION(idx))

Cache cache_init(uint64_t size, uint64_t nregions) {
  Cache cache = malloc(sizeof(struct _Cache));
  assert(cache);
  cache->buckets    = malloc(list_size() * size);
  cache->row_locks  = malloc(sizeof(pthread_rwlock_t) * nregions);
  assert(cache->buckets && cache->row_locks);
  cache->queue      = lru_init();
  cache->text_stats = stats_init();
  cache->bin_stats  = stats_init();
  cache->nregions   = nregions;
  cache->size       = size;
  if (pthread_mutex_init(&cache->ts_lock, NULL) < 0)
    perror("Inicializado lock para estadisticas de texto");
  if (pthread_mutex_init(&cache->bs_lock, NULL) < 0)
    perror("Inicializado lock para estadisticas binarias");
  for (uint32_t i = 0; i < nregions; i++)
    if (pthread_rwlock_init(cache->row_locks + i, NULL) < 0)
      perror("Inicializado de lock para region de cache");
  for (uint32_t i = 0; i < size; i++)
    cache->buckets[i] = list_init();
  log(2, "Inicializado de cache con %lu casillas y %lu regiones", size, nregions);
  return cache;
}

void cache_destroy(Cache cache) {
  for (uint32_t i = 0; i < cache->nregions; i++)
    pthread_rwlock_destroy(cache->row_locks + i);
  pthread_mutex_destroy(&cache->ts_lock);
  pthread_mutex_destroy(&cache->bs_lock);
  lru_destroy(cache->queue);
  free(cache->row_locks);
  for (uint32_t i = 0; i < cache->size; i++)
    list_free(cache->buckets[i]);
  free(cache->buckets);
  free(cache);
}

enum code cache_get(Cache cache, char mode, char* key, unsigned klen, char **val, unsigned *vlen) {
  unsigned idx = NROW(key, klen);
  RD_LOCK_ROW(idx);
  List node = list_search(cache->buckets[idx], mode, key, klen);
  if (!node) {
    UNLOCK_ROW(idx);
    return ENOTFOUND;
  }
  Data data = list_get_data(node);
  *vlen = data.vlen;
  *val = malloc(*vlen);
  assert(*val);
  memcpy(*val, data.val, *vlen); // Copiamos para proteger la informacion
  reset_lru_status(cache_get_lru_queue(cache), list_get_lru_priority(node));
  UNLOCK_ROW(idx);
  return OK;
}

enum code cache_put(Cache cache, char mode, char* key, unsigned klen, char *value, unsigned vlen) {
  printf("key (put) %s\n",key);
  printf("klen (put) %u \n",klen);
  unsigned idx = NROW(key, klen);
  WR_LOCK_ROW(idx);
  List node = list_search(cache->buckets[idx], mode, key, klen);
  if (!node) {
    Data new_data = data_wrap(key, klen, value, vlen, mode);
    // unlock row hasta conseguir prioridad?
    List new_node = list_insert(cache->buckets[idx], new_data);
    LRUNode lru_priority = lru_push(cache->queue, idx, new_node);
    // aca habria que lockear la row y hicimos el unlock antes
    list_set_lru_priority(new_node, lru_priority);
  } else {
    Data data = list_get_data(node);
    free(data.val);
    data.mode = mode;
    data.val = value;
    data.vlen = vlen;
    list_set_data(node, data);
    reset_lru_status(cache->queue, list_get_lru_priority(node));
  }
  UNLOCK_ROW(idx);
  return OK;
}

enum code cache_del(Cache cache, char mode, char* key, unsigned klen) {
  unsigned idx = NROW(key, klen);
  WR_LOCK_ROW(idx);
  List list = cache->buckets[idx];
  List del_node = list_search_and_remove(list, mode, key, klen);
  if (!del_node) {
    UNLOCK_ROW(idx);
    return ENOTFOUND;
  } else {
    LRUNode lru_priority = list_get_lru_priority(del_node);
    lru_remove(cache->queue, lru_priority);
    UNLOCK_ROW(idx);
    lru_free_node(lru_priority);
    list_free_node(del_node);
  }
  return OK;
}

enum code cache_stats(Cache cache, char mode, struct Stats* stats) {
  switch (mode) {
  case TEXT_MODE:
    pthread_mutex_lock(&cache->ts_lock);
    *stats = cache->text_stats;
    pthread_mutex_unlock(&cache->ts_lock);
    break;
  case BIN_MODE:
    pthread_mutex_lock(&cache->bs_lock);
    *stats = cache->bin_stats;
    pthread_mutex_unlock(&cache->bs_lock);
    break;
  default:
    return EUNK;
  }
  return OK;
}

int cache_try_dismiss(Cache cache, uint64_t idx, List data_node) {
  if (!WR_TRYLOCK_ROW(idx)) // Casilla bloqueada
    return 0;
  list_remove(data_node);
  list_free_node(data_node);
  UNLOCK_ROW(idx);
  return 1;
}
