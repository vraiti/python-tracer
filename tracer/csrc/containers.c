#include "containers.h"
#include "internal/pycore_frame.h"
#include <string.h>
#include <stdlib.h>

/* forward from hook.c */
extern PyObject *py_current_record(PyObject *self, PyObject *args);

/* ---- helpers ---- */

static ArwEntry caller_arw(void) {
    ArwEntry e = {0, 0};
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return e;
    e.caller_id = ((PyFrameObject *)frame)->call_id;
    e.call_lineno = PyFrame_GetLineNumber(frame);
    return e;
}

static void emit_read(const ArwEntry *arw) {
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;
    PyObject *rec = py_current_record(NULL, NULL);
    if (!rec || rec == Py_None) { Py_XDECREF(rec); return; }
    uint64_t caller_id = ((PyFrameObject *)frame)->call_id;
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

static uint64_t pyobj_keyhash(PyObject *key) {
    Py_hash_t h = PyObject_Hash(key);
    if (h == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return (uint64_t)(uintptr_t)key;
    }
    return (uint64_t)h;
}

/* ======================================================================== */
/* TracedDict                                                                */
/* ======================================================================== */

PyTypeObject *TracedDictType = NULL;

static int TracedDict_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDictObject *o = (TracedDictObject *)self;
    static char *kwlist[] = {"source", "db", "trace_hook", "owner_idx", "attr", NULL};
    PyObject *source, *db, *trace_hook;
    int owner_idx;
    const char *attr;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOOis", kwlist,
            &source, &db, &trace_hook, &owner_idx, &attr))
        return -1;

    o->inner = PyDict_Copy(source);
    if (!o->inner) return -1;
    Py_INCREF(db); o->db = db;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;
    umap_init(&o->arws, 32);

    PyObject *keys = PyDict_Keys(source);
    if (keys) {
        Py_ssize_t n = PyList_GET_SIZE(keys);
        for (Py_ssize_t i = 0; i < n; i++) {
            uint64_t h = pyobj_keyhash(PyList_GET_ITEM(keys, i));
            ArwEntry *e = calloc(1, sizeof(ArwEntry));
            umap_set(&o->arws, (uintptr_t)h, (intptr_t)e);
        }
        Py_DECREF(keys);
    }
    return 0;
}

static void TracedDict_dealloc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->inner);
    Py_XDECREF(o->db);
    Py_XDECREF(o->trace_hook);
    if (o->arws.entries) {
        for (size_t i = 0; i < o->arws.capacity; i++)
            if (o->arws.entries[i].occupied && o->arws.entries[i].value)
                free((void *)o->arws.entries[i].value);
    }
    umap_free(&o->arws);
    Py_TYPE(self)->tp_free(self);
}

static int TracedDict_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_VISIT(o->inner); Py_VISIT(o->db); Py_VISIT(o->trace_hook);
    return 0;
}

static int TracedDict_clear_gc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_CLEAR(o->inner); Py_CLEAR(o->db); Py_CLEAR(o->trace_hook);
    return 0;
}

static Py_ssize_t TracedDict_len(PyObject *self) {
    return PyDict_Size(((TracedDictObject *)self)->inner);
}

static int TracedDict_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    TracedDictObject *o = (TracedDictObject *)self;
    if (value == NULL) {
        /* __delitem__ */
        if (PyDict_DelItem(o->inner, key) < 0) return -1;
        uint64_t h = pyobj_keyhash(key);
        intptr_t old;
        if (umap_get(&o->arws, (uintptr_t)h, &old)) {
            free((void *)old);
            umap_set(&o->arws, (uintptr_t)h, (intptr_t)0);
        }
        return 0;
    }
    /* __setitem__ */
    if (PyDict_SetItem(o->inner, key, value) < 0) return -1;
    uint64_t h = pyobj_keyhash(key);
    intptr_t old;
    ArwEntry *e;
    if (umap_get(&o->arws, (uintptr_t)h, &old)) {
        e = (ArwEntry *)old;
    } else {
        e = malloc(sizeof(ArwEntry));
        umap_set(&o->arws, (uintptr_t)h, (intptr_t)e);
    }
    *e = caller_arw();
    return 0;
}

