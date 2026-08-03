#include "containers.h"
#include "internal/pycore_frame.h"
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* ARWList — dynamic array with CPython's resize algorithm                   */
/* ======================================================================== */

static int
arwlist_resize(ARWList *l, size_t newsize)
{
    size_t new_allocated;
    ARW *items;

    if (l->allocated >= newsize && newsize >= (l->allocated >> 1)) {
        l->len = newsize;
        return 0;
    }

    new_allocated = ((size_t)newsize + (newsize >> 3) + 6) & ~(size_t)3;

    if (newsize - l->len > new_allocated - newsize)
        new_allocated = ((size_t)newsize + 3) & ~(size_t)3;

    if (newsize == 0)
        new_allocated = 0;

    items = realloc(l->items, new_allocated * sizeof(ARW));
    if (items == NULL && new_allocated != 0)
        return -1;

    l->items = items;
    l->len = newsize;
    l->allocated = new_allocated;
    return 0;
}

void
arwlist_init(ARWList *l)
{
    l->items = NULL;
    l->len = 0;
    l->allocated = 0;
}

void
arwlist_free(ARWList *l)
{
    free(l->items);
    l->items = NULL;
    l->len = 0;
    l->allocated = 0;
}

int
arwlist_append(ARWList *l, ARW value)
{
    if (arwlist_resize(l, l->len + 1) < 0)
        return -1;
    l->items[l->len - 1] = value;
    return 0;
}

int
arwlist_get(const ARWList *l, size_t index, ARW *out)
{
    if (index >= l->len)
        return -1;
    *out = l->items[index];
    return 0;
}

int
arwlist_set(ARWList *l, size_t index, ARW value)
{
    if (index >= l->len)
        return -1;
    l->items[index] = value;
    return 0;
}

int
arwlist_insert(ARWList *l, size_t index, ARW value)
{
    if (index > l->len)
        return -1;
    if (arwlist_resize(l, l->len + 1) < 0)
        return -1;
    if (index < l->len - 1)
        memmove(&l->items[index + 1], &l->items[index],
                (l->len - 1 - index) * sizeof(ARW));
    l->items[index] = value;
    return 0;
}

ARW
arwlist_pop(ARWList *l)
{
    ARW value = l->items[l->len - 1];
    arwlist_resize(l, l->len - 1);
    return value;
}

size_t
arwlist_len(const ARWList *l)
{
    return l->len;
}

/* ======================================================================== */
/* TracedList  (subclasses list)                                             */
/* ======================================================================== */

PyTypeObject *TracedListType = NULL;

static inline ListTraceData *get_list_trace(PyObject *self) {
    return (ListTraceData *)PyObject_GetExtra(self);
}

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

    ListTraceData *td = malloc(sizeof(ListTraceData));
    if (!td) return -1;
    td->base.id = (uint64_t)owner_idx;
    ARWMap_init(&td->base.attrs, 0);
    td->base.type = CONTAINER_LIST;
    arwlist_init(&td->arws);

    Py_ssize_t n = PyList_GET_SIZE(self);
    ARW zero = {0, 0};
    for (Py_ssize_t i = 0; i < n; i++)
        arwlist_append(&td->arws, zero);

    PyObject_SetExtra(self, td);
    return 0;
}

static void TracedList_dealloc(PyObject *self) {
    TracedListObject *o = (TracedListObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->db);
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
        ListTraceData *td = get_list_trace(self);
        if (td) {
            ARW arw;
            if (arwlist_get(&td->arws, (size_t)idx, &arw) == 0)
                emit_read(&arw);
        }
        Py_INCREF(val);
        return val;
    }
    return PyList_Type.tp_as_mapping->mp_subscript(self, key);
}

