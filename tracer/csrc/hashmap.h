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

/* ---- string-keyed map macro ---- */

#define DECLARE_STRMAP(PREFIX, VTYPE)                                   \
    typedef struct {                                                    \
        char *key;                                                      \
        VTYPE value;                                                    \
        uint8_t occupied;                                               \
    } PREFIX##Entry;                                                    \
                                                                        \
    typedef struct {                                                    \
        PREFIX##Entry *entries;                                         \
        size_t capacity;                                                \
        size_t count;                                                   \
    } PREFIX;                                                           \
                                                                        \
    void PREFIX##_init(PREFIX *m, size_t initial_cap);                  \
    void PREFIX##_free(PREFIX *m);                                      \
    int PREFIX##_get(const PREFIX *m, const char *key, VTYPE *out);     \
    void PREFIX##_set(PREFIX *m, const char *key, VTYPE value);         \
    int PREFIX##_contains(const PREFIX *m, const char *key);

typedef struct {
    uint64_t caller_id;
    int32_t call_lineno;
} ARW;

DECLARE_STRMAP(SMap, void *)
DECLARE_STRMAP(ARWMap, ARW)

void smap_free_values(SMap *m);

#endif