static PyObject *TracedDict_subscript(PyObject *self, PyObject *key) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *val = PyDict_GetItemWithError(o->inner, key);
    if (!val) {
        if (!PyErr_Occurred())
            PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    uint64_t h = pyobj_keyhash(key);
    intptr_t arw_ptr;
    if (umap_get(&o->arws, (uintptr_t)h, &arw_ptr))
        emit_read((ArwEntry *)arw_ptr);
    Py_INCREF(val);
    return val;
}

static int TracedDict_contains(PyObject *self, PyObject *key) {
    return PyDict_Contains(((TracedDictObject *)self)->inner, key);
}

static PyObject *TracedDict_repr(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *r = PyObject_Repr(o->inner);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedDict(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedDict_iter(PyObject *self) {
    return PyObject_GetIter(((TracedDictObject *)self)->inner);
}

static PyObject *TracedDict_get(PyObject *self, PyObject *args) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    PyObject *val = PyDict_GetItemWithError(o->inner, key);
    if (val) {
        uint64_t h = pyobj_keyhash(key);
        intptr_t arw_ptr;
        if (umap_get(&o->arws, (uintptr_t)h, &arw_ptr))
            emit_read((ArwEntry *)arw_ptr);
        Py_INCREF(val);
        return val;
    }
    if (PyErr_Occurred()) return NULL;
    Py_INCREF(def);
    return def;
}

static PyObject *TracedDict_pop(PyObject *self, PyObject *args) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *key;
    PyObject *def = NULL;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    uint64_t h = pyobj_keyhash(key);
    intptr_t arw_ptr;
    if (umap_get(&o->arws, (uintptr_t)h, &arw_ptr))
        emit_read((ArwEntry *)arw_ptr);
    PyObject *result;
    if (def)
        result = PyObject_CallMethod(o->inner, "pop", "OO", key, def);
    else
        result = PyObject_CallMethod(o->inner, "pop", "(O)", key);
    return result;
}

static PyObject *TracedDict_update(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *result = PyObject_Call(
        PyObject_GetAttrString(o->inner, "update"), args, kw);
    if (!result) return NULL;
    Py_DECREF(result);
    ArwEntry arw = caller_arw();
    PyObject *keys = PyDict_Keys(o->inner);
    if (keys) {
        Py_ssize_t n = PyList_GET_SIZE(keys);
        for (Py_ssize_t i = 0; i < n; i++) {
            uint64_t h = pyobj_keyhash(PyList_GET_ITEM(keys, i));
            if (!umap_contains(&o->arws, (uintptr_t)h)) {
                ArwEntry *e = malloc(sizeof(ArwEntry));
                *e = arw;
                umap_set(&o->arws, (uintptr_t)h, (intptr_t)e);
            }
        }
        Py_DECREF(keys);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedDict_setdefault(PyObject *self, PyObject *args) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    if (PyDict_Contains(o->inner, key)) {
        return TracedDict_subscript(self, key);
    }
    TracedDict_ass_sub(self, key, def);
    Py_INCREF(def);
    return def;
}

static PyObject *TracedDict_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyDict_Clear(o->inner);
    if (o->arws.entries) {
        for (size_t i = 0; i < o->arws.capacity; i++)
            if (o->arws.entries[i].occupied && o->arws.entries[i].value)
                free((void *)o->arws.entries[i].value);
    }
    umap_free(&o->arws);
    umap_init(&o->arws, 32);
    Py_RETURN_NONE;
}

static PyObject *TracedDict_keys(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Keys(((TracedDictObject *)self)->inner);
}

static PyObject *TracedDict_values(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Values(((TracedDictObject *)self)->inner);
}

static PyObject *TracedDict_items(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Items(((TracedDictObject *)self)->inner);
}

