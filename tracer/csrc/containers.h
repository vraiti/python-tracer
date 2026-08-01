#ifndef TRACER_CONTAINERS_H
#define TRACER_CONTAINERS_H

#include <Python.h>
#include <stdint.h>
#include "hashmap.h"
#include "records.h"

typedef struct {
    uint64_t caller_id;
    int32_t call_lineno;
} ArwEntry;

/* ---- TracedDict (subclasses dict) ---- */

typedef struct {
    PyDictObject dict;
    UMap arws;            /* hash(key) -> heap ArwEntry* (cast to intptr_t) */
    PyObject *db;
    PyObject *trace_hook;
} TracedDictObject;

extern PyTypeObject *TracedDictType;

/* ---- TracedList (subclasses list) ---- */

typedef struct {
    PyListObject list;
    ArwEntry *arws;
    size_t arw_count;
    size_t arw_cap;
    PyObject *db;
    PyObject *trace_hook;
} TracedListObject;

extern PyTypeObject *TracedListType;

/* ---- TracedDeque (wrapper) ---- */

typedef struct {
    PyObject_HEAD
    PyObject *inner;      /* deque */
    ArwEntry *arws;
    size_t arw_count;
    size_t arw_cap;
    PyObject *db;
    PyObject *trace_hook;
} TracedDequeObject;

extern PyTypeObject *TracedDequeType;

PyObject *wrap_container_inner(PyObject *value, PyObject *db,
                               PyObject *trace_hook, int obj_idx,
                               const char *attr_name);

int containers_init(PyObject *module);

#endif
