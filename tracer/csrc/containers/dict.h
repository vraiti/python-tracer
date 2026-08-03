#ifndef TRACER_CONTAINERS_DICT_H
#define TRACER_CONTAINERS_DICT_H

#include "../hashmap.h"
#include <stddef.h>
#include <stdint.h>

#define ARWDICT_MINSIZE 8
#define ARWDICT_EMPTY   (-1)
#define ARWDICT_DUMMY   (-2)

typedef struct {
    char *key;
    ARW value;
} ARWDictEntry;

typedef struct {
    int8_t log2_size;
    size_t usable;
    size_t nentries;
    size_t used;
    int32_t *indices;
    ARWDictEntry *entries;
} ARWDict;

void arwdict_init(ARWDict *d);
void arwdict_free(ARWDict *d);
int arwdict_get(const ARWDict *d, const char *key, ARW *out);
int arwdict_set(ARWDict *d, const char *key, ARW value);
int arwdict_del(ARWDict *d, const char *key);
int arwdict_contains(const ARWDict *d, const char *key);
size_t arwdict_len(const ARWDict *d);

#ifdef Py_PYTHON_H

#include "../hook.h"

typedef struct {
    ObjectTraceData base;
    ARWDict arws;
} DictTraceData;

typedef struct {
    PyDictObject dict;
    PyObject *db;
} TracedDictObject;

extern PyTypeObject *TracedDictType;

int traced_dict_init(PyObject *module);

#endif /* Py_PYTHON_H */

#endif