static PyObject *TracedDict_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *dict_type = PyObject_GetAttrString(builtins, "dict");
    Py_DECREF(builtins);
    if (!dict_type) return NULL;
    PyObject *items = PyDict_Items(o->inner);
    if (!items) { Py_DECREF(dict_type); return NULL; }
    PyObject *t_args = PyTuple_Pack(1, items);
    Py_DECREF(items);
    if (!t_args) { Py_DECREF(dict_type); return NULL; }
    PyObject *result = PyTuple_Pack(2, dict_type, t_args);
    Py_DECREF(dict_type);
    Py_DECREF(t_args);
    return result;
}

static PyObject *TracedDict_get_wrapped(PyObject *self, void *closure) {
    Py_RETURN_TRUE;
}

static PyMethodDef TracedDict_methods[] = {
    {"get",        (PyCFunction)TracedDict_get,        METH_VARARGS, NULL},
    {"pop",        (PyCFunction)TracedDict_pop,        METH_VARARGS, NULL},
    {"update",     (PyCFunction)TracedDict_update,     METH_VARARGS | METH_KEYWORDS, NULL},
    {"setdefault", (PyCFunction)TracedDict_setdefault, METH_VARARGS, NULL},
    {"clear",      TracedDict_clear,                   METH_NOARGS, NULL},
    {"keys",       TracedDict_keys,                    METH_NOARGS, NULL},
    {"values",     TracedDict_values,                  METH_NOARGS, NULL},
    {"items",      TracedDict_items,                   METH_NOARGS, NULL},
    {"__reduce__", TracedDict_reduce,                  METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef TracedDict_getset[] = {
    {"_tr_wrapped", TracedDict_get_wrapped, NULL, NULL, NULL},
    {NULL}
};

static PySequenceMethods TracedDict_as_sequence = {
    .sq_contains = TracedDict_contains,
};

static PyMappingMethods TracedDict_as_mapping = {
    .mp_length = TracedDict_len,
    .mp_subscript = TracedDict_subscript,
    .mp_ass_subscript = TracedDict_ass_sub,
};

static PyType_Slot TracedDict_slots[] = {
    {Py_tp_init,      TracedDict_init},
    {Py_tp_dealloc,   TracedDict_dealloc},
    {Py_tp_traverse,  TracedDict_traverse},
    {Py_tp_clear,     TracedDict_clear_gc},
    {Py_tp_repr,      TracedDict_repr},
    {Py_tp_iter,      TracedDict_iter},
    {Py_tp_methods,   TracedDict_methods},
    {Py_tp_getset,    TracedDict_getset},
    {Py_sq_contains,  TracedDict_contains},
    {Py_mp_length,    TracedDict_len},
    {Py_mp_subscript, TracedDict_subscript},
    {Py_mp_ass_subscript, TracedDict_ass_sub},
    {0, NULL}
};

static PyType_Spec TracedDict_spec = {
    .name = "tracer._tracer.TracedDict",
    .basicsize = sizeof(TracedDictObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedDict_slots,
};

/* ======================================================================== */
/* TracedList                                                                */
/* ======================================================================== */

PyTypeObject *TracedListType = NULL;

static int TracedList_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedListObject *o = (TracedListObject *)self;
    static char *kwlist[] = {"source", "db", "trace_hook", "owner_idx", "attr", NULL};
    PyObject *source, *db, *trace_hook;
    int owner_idx;
    const char *attr;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOOis", kwlist,
            &source, &db, &trace_hook, &owner_idx, &attr))
        return -1;

    o->inner = PyObject_CallMethod(source, "copy", NULL);
    if (!o->inner) return -1;
    Py_INCREF(db); o->db = db;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;

    Py_ssize_t n = PyList_GET_SIZE(o->inner);
    o->arw_cap = n > 0 ? (size_t)n : 8;
    o->arws = calloc(o->arw_cap, sizeof(ArwEntry));
    o->arw_count = (size_t)n;
    return 0;
}

static void TracedList_dealloc(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->inner);
    Py_XDECREF(o->db);
    Py_XDECREF(o->trace_hook);
    free(o->arws);
    Py_TYPE(self)->tp_free(self);
}

