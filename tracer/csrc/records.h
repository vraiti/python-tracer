#ifndef TRACER_RECORDS_H
#define TRACER_RECORDS_H

#include <Python.h>
#include <stdint.h>
#include "hashmap.h"

/* ---- Plain C record structs (no PyObject_HEAD) ---- */

typedef struct {
    uint64_t caller_id;
    int32_t call_lineno;
} AttrRecordWriteData;

typedef struct {
    uint64_t caller_id;
    int32_t write_call_lineno;
    int32_t read_call_lineno;
} AttrRecordReadData;

typedef struct {
    uint64_t call_id;
    int32_t function_id;
    uint64_t caller_id;
    int32_t call_lineno;
    int32_t obj_id;
    uint8_t *control_flow;
    Py_ssize_t control_flow_len;
    AttrRecordReadData *attr_reads;
    Py_ssize_t attr_reads_len;
    Py_ssize_t attr_reads_cap;
} CallRecordData;

typedef struct {
    uint64_t call_id;
    SMap members;
} ObjectRecordData;

typedef struct {
    char *name;
    int64_t obj_idx;
} IpcRecordData;

/* ---- Database (Python type with C array storage) ---- */

typedef struct {
    PyObject_HEAD
    CallRecordData *calls;
    Py_ssize_t calls_len, calls_cap;
    ObjectRecordData *objects;
    Py_ssize_t objects_len, objects_cap;
    IpcRecordData *ipc;
    Py_ssize_t ipc_len, ipc_cap;
    SMap arw_map;
} DatabaseObject;

extern PyTypeObject *DatabaseType;

/* ---- Database helpers ---- */

CallRecordData *db_add_call(DatabaseObject *db,
                            uint64_t call_id, int32_t function_id,
                            uint64_t caller_id, int32_t call_lineno,
                            int32_t obj_id);

Py_ssize_t db_add_object(DatabaseObject *db, uint64_t call_id);

void db_add_ipc_entry(DatabaseObject *db, const char *name, int64_t obj_idx);

void db_add_attr_read(CallRecordData *rec,
                      uint64_t caller_id,
                      int32_t write_call_lineno,
                      int32_t read_call_lineno);

void db_set_arw(DatabaseObject *db, int32_t obj_id, const char *attr_name,
                uint64_t caller_id, int32_t call_lineno);

AttrRecordWriteData *db_get_arw(DatabaseObject *db, int32_t obj_id,
                                const char *attr_name);

int records_init(PyObject *module);

#endif
