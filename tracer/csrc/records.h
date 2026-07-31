#ifndef TRACER_RECORDS_H
#define TRACER_RECORDS_H

#include <Python.h>
#include <stdint.h>

/* ---- AttrRecordWrite ---- */

typedef struct {
    PyObject_HEAD
    uint64_t caller_id;
    int32_t call_lineno;
} AttrRecordWriteObject;

extern PyTypeObject *AttrRecordWriteType;

/* ---- AttrRecordRead ---- */

typedef struct {
    PyObject_HEAD
    uint64_t caller_id;
    int32_t write_call_lineno;
    int32_t read_call_lineno;
} AttrRecordReadObject;

extern PyTypeObject *AttrRecordReadType;

/* ---- CallRecord ---- */

typedef struct {
    PyObject_HEAD
    uint64_t call_id;
    int32_t function_id;
    uint64_t caller_id;
    int32_t call_lineno;
    int32_t obj_id;
    PyObject *control_flow;   /* bytearray */
    PyObject *attr_reads;     /* list */
} CallRecordObject;

extern PyTypeObject *CallRecordType;

/* ---- ObjectRecord ---- */

typedef struct {
    PyObject_HEAD
    uint64_t call_id;
    PyObject *members;        /* dict */
} ObjectRecordObject;

extern PyTypeObject *ObjectRecordType;

/* ---- IpcRecord ---- */

typedef struct {
    PyObject_HEAD
    PyObject *name;           /* str */
    int64_t obj_idx;
} IpcRecordObject;

extern PyTypeObject *IpcRecordType;

/* ---- Database ---- */

typedef struct {
    PyObject_HEAD
    PyObject *calls;          /* list */
    PyObject *objects;        /* list */
    PyObject *ipc;            /* list */
} DatabaseObject;

extern PyTypeObject *DatabaseType;

/* Register all record types on the module. Returns 0 on success, -1 on error. */
int records_init(PyObject *module);

#endif