static int TracedList_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedListObject *o = (TracedListObject *)self;
    Py_VISIT(o->inner); Py_VISIT(o->db); Py_VISIT(o->trace_hook);
    return 0;
}

static int TracedList_clear_gc(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    Py_CLEAR(o->inner); Py_CLEAR(o->db); Py_CLEAR(o->trace_hook);
    return 0;
}

static Py_ssize_t TracedList_len(PyObject *self) {
    return PyList_GET_SIZE(((TracedListObject *)self)->inner);
}

static PyObject *TracedList_repr(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject *r = PyObject_Repr(o->inner);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedList(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedList_iter(PyObject *self) {
    return PyObject_GetIter(((TracedListObject *)self)->inner);
}

static PyObject *TracedList_subscript(PyObject *self, PyObject *key) {
    TracedListObject *o = (TracedListObject *)self;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (i == -1 && PyErr_Occurred()) return NULL;
        Py_ssize_t len = (Py_ssize_t)o->arw_count;
        Py_ssize_t idx = i < 0 ? len + i : i;
        PyObject *val = PyList_GetItem(o->inner, idx);
        if (!val) return NULL;
        if (idx >= 0 && idx < len)
            emit_read(&o->arws[idx]);
        Py_INCREF(val);
        return val;
    }
    return PyObject_CallMethod(o->inner, "__getitem__", "(O)", key);
}

static int TracedList_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    TracedListObject *o = (TracedListObject *)self;
    if (!PyLong_Check(key))
        return PyObject_SetItem(o->inner, key, value);
    Py_ssize_t i = PyLong_AsSsize_t(key);
    if (i == -1 && PyErr_Occurred()) return -1;
    Py_ssize_t len = (Py_ssize_t)o->arw_count;
    Py_ssize_t idx = i < 0 ? len + i : i;
    if (PyList_SetItem(o->inner, idx, value) < 0) return -1;
    Py_INCREF(value); /* SetItem steals a ref */
    if (idx >= 0 && idx < len)
        o->arws[idx] = caller_arw();
    return 0;
}

static void tl_ensure_cap(TracedListObject *o, size_t need) {
    if (need > o->arw_cap) {
        o->arw_cap = need * 2;
        o->arws = realloc(o->arws, o->arw_cap * sizeof(ArwEntry));
    }
}

static PyObject *TracedList_append(PyObject *self, PyObject *value) {
    TracedListObject *o = (TracedListObject *)self;
    if (PyList_Append(o->inner, value) < 0) return NULL;
    tl_ensure_cap(o, o->arw_count + 1);
    o->arws[o->arw_count++] = caller_arw();
    Py_RETURN_NONE;
}

static PyObject *TracedList_extend(PyObject *self, PyObject *values) {
    TracedListObject *o = (TracedListObject *)self;
    ArwEntry arw = caller_arw();
    size_t start = (size_t)PyList_GET_SIZE(o->inner);
    PyObject *r = PyObject_CallMethod(o->inner, "extend", "(O)", values);
    if (!r) return NULL;
    Py_DECREF(r);
    size_t new_len = (size_t)PyList_GET_SIZE(o->inner);
    tl_ensure_cap(o, new_len);
    for (size_t i = start; i < new_len; i++)
        o->arws[i] = arw;
    o->arw_count = new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedList_insert(PyObject *self, PyObject *args) {
    TracedListObject *o = (TracedListObject *)self;
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "nO", &index, &value)) return NULL;
    Py_ssize_t len = (Py_ssize_t)o->arw_count;
    Py_ssize_t idx = index < 0 ? (len + 1 + index > 0 ? len + 1 + index : 0)
                                : (index < len ? index : len);
    if (PyList_Insert(o->inner, idx, value) < 0) return NULL;
    tl_ensure_cap(o, o->arw_count + 1);
    memmove(&o->arws[idx + 1], &o->arws[idx],
            (o->arw_count - (size_t)idx) * sizeof(ArwEntry));
    o->arws[idx] = caller_arw();
    o->arw_count++;
    Py_RETURN_NONE;
}

