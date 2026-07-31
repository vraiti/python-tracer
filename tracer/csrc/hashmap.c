#include "hashmap.h"
#include <stdlib.h>
#include <string.h>

/* ---- usize-keyed map ---- */

static inline size_t umap_hash(uintptr_t key) {
    return (size_t)(key * 11400714819323198485ULL);
}

void umap_init(UMap *m, size_t initial_cap) {
    if (initial_cap < 16) initial_cap = 16;
    m->entries = calloc(initial_cap, sizeof(UMapEntry));
    m->capacity = initial_cap;
    m->count = 0;
}

void umap_free(UMap *m) {
    free(m->entries);
    m->entries = NULL;
    m->capacity = 0;
    m->count = 0;
}

static void umap_grow(UMap *m) {
    size_t old_cap = m->capacity;
    UMapEntry *old = m->entries;
    size_t new_cap = old_cap * 2;
    m->entries = calloc(new_cap, sizeof(UMapEntry));
    m->capacity = new_cap;
    m->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].occupied)
            umap_set(m, old[i].key, old[i].value);
    }
    free(old);
}

int umap_get(const UMap *m, uintptr_t key, intptr_t *out) {
    if (!m->entries) return 0;
    size_t mask = m->capacity - 1;
    size_t idx = umap_hash(key) & mask;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t pos = (idx + i) & mask;
        if (!m->entries[pos].occupied) return 0;
        if (m->entries[pos].key == key) {
            *out = m->entries[pos].value;
            return 1;
        }
    }
    return 0;
}

void umap_set(UMap *m, uintptr_t key, intptr_t value) {
    if (m->count * 4 >= m->capacity * 3)
        umap_grow(m);
    size_t mask = m->capacity - 1;
    size_t idx = umap_hash(key) & mask;
    for (;;) {
        if (!m->entries[idx].occupied) {
            m->entries[idx].key = key;
            m->entries[idx].value = value;
            m->entries[idx].occupied = 1;
            m->count++;
            return;
        }
        if (m->entries[idx].key == key) {
            m->entries[idx].value = value;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

int umap_contains(const UMap *m, uintptr_t key) {
    intptr_t dummy;
    return umap_get(m, key, &dummy);
}

/* ---- string-keyed map ---- */

static inline size_t smap_hash(const char *key) {
    size_t h = 14695981039346656037ULL;
    for (const char *p = key; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

void smap_init(SMap *m, size_t initial_cap) {
    if (initial_cap < 16) initial_cap = 16;
    m->entries = calloc(initial_cap, sizeof(SMapEntry));
    m->capacity = initial_cap;
    m->count = 0;
}

void smap_free(SMap *m) {
    if (m->entries) {
        for (size_t i = 0; i < m->capacity; i++) {
            if (m->entries[i].occupied)
                free(m->entries[i].key);
        }
        free(m->entries);
    }
    m->entries = NULL;
    m->capacity = 0;
    m->count = 0;
}

void smap_free_values(SMap *m) {
    if (m->entries) {
        for (size_t i = 0; i < m->capacity; i++) {
            if (m->entries[i].occupied) {
                free(m->entries[i].key);
                free(m->entries[i].value);
            }
        }
        free(m->entries);
    }
    m->entries = NULL;
    m->capacity = 0;
    m->count = 0;
}

static void smap_grow(SMap *m) {
    size_t old_cap = m->capacity;
    SMapEntry *old = m->entries;
    size_t new_cap = old_cap * 2;
    m->entries = calloc(new_cap, sizeof(SMapEntry));
    m->capacity = new_cap;
    m->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].occupied) {
            /* transfer ownership of key string — don't strdup again */
            size_t mask = new_cap - 1;
            size_t idx = smap_hash(old[i].key) & mask;
            while (m->entries[idx].occupied)
                idx = (idx + 1) & mask;
            m->entries[idx].key = old[i].key;
            m->entries[idx].value = old[i].value;
            m->entries[idx].occupied = 1;
            m->count++;
        }
    }
    free(old);
}

int smap_get(const SMap *m, const char *key, void **out) {
    if (!m->entries) return 0;
    size_t mask = m->capacity - 1;
    size_t idx = smap_hash(key) & mask;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t pos = (idx + i) & mask;
        if (!m->entries[pos].occupied) return 0;
        if (strcmp(m->entries[pos].key, key) == 0) {
            *out = m->entries[pos].value;
            return 1;
        }
    }
    return 0;
}

void smap_set(SMap *m, const char *key, void *value) {
    if (m->count * 4 >= m->capacity * 3)
        smap_grow(m);
    size_t mask = m->capacity - 1;
    size_t idx = smap_hash(key) & mask;
    for (;;) {
        if (!m->entries[idx].occupied) {
            m->entries[idx].key = strdup(key);
            m->entries[idx].value = value;
            m->entries[idx].occupied = 1;
            m->count++;
            return;
        }
        if (strcmp(m->entries[idx].key, key) == 0) {
            m->entries[idx].value = value;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

int smap_contains(const SMap *m, const char *key) {
    void *dummy;
    return smap_get(m, key, &dummy);
}
