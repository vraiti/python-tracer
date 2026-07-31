#ifndef TRACER_OWNERSHIP_H
#define TRACER_OWNERSHIP_H

#include <Python.h>
#include "hashmap.h"
#include "records.h"

/* ---- OwnershipHook ---- */

typedef struct {
    PyObject_HEAD
    PyObject *db;             /* DatabaseObject* */
    PyObject *trace_hook;
    UMap patched_classes;     /* cls ptr -> 1 */
} OwnershipHookObject;

extern PyTypeObject *OwnershipHookType;

/* ---- TracedSetattr / BoundSetattr ---- */

typedef struct {
    PyObject_HEAD
    PyObject *original;
    PyObject *db;
    PyObject *trace_hook;
} TracedSetattrObject;

extern PyTypeObject *TracedSetattrType;

typedef struct {
    PyObject_HEAD
    PyObject *inner;          /* TracedSetattrObject* */
    PyObject *instance;
} BoundSetattrObject;

extern PyTypeObject *BoundSetattrType;

/* ---- TracedGetattr / BoundGetattr ---- */

typedef struct {
    PyObject_HEAD
    PyObject *original;
    PyObject *trace_hook;
} TracedGetattrObject;

extern PyTypeObject *TracedGetattrType;

typedef struct {
    PyObject_HEAD
    PyObject *inner;          /* TracedGetattrObject* */
    PyObject *instance;
} BoundGetattrObject;

extern PyTypeObject *BoundGetattrType;

/* Called from hook.c */
void ownership_patch_class(PyObject *ownership, PyObject *cls);

int ownership_init(PyObject *module);

#endif