static PyObject *TracedList_pop(PyObject *self, PyObject *args) {
    TracedListObject *o = (TracedListObject *)self;
    Py_ssize_t index = -1;
    if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
    Py_ssize_t len = (Py_ssize_t)o->arw_count;
    Py_ssize_t idx = index < 0 ? len + index : index;
    if (idx >= 0 && idx < len) {
        emit_read(&o->arws[idx]);
        memmove(&o->arws[idx], &o->arws[idx + 1],
                (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
        o->arw_count--;
    }
    return PyObject_CallMethod(o->inner, "pop", "n", index);
}

static PyObject *TracedList_remove(PyObject *self, PyObject *value) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject *idx_obj = PyObject_CallMethod(o->inner, "index", "(O)", value);
    if (!idx_obj) return NULL;
    Py_ssize_t idx = PyLong_AsSsize_t(idx_obj);
    Py_DECREF(idx_obj);
    if (idx >= 0 && (size_t)idx < o->arw_count) {
        memmove(&o->arws[idx], &o->arws[idx + 1],
                (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
        o->arw_count--;
    }
    return PyObject_CallMethod(o->inner, "remove", "(O)", value);
}

static PyObject *TracedList_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject *r = PyObject_CallMethod(o->inner, "clear", NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    o->arw_count = 0;
    Py_RETURN_NONE;
}

static PyObject *TracedList_copy(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyObject_CallMethod(((TracedListObject *)self)->inner, "copy", NULL);
}

static PyObject *TracedList_add(PyObject *self, PyObject *other) {
    return PyObject_CallMethod(((TracedListObject *)self)->inner, "__add__", "(O)", other);
}

static PyObject *TracedList_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *list_type = PyObject_GetAttrString(builtins, "list");
    Py_DECREF(builtins);
    if (!list_type) return NULL;
    PyObject *t_args = PyTuple_Pack(1, o->inner);
    if (!t_args) { Py_DECREF(list_type); return NULL; }
    PyObject *result = PyTuple_Pack(2, list_type, t_args);
    Py_DECREF(list_type); Py_DECREF(t_args);
    return result;
}

static PyObject *TracedList_get_wrapped(PyObject *self, void *closure) {
    Py_RETURN_TRUE;
}

static PyMethodDef TracedList_methods[] = {
    {"append",     TracedList_append,  METH_O, NULL},
    {"extend",     TracedList_extend,  METH_O, NULL},
    {"insert",     (PyCFunction)TracedList_insert,  METH_VARARGS, NULL},
    {"pop",        (PyCFunction)TracedList_pop,      METH_VARARGS, NULL},
    {"remove",     TracedList_remove,  METH_O, NULL},
    {"clear",      TracedList_clear,   METH_NOARGS, NULL},
    {"copy",       TracedList_copy,    METH_NOARGS, NULL},
    {"__reduce__", TracedList_reduce,  METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef TracedList_getset[] = {
    {"_tr_wrapped", TracedList_get_wrapped, NULL, NULL, NULL},
    {NULL}
};

static PyNumberMethods TracedList_as_number = {
    .nb_add = TracedList_add,
};

static PyType_Slot TracedList_slots[] = {
    {Py_tp_init,      TracedList_init},
    {Py_tp_dealloc,   TracedList_dealloc},
    {Py_tp_traverse,  TracedList_traverse},
    {Py_tp_clear,     TracedList_clear_gc},
    {Py_tp_repr,      TracedList_repr},
    {Py_tp_iter,      TracedList_iter},
    {Py_tp_methods,   TracedList_methods},
    {Py_tp_getset,    TracedList_getset},
    {Py_sq_length,    TracedList_len},
    {Py_mp_length,    TracedList_len},
    {Py_mp_subscript, TracedList_subscript},
    {Py_mp_ass_subscript, TracedList_ass_sub},
    {Py_nb_add,       TracedList_add},
    {0, NULL}
};

static PyType_Spec TracedList_spec = {
    .name = "tracer._tracer.TracedList",
    .basicsize = sizeof(TracedListObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedList_slots,
};

/* ======================================================================== */
/* TracedDeque                                                               */
/* ======================================================================== */

PyTypeObject *TracedDequeType = NULL;

static int TracedDeque_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    static char *kwlist[] = {"source", "db", "trace_hook", "owner_idx", "attr", NULL};
    PyObject *source, *db, *trace_hook;
    int owner_idx;
    const char *attr;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOOis", kwlist,
            &source, &db, &trace_hook, &owner_idx, &attr))
        return -1;

    PyObject *collections = PyImport_ImportModule("collections");
    if (!collections) return -1;
    PyObject *deque_type = PyObject_GetAttrString(collections, "deque");
    Py_DECREF(collections);
    if (!deque_type) return -1;
    o->inner = PyObject_CallFunctionObjArgs(deque_type, source, NULL);
    Py_DECREF(deque_type);
    if (!o->inner) return -1;

    Py_INCREF(db); o->db = db;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;

    Py_ssize_t n = PyObject_Length(o->inner);
    if (n < 0) { PyErr_Clear(); n = 0; }
    o->arw_cap = n > 0 ? (size_t)n : 8;
    o->arws = calloc(o->arw_cap, sizeof(ArwEntry));
    o->arw_count = (size_t)n;
    return 0;
}

static void TracedDeque_dealloc(PyObject *self) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->inner);
    Py_XDECREF(o->db);
    Py_XDECREF(o->trace_hook);
    free(o->arws);
    Py_TYPE(self)->tp_free(self);
}