static int TracedList_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    if (!PyLong_Check(key))
        return PyList_Type.tp_as_mapping->mp_ass_subscript(self, key, value);
    Py_ssize_t i = PyLong_AsSsize_t(key);
    if (i == -1 && PyErr_Occurred()) return -1;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = i < 0 ? len + i : i;
    if (idx < 0 || idx >= len) {
        PyErr_SetString(PyExc_IndexError,
                        "list assignment index out of range");
        return -1;
    }
    ListTraceData *td = get_list_trace(self);
    if (value == NULL) {
        if (td && (size_t)idx < td->arws.len) {
            memmove(&td->arws.items[idx], &td->arws.items[idx + 1],
                    (td->arws.len - (size_t)idx - 1) * sizeof(ARW));
            td->arws.len--;
        }
        return PyList_SetSlice(self, idx, idx + 1, NULL);
    }
    Py_INCREF(value);
    if (PyList_SetItem(self, idx, value) < 0) return -1;
    if (td)
        arwlist_set(&td->arws, (size_t)idx, caller_arw());
    return 0;
}

static PyObject *TracedList_append(PyObject *self, PyObject *value) {
    if (PyList_Append(self, value) < 0) return NULL;
    ListTraceData *td = get_list_trace(self);
    if (td)
        arwlist_append(&td->arws, caller_arw());
    Py_RETURN_NONE;
}

static PyObject *TracedList_extend(PyObject *self, PyObject *values) {
    ARW arw = caller_arw();
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

    ListTraceData *td = get_list_trace(self);
    if (td) {
        Py_ssize_t new_len = PyList_GET_SIZE(self);
        for (Py_ssize_t i = start; i < new_len; i++)
            arwlist_append(&td->arws, arw);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedList_insert(PyObject *self, PyObject *args) {
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "nO", &index, &value)) return NULL;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = index < 0 ? (len + 1 + index > 0 ? len + 1 + index : 0)
                                : (index < len ? index : len);
    if (PyList_Insert(self, idx, value) < 0) return NULL;
    ListTraceData *td = get_list_trace(self);
    if (td)
        arwlist_insert(&td->arws, (size_t)idx, caller_arw());
    Py_RETURN_NONE;
}

static PyObject *TracedList_pop(PyObject *self, PyObject *args) {
    Py_ssize_t index = -1;
    if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
    Py_ssize_t len = PyList_GET_SIZE(self);
    Py_ssize_t idx = index < 0 ? len + index : index;
    if (idx < 0 || idx >= len) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return NULL;
    }
    ListTraceData *td = get_list_trace(self);
    if (td && (size_t)idx < td->arws.len) {
        ARW arw;
        if (arwlist_get(&td->arws, (size_t)idx, &arw) == 0)
            emit_read(&arw);
        memmove(&td->arws.items[idx], &td->arws.items[idx + 1],
                (td->arws.len - (size_t)idx - 1) * sizeof(ARW));
        td->arws.len--;
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
    ListTraceData *td = get_list_trace(self);
    if (td && (size_t)idx < td->arws.len) {
        memmove(&td->arws.items[idx], &td->arws.items[idx + 1],
                (td->arws.len - (size_t)idx - 1) * sizeof(ARW));
        td->arws.len--;
    }
    if (PyList_SetSlice(self, idx, idx + 1, NULL) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *TracedList_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    if (PyList_SetSlice(self, 0, PyList_GET_SIZE(self), NULL) < 0) return NULL;
    ListTraceData *td = get_list_trace(self);
    if (td) {
        arwlist_free(&td->arws);
        arwlist_init(&td->arws);
    }
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

static PyType_Slot TracedList_slots[] = {
    {Py_tp_init,      TracedList_init},
    {Py_tp_dealloc,   TracedList_dealloc},
    {Py_tp_traverse,  TracedList_traverse},
    {Py_tp_clear,     TracedList_clear_gc},
    {Py_tp_repr,      TracedList_repr},
    {Py_tp_methods,   TracedList_methods},
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

int traced_list_init(PyObject *module) {
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
    return 0;
}
