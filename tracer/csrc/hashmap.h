#ifndef TRACER_HASHMAP_H
#define TRACER_HASHMAP_H

#include <stdint.h>
#include <stddef.h>

/* ---- usize-keyed map (key: uintptr_t, value: intptr_t) ---- */

typedef struct {
    uintptr_t key;
    intptr_t value;
    uint8_t occupied;
} UMapEntry;

typedef struct {
    UMapEntry *entries;
    size_t capacity;
    size_t count;
} UMap;

void umap_init(UMap *m, size_t initial_cap);
void umap_free(UMap *m);
int umap_get(const UMap *m, uintptr_t key, intptr_t *out);
void umap_set(UMap *m, uintptr_t key, intptr_t value);
int umap_contains(const UMap *m, uintptr_t key);

/* ---- string-keyed map (key: owned char*, value: void*) ---- */

typedef struct {
    char *key;
    void *value;
    uint8_t occupied;
} SMapEntry;

typedef struct {
    SMapEntry *entries;
    size_t capacity;
    size_t count;
} SMap;

void smap_init(SMap *m, size_t initial_cap);
void smap_free(SMap *m);
void smap_free_values(SMap *m);
int smap_get(const SMap *m, const char *key, void **out);
void smap_set(SMap *m, const char *key, void *value);
int smap_contains(const SMap *m, const char *key);

#endif
