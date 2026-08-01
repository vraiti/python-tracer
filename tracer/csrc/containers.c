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

/* ======================================================================== */
/* TracedDict  (subclasses dict)                                             */
/* ======================================================================== */

PyTypeObject *TracedDictType = NULL;

static PyObject *make_arw_tuple(ArwEntry e) {
    return Py_BuildValue("Ki", e.caller_id, e.call_lineno);
}

static ArwEntry arw_from_tuple(PyObject *t) {
    ArwEntry e = {0, 0};
    if (t && PyTuple_CheckExact(t) && PyTuple_GET_SIZE(t) == 2) {
        e.caller_id = PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(t, 0));
        e.call_lineno = (int32_t)PyLong_AsLong(PyTuple_GET_ITEM(t, 1));
        if (PyErr_Occurred()) { PyErr_Clear(); e.caller_id = 0; e.call_lineno = 0; }
    }
    return e;
}

static void td_emit_read(PyObject *self, PyObject *key) {
    PyObject *arw_obj = PyDict_GetItemWithError(self, key);
    if (arw_obj) {
        ArwEntry e = arw_from_tuple(arw_obj);
        emit_read(&e);
    } else if (PyErr_Occurred()) {
        PyErr_Clear();
    }
}

static int TracedDict_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDictObject *o = (TracedDictObject *)self;
    static char *kwlist[] = {"source", "db", "owner_idx", NULL};
    PyObject *source, *db;
    int owner_idx;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi", kwlist,
            &source, &db, &owner_idx))
        return -1;

    Py_INCREF(source); o->source = source;
    Py_INCREF(db); o->db = db;

    PyObject *zero_arw = make_arw_tuple((ArwEntry){0, 0});
    if (!zero_arw) return -1;
    PyObject *keys = PyDict_Keys(source);
    if (keys) {
        Py_ssize_t n = PyList_GET_SIZE(keys);
        for (Py_ssize_t i = 0; i < n; i++)
            PyDict_SetItem(self, PyList_GET_ITEM(keys, i), zero_arw);
        Py_DECREF(keys);
    }
    Py_DECREF(zero_arw);
    return 0;
}

static void TracedDict_dealloc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->source);
    Py_XDECREF(o->db);
    PyDict_Type.tp_dealloc(self);
}

static int TracedDict_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_VISIT(o->source); Py_VISIT(o->db);
    return PyDict_Type.tp_traverse(self, visit, arg);
}

static int TracedDict_clear_gc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_CLEAR(o->source); Py_CLEAR(o->db);
    return PyDict_Type.tp_clear(self);
}

static Py_ssize_t TracedDict_len(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Size(o->source);
}

static int TracedDict_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    TracedDictObject *o = (TracedDictObject *)self;
    if (value == NULL) {
        if (PyDict_DelItem(o->source, key) < 0) return -1;
        PyDict_DelItem(self, key);
        PyErr_Clear();
        return 0;
    }
    if (PyDict_SetItem(o->source, key, value) < 0) return -1;
    PyObject *arw = make_arw_tuple(caller_arw());
    if (arw) {
        PyDict_SetItem(self, key, arw);
        Py_DECREF(arw);
    }
    return 0;
}

static PyObject *TracedDict_subscript(PyObject *self, PyObject *key) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *val = PyDict_GetItemWithError(o->source, key);
    if (!val) {
        if (!PyErr_Occurred())
            PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    td_emit_read(self, key);
    Py_INCREF(val);
    return val;
}

static int TracedDict_contains(PyObject *self, PyObject *key) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Contains(o->source, key);
}

