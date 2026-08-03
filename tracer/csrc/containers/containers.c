#include "containers.h"
#include "internal/pycore_frame.h"
#include <string.h>
#include <stdlib.h>

extern PyObject *py_current_record(PyObject *self, PyObject *args);

ARW caller_arw(void) {
    ARW e = {0, 0};
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return e;
    e.caller_id = frame->f_frame->call_id;
    e.call_lineno = PyFrame_GetLineNumber(frame);
    return e;
}

void emit_read(const ARW *arw) {
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;
    PyObject *rec = py_current_record(NULL, NULL);
    if (!rec || rec == Py_None) { Py_XDECREF(rec); return; }
    uint64_t caller_id = frame->f_frame->call_id;
    int lineno = PyFrame_GetLineNumber(frame);
    PyObject *attr_read = PyObject_CallFunction(
        (PyObject *)AttrRecordReadType,
        "Kii", caller_id, arw->call_lineno, lineno);
    if (attr_read) {
        PyList_Append(((CallRecordObject *)rec)->attr_reads, attr_read);
        Py_DECREF(attr_read);
    } else {
        PyErr_Clear();
    }
    Py_DECREF(rec);
}

PyObject *wrap_container(PyObject *value, PyObject *db, int obj_idx) {
    ObjectTraceData *existing = (ObjectTraceData *)PyObject_GetExtra(value);
    if (existing && existing->type != CONTAINER_NONE)
        Py_RETURN_NONE;

    if (PyDict_Check(value)) {
        return PyObject_CallFunction(
            (PyObject *)TracedDictType, "OOi",
            value, db, obj_idx);
    }
    if (PyList_Check(value)) {
        return PyObject_CallFunction(
            (PyObject *)TracedListType, "OOi",
            value, db, obj_idx);
    }
    if (PyAnySet_Check(value) && !PyFrozenSet_Check(value)) {
        return PyObject_CallFunction(
            (PyObject *)TracedSetType, "OOi",
            value, db, obj_idx);
    }
    PyObject *qn = PyObject_GetAttrString((PyObject *)Py_TYPE(value), "__qualname__");
    if (qn) {
        const char *qns = PyUnicode_AsUTF8(qn);
        Py_DECREF(qn);
        if (qns && strcmp(qns, "deque") == 0) {
            return PyObject_CallFunction(
                (PyObject *)TracedDequeType, "OOi",
                value, db, obj_idx);
        }
    } else {
        PyErr_Clear();
    }
    Py_RETURN_NONE;
}

int containers_init(PyObject *module) {
    if (traced_dict_init(module) < 0) return -1;
    if (traced_list_init(module) < 0) return -1;
    if (traced_deque_init(module) < 0) return -1;
    if (traced_set_init(module) < 0) return -1;
    return 0;
}
