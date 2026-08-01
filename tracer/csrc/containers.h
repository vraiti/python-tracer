#ifndef TRACER_CONTAINERS_H
#define TRACER_CONTAINERS_H

#include <Python.h>
#include <stdint.h>
#include "records.h"

typedef struct {
    uint64_t caller_id;
    int32_t call_lineno;
} ArwEntry;

/* ---- TracedDict (subclasses dict) ---- */

typedef struct {
    PyDictObject dict;    /* internal dict stores key -> (caller_id, lineno) tuples */
    PyObject *source;     /* the actual dict being tracked */
    PyObject *db;
} TracedDictObject;

extern PyTypeObject *TracedDictType;

/* ---- TracedList (subclasses list) ---- */

typedef struct {
    PyListObject list;
    ArwEntry *arws;
    size_t arw_count;
    size_t arw_cap;
    PyObject *db;
} TracedListObject;

extern PyTypeObject *TracedListType;

/* ---- TracedDeque (subclasses collections.deque) ---- */
/* Extra fields accessed at runtime offset past deque's struct */

extern PyTypeObject *TracedDequeType;

PyObject *wrap_container_inner(PyObject *value, PyObject *db, int obj_idx);

int containers_init(PyObject *module);

#endif