static PyObject *TracedDict_repr(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *r = PyObject_Repr(o->source);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedDict(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedDict_get(PyObject *self, PyObject *args) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    PyObject *val = PyDict_GetItemWithError(o->source, key);
    if (val) {
        td_emit_read(self, key);
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
    td_emit_read(self, key);
    PyObject *val = PyDict_GetItemWithError(o->source, key);
    if (val) {
        Py_INCREF(val);
        PyDict_DelItem(o->source, key);
        PyDict_DelItem(self, key);
        PyErr_Clear();
        return val;
    }
    if (PyErr_Occurred()) return NULL;
    if (def) {
        Py_INCREF(def);
        return def;
    }
    PyErr_SetObject(PyExc_KeyError, key);
    return NULL;
}

static PyObject *TracedDict_update(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *other = NULL;
    if (PyTuple_GET_SIZE(args) > 0)
        other = PyTuple_GET_ITEM(args, 0);
    if (other && PyDict_Check(other)) {
        if (PyDict_Merge(o->source, other, 1) < 0) return NULL;
    } else if (other) {
        if (PyDict_MergeFromSeq2(o->source, other, 1) < 0) return NULL;
    }
    if (kw && PyDict_Size(kw) > 0) {
        if (PyDict_Merge(o->source, kw, 1) < 0) return NULL;
    }

    ArwEntry arw = caller_arw();
    PyObject *arw_obj = make_arw_tuple(arw);
    if (arw_obj) {
        PyObject *keys = PyDict_Keys(o->source);
        if (keys) {
            Py_ssize_t n = PyList_GET_SIZE(keys);
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *k = PyList_GET_ITEM(keys, i);
                if (!PyDict_Contains(self, k))
                    PyDict_SetItem(self, k, arw_obj);
            }
            Py_DECREF(keys);
        }
        Py_DECREF(arw_obj);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedDict_setdefault(PyObject *self, PyObject *args) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    if (PyDict_Contains(o->source, key))
        return TracedDict_subscript(self, key);
    TracedDict_ass_sub(self, key, def);
    Py_INCREF(def);
    return def;
}

static PyObject *TracedDict_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyDict_Clear(o->source);
    PyDict_Clear(self);
    Py_RETURN_NONE;
}

static PyObject *TracedDict_keys(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Keys(o->source);
}

static PyObject *TracedDict_values(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Values(o->source);
}

static PyObject *TracedDict_items(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Items(o->source);
}

static PyObject *TracedDict_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *dict_type = PyObject_GetAttrString(builtins, "dict");
    Py_DECREF(builtins);
    if (!dict_type) return NULL;
    PyObject *items = PyDict_Items(o->source);
    if (!items) { Py_DECREF(dict_type); return NULL; }
    PyObject *t_args = PyTuple_Pack(1, items);
    Py_DECREF(items);
    if (!t_args) { Py_DECREF(dict_type); return NULL; }
    PyObject *result = PyTuple_Pack(2, dict_type, t_args);
    Py_DECREF(dict_type);
    Py_DECREF(t_args);
    return result;
}

static PyObject *TracedDict_copy(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDictObject *o = (TracedDictObject *)self;
    return PyDict_Copy(o->source);
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
    {"copy",       TracedDict_copy,                    METH_NOARGS, NULL},
    {NULL}
};

static PyGetSetDef TracedDict_getset[] = {
    {"_tr_wrapped", TracedDict_get_wrapped, NULL, NULL, NULL},
    {NULL}
};

static PyType_Slot TracedDict_slots[] = {
    {Py_tp_init,      TracedDict_init},
    {Py_tp_dealloc,   TracedDict_dealloc},
    {Py_tp_traverse,  TracedDict_traverse},
    {Py_tp_clear,     TracedDict_clear_gc},
    {Py_tp_repr,      TracedDict_repr},
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
/* TracedList  (subclasses list)                                             */
/* ======================================================================== */

PyTypeObject *TracedListType = NULL;

static int TracedList_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedListObject *o = (TracedListObject *)self;
    static char *kwlist[] = {"source", "db", "owner_idx", NULL};
    PyObject *source, *db;
    int owner_idx;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi", kwlist,
            &source, &db, &owner_idx))
        return -1;

    PyObject *init_args = PyTuple_Pack(1, source);
    if (!init_args) return -1;
    if (PyList_Type.tp_init(self, init_args, NULL) < 0) {
        Py_DECREF(init_args);
        return -1;
    }
    Py_DECREF(init_args);

    Py_INCREF(db); o->db = db;

    Py_ssize_t n = PyList_GET_SIZE(self);
    o->arw_cap = n > 0 ? (size_t)n : 8;
    o->arws = calloc(o->arw_cap, sizeof(ArwEntry));
    o->arw_count = (size_t)n;
    return 0;
}

static void TracedList_dealloc(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->db);
    free(o->arws);
    PyList_Type.tp_dealloc(self);
}

static int TracedList_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedListObject *o = (TracedListObject *)self;
    Py_VISIT(o->db);
    return PyList_Type.tp_traverse(self, visit, arg);
}

static int TracedList_clear_gc(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    Py_CLEAR(o->db);
    return PyList_Type.tp_clear(self);
}

