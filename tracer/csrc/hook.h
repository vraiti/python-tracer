#ifndef TRACER_HOOK_H
#define TRACER_HOOK_H

#include <Python.h>
#include <stdint.h>
#include "hashmap.h"
#include "records.h"
#include "filter.h"

/* Bitset for control-flow lines */
typedef struct {
    uint64_t *words;
    int32_t max_line;
    size_t n_words;
} Bitset;

/* Per-frame entry on the trace stack */
typedef struct {
    uint64_t call_id;
    CallRecordData *record;
    const Bitset *cf_bits;
    int32_t pending_cf;
    uint8_t *branch_buf;
    size_t branch_len;
    size_t branch_cap;
} FrameEntry;

/* Global trace state */
typedef struct {
    uint64_t next_call_id;
    int enabled;

    PyObject *hook_obj;       /* the object passed to PyEval_SetTrace */
    PyObject *db;             /* DatabaseObject* */
    PyObject *ownership;      /* OwnershipHookObject* */
    PyObject *filter;         /* PathFilterObject* */

    char **prefixes;
    Py_ssize_t prefix_count;
    UMap scope_cache;

    char **taint_patterns;
    Py_ssize_t taint_count;

    /* AST data */
    SMap func_to_id;          /* ref_str -> int32_t (cast from void*) */
    int32_t next_func_id;
    Bitset *cf_bits;          /* array indexed by function_id */
    int32_t cf_bits_len;
    int32_t cf_bits_cap;

    /* Frame stack */
    FrameEntry *frames;
    size_t frame_count;
    size_t frame_cap;
} TraceState;

extern TraceState g_state;

int hook_init(PyObject *module);

#endif
