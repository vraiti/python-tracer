#ifndef TRACER_CONTAINERS_SET_H
#define TRACER_CONTAINERS_SET_H

#include "../hashmap.h"
#include <stddef.h>
#include <stdint.h>

#define ARWSET_MINSIZE 8
#define ARWSET_EMPTY   0
#define ARWSET_DUMMY   ((Py_hash_t)-1)

typedef struct {
    Py_hash_t hash;
    ARW value;
} ARWSetEntry;

typedef struct {
    ARWSetEntry *table;
    ARWSetEntry smalltable[ARWSET_MINSIZE];
    size_t mask;
    size_t fill;
    size_t used;
    size_t finger;
} ARWSet;

void arwset_init(ARWSet *s);
void arwset_free(ARWSet *s);
int arwset_add(ARWSet *s, Py_hash_t hash, ARW value);
int arwset_get(const ARWSet *s, Py_hash_t hash, ARW *out);
int arwset_del(ARWSet *s, Py_hash_t hash);
int arwset_contains(const ARWSet *s, Py_hash_t hash);
int arwset_pop(ARWSet *s, ARW *out);
size_t arwset_len(const ARWSet *s);

#ifdef Py_PYTHON_H

#include "../hook.h"

typedef struct {
    ObjectTraceData base;
    ARWSet arws;
} SetTraceData;

typedef struct {
    PySetObject set;
    PyObject *db;
} TracedSetObject;

extern PyTypeObject *TracedSetType;

int traced_set_init(PyObject *module);

#endif /* Py_PYTHON_H */

#endif