static int TracedDeque_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    Py_VISIT(o->inner); Py_VISIT(o->db); Py_VISIT(o->trace_hook);
    return 0;
}

static int TracedDeque_clear_gc(PyObject *self) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    Py_CLEAR(o->inner); Py_CLEAR(o->db); Py_CLEAR(o->trace_hook);
    return 0;
}

static void td_ensure_cap(TracedDequeObject *o, size_t need) {
    if (need > o->arw_cap) {
        o->arw_cap = need * 2;
        o->arws = realloc(o->arws, o->arw_cap * sizeof(ArwEntry));
    }
}

static Py_ssize_t TracedDeque_len(PyObject *self) {
    return PyObject_Length(((TracedDequeObject *)self)->inner);
}

static PyObject *TracedDeque_repr(PyObject *self) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *r = PyObject_Repr(o->inner);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedDeque(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedDeque_iter(PyObject *self) {
    return PyObject_GetIter(((TracedDequeObject *)self)->inner);
}

static PyObject *TracedDeque_getitem(PyObject *self, PyObject *args) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    Py_ssize_t index;
    if (!PyArg_ParseTuple(args, "n", &index)) return NULL;
    PyObject *result = PyObject_CallMethod(o->inner, "__getitem__", "n", index);
    if (!result) return NULL;
    Py_ssize_t len = (Py_ssize_t)o->arw_count;
    Py_ssize_t idx = index < 0 ? len + index : index;
    if (idx >= 0 && idx < len)
        emit_read(&o->arws[idx]);
    return result;
}

