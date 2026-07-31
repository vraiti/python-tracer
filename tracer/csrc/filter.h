#ifndef TRACER_FILTER_H
#define TRACER_FILTER_H

#include <Python.h>
#include "hashmap.h"

typedef struct {
    PyObject_HEAD
    char **prefixes;
    Py_ssize_t prefix_count;
    SMap tracked_classes;    /* string set (values unused) */
    UMap scope_cache;        /* ptr -> bool */
} PathFilterObject;

extern PyTypeObject *PathFilterType;

int filter_init(PyObject *module);

#endif