static Py_ssize_t TracedList_len(PyObject *self) {
    return PyList_GET_SIZE(self);
}

static PyObject *TracedList_repr(PyObject *self) {
    PyObject *r = PyList_Type.tp_repr(self);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedList(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedList_subscript(PyObject *self, PyObject *key) {
    TracedListObject *o = (TracedListObject *)self;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (i == -1 && PyErr_Occurred()) return NULL;
        Py_ssize_t len = PyList_GET_SIZE(self);
        Py_ssize_t idx = i < 0 ? len + i : i;
        if (idx < 0 || idx >= len) {
            PyErr_SetString(PyExc_IndexError, "list index out of range");
            return NULL;
        }
        PyObject *val = PyList_GET_ITEM(self, idx);
        if ((size_t)idx < o->arw_count)
            emit_read(&o->arws[idx]);
        Py_INCREF(val);
        return val;
    }
    return PyList_Type.tp_as_mapping->mp_subscript(self, key);
}

static int TracedList_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    TracedListObject *o = (TracedListObject *)self;
    if (!PyLong_Check(key))
        return PyList_Type.tp_as_mapping->mp_ass_subscript(self, key, value);
    Py_ssize_t i = PyLong_AsSsize_t(key);
    if (i == -1 && PyErr_Occurred()) return -1;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = i < 0 ? len + i : i;
    if (value == NULL) {
        if (idx < 0 || idx >= len) {
            PyErr_SetString(PyExc_IndexError,
                            "list assignment index out of range");
            return -1;
        }
        if ((size_t)idx < o->arw_count) {
            memmove(&o->arws[idx], &o->arws[idx + 1],
                    (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
            o->arw_count--;
        }
        return PyList_SetSlice(self, idx, idx + 1, NULL);
    }
    if (idx < 0 || idx >= len) {
        PyErr_SetString(PyExc_IndexError,
                        "list assignment index out of range");
        return -1;
    }
    Py_INCREF(value);
    if (PyList_SetItem(self, idx, value) < 0) return -1;
    if ((size_t)idx < o->arw_count)
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
    if (PyList_Append(self, value) < 0) return NULL;
    tl_ensure_cap(o, o->arw_count + 1);
    o->arws[o->arw_count++] = caller_arw();
    Py_RETURN_NONE;
}

static PyObject *TracedList_extend(PyObject *self, PyObject *values) {
    TracedListObject *o = (TracedListObject *)self;
    ArwEntry arw = caller_arw();
    Py_ssize_t start = PyList_GET_SIZE(self);

    PyObject *iter = PyObject_GetIter(values);
    if (!iter) return NULL;
    PyObject *item;
    while ((item = PyIter_Next(iter)) != NULL) {
        if (PyList_Append(self, item) < 0) {
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }
        Py_DECREF(item);
    }
    Py_DECREF(iter);
    if (PyErr_Occurred()) return NULL;

    Py_ssize_t new_len = PyList_GET_SIZE(self);
    tl_ensure_cap(o, (size_t)new_len);
    for (Py_ssize_t i = start; i < new_len; i++)
        o->arws[i] = arw;
    o->arw_count = (size_t)new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedList_insert(PyObject *self, PyObject *args) {
    TracedListObject *o = (TracedListObject *)self;
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "nO", &index, &value)) return NULL;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = index < 0 ? (len + 1 + index > 0 ? len + 1 + index : 0)
                                : (index < len ? index : len);
    if (PyList_Insert(self, idx, value) < 0) return NULL;
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
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = index < 0 ? len + index : index;
    if (idx < 0 || idx >= len) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return NULL;
    }
    if ((size_t)idx < o->arw_count) {
        emit_read(&o->arws[idx]);
        memmove(&o->arws[idx], &o->arws[idx + 1],
                (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
        o->arw_count--;
    }
    PyObject *val = PyList_GET_ITEM(self, idx);
    Py_INCREF(val);
    if (PyList_SetSlice(self, idx, idx + 1, NULL) < 0) {
        Py_DECREF(val);
        return NULL;
    }
    return val;
}

static PyObject *TracedList_remove(PyObject *self, PyObject *value) {
    TracedListObject *o = (TracedListObject *)self;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = -1;
    for (Py_ssize_t i = 0; i < len; i++) {
        int cmp = PyObject_RichCompareBool(PyList_GET_ITEM(self, i), value, Py_EQ);
        if (cmp < 0) return NULL;
        if (cmp) { idx = i; break; }
    }
    if (idx < 0) {
        PyErr_SetString(PyExc_ValueError, "list.remove(x): x not in list");
        return NULL;
    }
    if ((size_t)idx < o->arw_count) {
        memmove(&o->arws[idx], &o->arws[idx + 1],
                (o->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
        o->arw_count--;
    }
    if (PyList_SetSlice(self, idx, idx + 1, NULL) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *TracedList_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedListObject *o = (TracedListObject *)self;
    if (PyList_SetSlice(self, 0, PyList_GET_SIZE(self), NULL) < 0) return NULL;
    o->arw_count = 0;
    Py_RETURN_NONE;
}

static PyObject *TracedList_copy(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyList_GetSlice(self, 0, PyList_GET_SIZE(self));
}

static PyObject *TracedList_add(PyObject *self, PyObject *other) {
    if (PyList_Type.tp_as_sequence && PyList_Type.tp_as_sequence->sq_concat)
        return PyList_Type.tp_as_sequence->sq_concat(self, other);
    Py_RETURN_NOTIMPLEMENTED;
}

static PyObject *TracedList_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *list_type = PyObject_GetAttrString(builtins, "list");
    Py_DECREF(builtins);
    if (!list_type) return NULL;
    PyObject *t_args = PyTuple_Pack(1, self);
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

static PyType_Slot TracedList_slots[] = {
    {Py_tp_init,      TracedList_init},
    {Py_tp_dealloc,   TracedList_dealloc},
    {Py_tp_traverse,  TracedList_traverse},
    {Py_tp_clear,     TracedList_clear_gc},
    {Py_tp_repr,      TracedList_repr},
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
/* TracedDeque  (subclasses collections.deque)                               */
/* ======================================================================== */

PyTypeObject *TracedDequeType = NULL;
static PyTypeObject *deque_type_obj = NULL;
static Py_ssize_t deque_basicsize = 0;

typedef struct {
    ArwEntry *arws;
    size_t arw_count;
    size_t arw_cap;
    PyObject *db;
} TracedDequeExtra;

static inline TracedDequeExtra *td_extra(PyObject *self) {
    return (TracedDequeExtra *)((char *)self + deque_basicsize);
}

static PyObject *deque_base_call(PyObject *self, const char *method, PyObject *arg) {
    PyObject *meth = PyObject_GetAttrString((PyObject *)deque_type_obj, method);
    if (!meth) return NULL;
    PyObject *result;
    if (arg)
        result = PyObject_CallFunctionObjArgs(meth, self, arg, NULL);
    else
        result = PyObject_CallFunctionObjArgs(meth, self, NULL);
    Py_DECREF(meth);
    return result;
}

static Py_ssize_t base_deque_len(PyObject *self) {
    return deque_type_obj->tp_as_sequence->sq_length(self);
}

static int TracedDeque_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDequeExtra *x = td_extra(self);
    static char *kwlist[] = {"source", "db", "owner_idx", NULL};
    PyObject *source, *db;
    int owner_idx;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi", kwlist,
            &source, &db, &owner_idx))
        return -1;

    PyObject *base_args = PyTuple_Pack(1, source);
    if (!base_args) return -1;
    int rc = deque_type_obj->tp_init(self, base_args, NULL);
    Py_DECREF(base_args);
    if (rc < 0) return -1;

    Py_INCREF(db); x->db = db;

    Py_ssize_t n = base_deque_len(self);
    if (n < 0) { PyErr_Clear(); n = 0; }
    x->arw_cap = n > 0 ? (size_t)n : 8;
    x->arws = calloc(x->arw_cap, sizeof(ArwEntry));
    x->arw_count = (size_t)n;
    return 0;
}

static void TracedDeque_dealloc(PyObject *self) {
    TracedDequeExtra *x = td_extra(self);
    PyObject_GC_UnTrack(self);
    Py_XDECREF(x->db);
    free(x->arws);
    deque_type_obj->tp_dealloc(self);
}

static int TracedDeque_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedDequeExtra *x = td_extra(self);
    Py_VISIT(x->db);
    return deque_type_obj->tp_traverse(self, visit, arg);
}

static int TracedDeque_clear_gc(PyObject *self) {
    TracedDequeExtra *x = td_extra(self);
    Py_CLEAR(x->db);
    return deque_type_obj->tp_clear(self);
}

static void tdx_ensure_cap(TracedDequeExtra *x, size_t need) {
    if (need > x->arw_cap) {
        x->arw_cap = need * 2;
        x->arws = realloc(x->arws, x->arw_cap * sizeof(ArwEntry));
    }
}

static Py_ssize_t TracedDeque_len(PyObject *self) {
    return base_deque_len(self);
}

static PyObject *TracedDeque_repr(PyObject *self) {
    PyObject *r = deque_type_obj->tp_repr(self);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedDeque(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedDeque_iter(PyObject *self) {
    return deque_type_obj->tp_iter(self);
}

static PyObject *TracedDeque_subscript(PyObject *self, PyObject *key) {
    TracedDequeExtra *x = td_extra(self);
    PyObject *result = deque_type_obj->tp_as_mapping->mp_subscript(self, key);
    if (!result) return NULL;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (!(i == -1 && PyErr_Occurred())) {
            Py_ssize_t len = (Py_ssize_t)x->arw_count;
            Py_ssize_t idx = i < 0 ? len + i : i;
            if (idx >= 0 && idx < len)
                emit_read(&x->arws[idx]);
        } else {
            PyErr_Clear();
        }
    }
    return result;
}

static int TracedDeque_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    TracedDequeExtra *x = td_extra(self);
    if (value == NULL)
        return deque_type_obj->tp_as_mapping->mp_ass_subscript(self, key, NULL);
    if (deque_type_obj->tp_as_mapping->mp_ass_subscript(self, key, value) < 0)
        return -1;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (!(i == -1 && PyErr_Occurred())) {
            Py_ssize_t len = (Py_ssize_t)x->arw_count;
            Py_ssize_t idx = i < 0 ? len + i : i;
            if (idx >= 0 && idx < len)
                x->arws[idx] = caller_arw();
        } else {
            PyErr_Clear();
        }
    }
    return 0;
}

static PyObject *TracedDeque_append(PyObject *self, PyObject *value) {
    TracedDequeExtra *x = td_extra(self);
    PyObject *r = deque_base_call(self, "append", value);
    if (!r) return NULL;
    Py_DECREF(r);
    tdx_ensure_cap(x, x->arw_count + 1);
    x->arws[x->arw_count++] = caller_arw();
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_appendleft(PyObject *self, PyObject *value) {
    TracedDequeExtra *x = td_extra(self);
    PyObject *r = deque_base_call(self, "appendleft", value);
    if (!r) return NULL;
    Py_DECREF(r);
    tdx_ensure_cap(x, x->arw_count + 1);
    memmove(&x->arws[1], &x->arws[0], x->arw_count * sizeof(ArwEntry));
    x->arws[0] = caller_arw();
    x->arw_count++;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extend(PyObject *self, PyObject *values) {
    TracedDequeExtra *x = td_extra(self);
    ArwEntry arw = caller_arw();
    Py_ssize_t start = base_deque_len(self);
    PyObject *r = deque_base_call(self, "extend", values);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_ssize_t new_len = base_deque_len(self);
    tdx_ensure_cap(x, (size_t)new_len);
    for (Py_ssize_t i = start; i < new_len; i++)
        x->arws[i] = arw;
    x->arw_count = (size_t)new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extendleft(PyObject *self, PyObject *values) {
    TracedDequeExtra *x = td_extra(self);
    ArwEntry arw = caller_arw();
    Py_ssize_t start = base_deque_len(self);
    PyObject *r = deque_base_call(self, "extendleft", values);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_ssize_t new_len = base_deque_len(self);
    Py_ssize_t added = new_len - start;
    tdx_ensure_cap(x, (size_t)new_len);
    memmove(&x->arws[added], &x->arws[0], x->arw_count * sizeof(ArwEntry));
    for (Py_ssize_t i = 0; i < added; i++)
        x->arws[i] = arw;
    x->arw_count = (size_t)new_len;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_pop(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeExtra *x = td_extra(self);
    if (x->arw_count > 0) {
        emit_read(&x->arws[x->arw_count - 1]);
        x->arw_count--;
    }
    return deque_base_call(self, "pop", NULL);
}

static PyObject *TracedDeque_popleft(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeExtra *x = td_extra(self);
    if (x->arw_count > 0) {
        emit_read(&x->arws[0]);
        memmove(&x->arws[0], &x->arws[1], (x->arw_count - 1) * sizeof(ArwEntry));
        x->arw_count--;
    }
    return deque_base_call(self, "popleft", NULL);
}

static PyObject *TracedDeque_remove(PyObject *self, PyObject *value) {
    TracedDequeExtra *x = td_extra(self);
    PyObject *idx_obj = deque_base_call(self, "index", value);
    if (idx_obj) {
        Py_ssize_t idx = PyLong_AsSsize_t(idx_obj);
        Py_DECREF(idx_obj);
        if (idx >= 0 && (size_t)idx < x->arw_count) {
            memmove(&x->arws[idx], &x->arws[idx + 1],
                    (x->arw_count - (size_t)idx - 1) * sizeof(ArwEntry));
            x->arw_count--;
        }
    } else {
        PyErr_Clear();
    }
    return deque_base_call(self, "remove", value);
}

static PyObject *TracedDeque_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    TracedDequeExtra *x = td_extra(self);
    PyObject *r = deque_base_call(self, "clear", NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    x->arw_count = 0;
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *copy = deque_base_call(self, "copy", NULL);
    if (!copy) return NULL;
    PyObject *t_args = PyTuple_Pack(1, copy);
    Py_DECREF(copy);
    if (!t_args) return NULL;
    PyObject *result = PyTuple_Pack(2, (PyObject *)deque_type_obj, t_args);
    Py_DECREF(t_args);
    return result;
}

static PyObject *TracedDeque_get_wrapped(PyObject *self, void *closure) {
    Py_RETURN_TRUE;
}

static PyMethodDef TracedDeque_methods[] = {
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
    {Py_mp_subscript, TracedDeque_subscript},
    {Py_mp_ass_subscript, TracedDeque_ass_sub},
    {0, NULL}
};

static PyType_Spec TracedDeque_spec = {
    .name = "tracer._tracer.TracedDeque",
    .basicsize = 0, /* filled at init */
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedDeque_slots,
};

/* ======================================================================== */
/* wrap_container                                                            */
/* ======================================================================== */

PyObject *wrap_container_inner(PyObject *value, PyObject *db, int obj_idx) {
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

static PyObject *py_wrap_container(PyObject *self, PyObject *args) {
    PyObject *value, *db;
    int owner_idx;
    if (!PyArg_ParseTuple(args, "OOi", &value, &db, &owner_idx))
        return NULL;
    return wrap_container_inner(value, db, owner_idx);
}

int containers_init(PyObject *module) {
    PyObject *dict_bases = PyTuple_Pack(1, (PyObject *)&PyDict_Type);
    if (!dict_bases) return -1;
    TracedDictType = (PyTypeObject *)PyType_FromSpecWithBases(
        &TracedDict_spec, dict_bases);
    Py_DECREF(dict_bases);
    if (!TracedDictType) return -1;
    if (PyModule_AddObject(module, "TracedDict",
                           (PyObject *)TracedDictType) < 0) {
        Py_DECREF(TracedDictType);
        return -1;
    }

    PyObject *list_bases = PyTuple_Pack(1, (PyObject *)&PyList_Type);
    if (!list_bases) return -1;
    TracedListType = (PyTypeObject *)PyType_FromSpecWithBases(
        &TracedList_spec, list_bases);
    Py_DECREF(list_bases);
    if (!TracedListType) return -1;
    if (PyModule_AddObject(module, "TracedList",
                           (PyObject *)TracedListType) < 0) {
        Py_DECREF(TracedListType);
        return -1;
    }

    PyObject *collections = PyImport_ImportModule("collections");
    if (!collections) return -1;
    deque_type_obj = (PyTypeObject *)PyObject_GetAttrString(collections, "deque");
    Py_DECREF(collections);
    if (!deque_type_obj) return -1;
    deque_basicsize = deque_type_obj->tp_basicsize;
    TracedDeque_spec.basicsize = (int)(deque_basicsize + sizeof(TracedDequeExtra));

    PyObject *deque_bases = PyTuple_Pack(1, (PyObject *)deque_type_obj);
    if (!deque_bases) return -1;
    TracedDequeType = (PyTypeObject *)PyType_FromSpecWithBases(
        &TracedDeque_spec, deque_bases);
    Py_DECREF(deque_bases);
    if (!TracedDequeType) return -1;
    if (PyModule_AddObject(module, "TracedDeque",
                           (PyObject *)TracedDequeType) < 0) {
        Py_DECREF(TracedDequeType);
        return -1;
    }

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