static PyObject *TracedDeque_setitem(PyObject *self, PyObject *args) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "nO", &index, &value)) return NULL;
    PyObject *r = PyObject_CallMethod(o->inner, "__setitem__", "nO", index, value);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_ssize_t len = (Py_ssize_t)o->arw_count;
    Py_ssize_t idx = index < 0 ? len + index : index;
    if (idx >= 0 && idx < len)
        o->arws[idx] = caller_arw();
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_append(PyObject *self, PyObject *value) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *r = PyObject_CallMethod(o->inner, "append", "(O)", value);
    if (!r) return NULL;
    Py_DECREF(r);
    td_ensure_cap(o, o->arw_count + 1);
    o->arws[o->arw_count++] = caller_arw();
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_appendleft(PyObject *self, PyObject *value) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *r = PyObject_CallMethod(o->inner, "appendleft", "(O)", value);
    if (!r) return NULL;
    Py_DECREF(r);
    td_ensure_cap(o, o->arw_count + 1);
    memmove(&o->arws[1], &o->arws[0], o->arw_count * sizeof(ArwEntry));
    o->arws[0] = caller_arw();
    o->arw_count++;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extend(PyObject *self, PyObject *values) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    ArwEntry arw = caller_arw();
    Py_ssize_t start = PyObject_Length(o->inner);
    PyObject *r = PyObject_CallMethod(o->inner, "extend", "(O)", values);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_ssize_t new_len = PyObject_Length(o->inner);
    td_ensure_cap(o, (size_t)new_len);
    for (Py_ssize_t i = start; i < new_len; i++)
        o->arws[i] = arw;
    o->arw_count = (size_t)new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extendleft(PyObject *self, PyObject *values) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    ArwEntry arw = caller_arw();
    Py_ssize_t start = PyObject_Length(o->inner);
    PyObject *r = PyObject_CallMethod(o->inner, "extendleft", "(O)", values);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_ssize_t new_len = PyObject_Length(o->inner);
    Py_ssize_t added = new_len - start;
    td_ensure_cap(o, (size_t)new_len);
    memmove(&o->arws[added], &o->arws[0], o->arw_count * sizeof(ArwEntry));
    for (Py_ssize_t i = 0; i < added; i++)
        o->arws[i] = arw;
    o->arw_count = (size_t)new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_pop(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    if (o->arw_count > 0) {
        emit_read(&o->arws[o->arw_count - 1]);
        o->arw_count--;
    }
    return PyObject_CallMethod(o->inner, "pop", NULL);
}

static PyObject *TracedDeque_popleft(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    if (o->arw_count > 0) {
        emit_read(&o->arws[0]);
        memmove(&o->arws[0], &o->arws[1], (o->arw_count - 1) * sizeof(ArwEntry));
        o->arw_count--;
    }
    return PyObject_CallMethod(o->inner, "popleft", NULL);
}

static PyObject *TracedDeque_remove(PyObject *self, PyObject *value) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *idx_obj = PyObject_CallMethod(o->inner, "index", "(O)", value);
    if (idx_obj) {
        Py_ssize_t idx = PyLong_AsSsize_t(idx_obj);
        Py_DECREF(idx_obj);
        if (idx >= 0 && (size_t)idx < o->arw_count) {
            memmove(&o->arws[idx], &o->arws[idx + 1],
                    (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
            o->arw_count--;
        }
    } else {
        PyErr_Clear();
    }
    return PyObject_CallMethod(o->inner, "remove", "(O)", value);
}

static PyObject *TracedDeque_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *r = PyObject_CallMethod(o->inner, "clear", NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    o->arw_count = 0;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeObject *o = (TracedDequeObject *)self;
    PyObject *collections = PyImport_ImportModule("collections");
    if (!collections) return NULL;
    PyObject *deque_type = PyObject_GetAttrString(collections, "deque");
    Py_DECREF(collections);
    if (!deque_type) return NULL;
    PyObject *copy = PyObject_CallMethod(o->inner, "copy", NULL);
    if (!copy) { Py_DECREF(deque_type); return NULL; }
    PyObject *t_args = PyTuple_Pack(1, copy);
    Py_DECREF(copy);
    if (!t_args) { Py_DECREF(deque_type); return NULL; }
    PyObject *result = PyTuple_Pack(2, deque_type, t_args);
    Py_DECREF(deque_type); Py_DECREF(t_args);
    return result;
}

static PyObject *TracedDeque_get_wrapped(PyObject *self, void *closure) {
    Py_RETURN_TRUE;
}

static PyMethodDef TracedDeque_methods[] = {
    {"__getitem__", (PyCFunction)TracedDeque_getitem,     METH_VARARGS, NULL},
    {"__setitem__", (PyCFunction)TracedDeque_setitem,     METH_VARARGS, NULL},
    {"append",      TracedDeque_append,                   METH_O, NULL},
    {"appendleft",  TracedDeque_appendleft,               METH_O, NULL},
    {"extend",      TracedDeque_extend,                   METH_O, NULL},
    {"extendleft",  TracedDeque_extendleft,               METH_O, NULL},
    {"pop",         TracedDeque_pop,                      METH_NOARGS, NULL},
    {"popleft",     TracedDeque_popleft,                  METH_NOARGS, NULL},
    {"remove",      TracedDeque_remove,                   METH_O, NULL},
    {"clear",       TracedDeque_clear,                    METH_NOARGS, NULL},
    {"__reduce__",  TracedDeque_reduce,                   METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef TracedDeque_getset[] = {
    {"_tr_wrapped", TracedDeque_get_wrapped, NULL, NULL, NULL},
    {NULL}
};

static PyType_Slot TracedDeque_slots[] = {
    {Py_tp_init,     TracedDeque_init},
    {Py_tp_dealloc,  TracedDeque_dealloc},
    {Py_tp_traverse, TracedDeque_traverse},
    {Py_tp_clear,    TracedDeque_clear_gc},
    {Py_tp_repr,     TracedDeque_repr},
    {Py_tp_iter,     TracedDeque_iter},
    {Py_tp_methods,  TracedDeque_methods},
    {Py_tp_getset,   TracedDeque_getset},
    {Py_sq_length,   TracedDeque_len},
    {0, NULL}
};

static PyType_Spec TracedDeque_spec = {
    .name = "tracer._tracer.TracedDeque",
    .basicsize = sizeof(TracedDequeObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedDeque_slots,
};

/* ======================================================================== */
/* wrap_container                                                            */
/* ======================================================================== */

PyObject *wrap_container_inner(PyObject *value, PyObject *db,
                               PyObject *trace_hook, int obj_idx,
                               const char *attr_name) {
    if (PyDict_Check(value)) {
        return PyObject_CallFunction(
            (PyObject *)TracedDictType, "OOOis",
            value, db, trace_hook, obj_idx, attr_name);
    }
    if (PyList_Check(value)) {
        return PyObject_CallFunction(
            (PyObject *)TracedListType, "OOOis",
            value, db, trace_hook, obj_idx, attr_name);
    }
    PyObject *qn = PyObject_GetAttrString((PyObject *)Py_TYPE(value), "__qualname__");
    if (qn) {
        const char *qns = PyUnicode_AsUTF8(qn);
        Py_DECREF(qn);
        if (qns && strcmp(qns, "deque") == 0) {
            return PyObject_CallFunction(
                (PyObject *)TracedDequeType, "OOOis",
                value, db, trace_hook, obj_idx, attr_name);
        }
    } else {
        PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static PyObject *py_wrap_container(PyObject *self, PyObject *args) {
    PyObject *value, *db, *trace_hook;
    int owner_idx;
    const char *attr;
    if (!PyArg_ParseTuple(args, "OOOis", &value, &db, &trace_hook,
                          &owner_idx, &attr))
        return NULL;
    return wrap_container_inner(value, db, trace_hook, owner_idx, attr);
}

int containers_init(PyObject *module) {
#define REGISTER(Name, spec, typevar) do { \
    typevar = (PyTypeObject *)PyType_FromSpec(&spec); \
    if (!typevar) return -1; \
    if (PyModule_AddObject(module, #Name, (PyObject *)typevar) < 0) { \
        Py_DECREF(typevar); \
        return -1; \
    } \
} while(0)

    REGISTER(TracedDict,  TracedDict_spec,  TracedDictType);
    REGISTER(TracedList,  TracedList_spec,   TracedListType);
    REGISTER(TracedDeque, TracedDeque_spec,  TracedDequeType);

#undef REGISTER

    static PyMethodDef wrap_def = {
        "wrap_container", (PyCFunction)py_wrap_container, METH_VARARGS, NULL
    };
    PyObject *func = PyCFunction_NewEx(&wrap_def, NULL, NULL);
    if (!func) return -1;
    if (PyModule_AddObject(module, "wrap_container", func) < 0) {
        Py_DECREF(func);
        return -1;
    }
    return 0;
}
