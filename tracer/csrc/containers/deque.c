#include "containers.h"
#include <stdlib.h>
#include <assert.h>

extern ObjectTraceData *get_trace_data(PyObject *obj);

/* ======================================================================== */
/* ARWDeque — doubly-linked list of fixed-size blocks                        */
/* ======================================================================== */

static ARWBlock *
arwdeque_newblock(ARWDeque *d)
{
    if (d->numfreeblocks) {
        d->numfreeblocks--;
        return d->freeblocks[d->numfreeblocks];
    }
    return malloc(sizeof(ARWBlock));
}

static void
arwdeque_freeblock(ARWDeque *d, ARWBlock *b)
{
    if (d->numfreeblocks < ARW_MAX_FREEBLOCKS) {
        d->freeblocks[d->numfreeblocks] = b;
        d->numfreeblocks++;
    } else {
        free(b);
    }
}

void
arwdeque_init(ARWDeque *d)
{
    ARWBlock *b = malloc(sizeof(ARWBlock));
    b->leftlink = NULL;
    b->rightlink = NULL;
    d->leftblock = b;
    d->rightblock = b;
    d->leftindex = ARW_BLOCK_CENTER + 1;
    d->rightindex = ARW_BLOCK_CENTER;
    d->len = 0;
    d->numfreeblocks = 0;
}

void
arwdeque_free(ARWDeque *d)
{
    ARWBlock *b = d->leftblock;
    while (b) {
        ARWBlock *next = b->rightlink;
        free(b);
        b = next;
    }
    for (size_t i = 0; i < d->numfreeblocks; i++)
        free(d->freeblocks[i]);
    d->leftblock = NULL;
    d->rightblock = NULL;
    d->len = 0;
    d->numfreeblocks = 0;
}

int
arwdeque_append(ARWDeque *d, ARW value)
{
    if (d->rightindex == ARW_BLOCKLEN - 1) {
        ARWBlock *b = arwdeque_newblock(d);
        if (b == NULL)
            return -1;
        b->leftlink = d->rightblock;
        d->rightblock->rightlink = b;
        d->rightblock = b;
        b->rightlink = NULL;
        d->rightindex = (size_t)-1;
    }
    d->len++;
    d->rightindex++;
    d->rightblock->data[d->rightindex] = value;
    return 0;
}

int
arwdeque_appendleft(ARWDeque *d, ARW value)
{
    if (d->leftindex == 0) {
        ARWBlock *b = arwdeque_newblock(d);
        if (b == NULL)
            return -1;
        b->rightlink = d->leftblock;
        d->leftblock->leftlink = b;
        d->leftblock = b;
        b->leftlink = NULL;
        d->leftindex = ARW_BLOCKLEN;
    }
    d->len++;
    d->leftindex--;
    d->leftblock->data[d->leftindex] = value;
    return 0;
}

int
arwdeque_pop(ARWDeque *d, ARW *out)
{
    if (d->len == 0)
        return -1;

    *out = d->rightblock->data[d->rightindex];
    d->rightindex--;
    d->len--;

    if (d->rightindex == (size_t)-1) {
        if (d->len) {
            ARWBlock *prev = d->rightblock->leftlink;
            arwdeque_freeblock(d, d->rightblock);
            prev->rightlink = NULL;
            d->rightblock = prev;
            d->rightindex = ARW_BLOCKLEN - 1;
        } else {
            d->leftindex = ARW_BLOCK_CENTER + 1;
            d->rightindex = ARW_BLOCK_CENTER;
        }
    }
    return 0;
}

int
arwdeque_popleft(ARWDeque *d, ARW *out)
{
    if (d->len == 0)
        return -1;

    *out = d->leftblock->data[d->leftindex];
    d->leftindex++;
    d->len--;

    if (d->leftindex == ARW_BLOCKLEN) {
        if (d->len) {
            ARWBlock *next = d->leftblock->rightlink;
            arwdeque_freeblock(d, d->leftblock);
            next->leftlink = NULL;
            d->leftblock = next;
            d->leftindex = 0;
        } else {
            d->leftindex = ARW_BLOCK_CENTER + 1;
            d->rightindex = ARW_BLOCK_CENTER;
        }
    }
    return 0;
}

int
arwdeque_get(const ARWDeque *d, size_t index, ARW *out)
{
    if (index >= d->len)
        return -1;

    size_t real = d->leftindex + index;
    const ARWBlock *b = d->leftblock;
    while (real >= ARW_BLOCKLEN) {
        b = b->rightlink;
        real -= ARW_BLOCKLEN;
    }
    *out = b->data[real];
    return 0;
}

size_t
arwdeque_len(const ARWDeque *d)
{
    return d->len;
}

/* ======================================================================== */
/* TracedDeque  (subclasses collections.deque)                               */
/* ======================================================================== */

PyTypeObject *TracedDequeType = NULL;
static PyTypeObject *deque_type_obj = NULL;

static inline DequeTraceData *get_deque_trace(PyObject *self) {
    return (DequeTraceData *)get_trace_data(self);
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

    DequeTraceData *td = malloc(sizeof(DequeTraceData));
    if (!td) return -1;
    td->base.id = (uint64_t)owner_idx;
    ARWMap_init(&td->base.attrs, 0);
    td->base.type = CONTAINER_DEQUE;
    arwdeque_init(&td->arws);

    Py_ssize_t n = base_deque_len(self);
    if (n < 0) { PyErr_Clear(); n = 0; }
    ARW zero = {0, 0};
    for (Py_ssize_t i = 0; i < n; i++)
        arwdeque_append(&td->arws, zero);

    umap_set(&g_state.object_extras, (uintptr_t)self, (intptr_t)td);
    return 0;
}

static void TracedDeque_dealloc(PyObject *self) {
    PyObject_GC_UnTrack(self);
    deque_type_obj->tp_dealloc(self);
}

static int TracedDeque_traverse(PyObject *self, visitproc visit, void *arg) {
    return deque_type_obj->tp_traverse(self, visit, arg);
}

static int TracedDeque_clear_gc(PyObject *self) {
    return deque_type_obj->tp_clear(self);
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
    PyObject *result = deque_type_obj->tp_as_mapping->mp_subscript(self, key);
    if (!result) return NULL;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (!(i == -1 && PyErr_Occurred())) {
            DequeTraceData *td = get_deque_trace(self);
            if (td) {
                Py_ssize_t len = (Py_ssize_t)td->arws.len;
                Py_ssize_t idx = i < 0 ? len + i : i;
                if (idx >= 0 && idx < len) {
                    ARW arw;
                    if (arwdeque_get(&td->arws, (size_t)idx, &arw) == 0)
                        emit_read(&arw);
                }
            }
        } else {
            PyErr_Clear();
        }
    }
    return result;
}

static int TracedDeque_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    if (value == NULL)
        return deque_type_obj->tp_as_mapping->mp_ass_subscript(self, key, NULL);
    if (deque_type_obj->tp_as_mapping->mp_ass_subscript(self, key, value) < 0)
        return -1;
    if (PyLong_Check(key)) {
        Py_ssize_t i = PyLong_AsSsize_t(key);
        if (!(i == -1 && PyErr_Occurred())) {
            DequeTraceData *td = get_deque_trace(self);
            if (td) {
                Py_ssize_t len = (Py_ssize_t)td->arws.len;
                Py_ssize_t idx = i < 0 ? len + i : i;
                if (idx >= 0 && idx < len)
                    td->arws.leftblock->data[td->arws.leftindex + (size_t)idx] = caller_arw();
            }
        } else {
            PyErr_Clear();
        }
    }
    return 0;
}

static PyObject *TracedDeque_append(PyObject *self, PyObject *value) {
    PyObject *r = deque_base_call(self, "append", value);
    if (!r) return NULL;
    Py_DECREF(r);
    DequeTraceData *td = get_deque_trace(self);
    if (td)
        arwdeque_append(&td->arws, caller_arw());
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_appendleft(PyObject *self, PyObject *value) {
    PyObject *r = deque_base_call(self, "appendleft", value);
    if (!r) return NULL;
    Py_DECREF(r);
    DequeTraceData *td = get_deque_trace(self);
    if (td)
        arwdeque_appendleft(&td->arws, caller_arw());
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extend(PyObject *self, PyObject *values) {
    ARW arw = caller_arw();
    Py_ssize_t start = base_deque_len(self);
    PyObject *r = deque_base_call(self, "extend", values);
    if (!r) return NULL;
    Py_DECREF(r);
    DequeTraceData *td = get_deque_trace(self);
    if (td) {
        Py_ssize_t new_len = base_deque_len(self);
        for (Py_ssize_t i = start; i < new_len; i++)
            arwdeque_append(&td->arws, arw);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_extendleft(PyObject *self, PyObject *values) {
    ARW arw = caller_arw();
    Py_ssize_t start = base_deque_len(self);
    PyObject *r = deque_base_call(self, "extendleft", values);
    if (!r) return NULL;
    Py_DECREF(r);
    DequeTraceData *td = get_deque_trace(self);
    if (td) {
        Py_ssize_t new_len = base_deque_len(self);
        Py_ssize_t added = new_len - start;
        for (Py_ssize_t i = 0; i < added; i++)
            arwdeque_appendleft(&td->arws, arw);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedDeque_pop(PyObject *self, PyObject *Py_UNUSED(args)) {
    DequeTraceData *td = get_deque_trace(self);
    if (td && td->arws.len > 0) {
        ARW arw;
        if (arwdeque_pop(&td->arws, &arw) == 0)
            emit_read(&arw);
    }
    return deque_base_call(self, "pop", NULL);
}

static PyObject *TracedDeque_popleft(PyObject *self, PyObject *Py_UNUSED(args)) {
    DequeTraceData *td = get_deque_trace(self);
    if (td && td->arws.len > 0) {
        ARW arw;
        if (arwdeque_popleft(&td->arws, &arw) == 0)
            emit_read(&arw);
    }
    return deque_base_call(self, "popleft", NULL);
}

static PyObject *TracedDeque_remove(PyObject *self, PyObject *value) {
    DequeTraceData *td = get_deque_trace(self);
    if (td) {
        PyObject *idx_obj = deque_base_call(self, "index", value);
        if (idx_obj) {
            Py_ssize_t idx = PyLong_AsSsize_t(idx_obj);
            Py_DECREF(idx_obj);
            if (idx >= 0 && (size_t)idx < td->arws.len) {
                ARWDeque tmp;
                arwdeque_init(&tmp);
                for (size_t i = 0; i < td->arws.len; i++) {
                    if ((Py_ssize_t)i == idx) continue;
                    ARW arw;
                    arwdeque_get(&td->arws, i, &arw);
                    arwdeque_append(&tmp, arw);
                }
                arwdeque_free(&td->arws);
                td->arws = tmp;
            }
        } else {
            PyErr_Clear();
        }
    }
    return deque_base_call(self, "remove", value);
}

static PyObject *TracedDeque_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *r = deque_base_call(self, "clear", NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    DequeTraceData *td = get_deque_trace(self);
    if (td) {
        arwdeque_free(&td->arws);
        arwdeque_init(&td->arws);
    }
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

static PyType_Slot TracedDeque_slots[] = {
    {Py_tp_init,     TracedDeque_init},
    {Py_tp_dealloc,  TracedDeque_dealloc},
    {Py_tp_traverse, TracedDeque_traverse},
    {Py_tp_clear,    TracedDeque_clear_gc},
    {Py_tp_repr,     TracedDeque_repr},
    {Py_tp_iter,     TracedDeque_iter},
    {Py_tp_methods,  TracedDeque_methods},
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

int traced_deque_init(PyObject *module) {
    PyObject *collections = PyImport_ImportModule("collections");
    if (!collections) return -1;
    deque_type_obj = (PyTypeObject *)PyObject_GetAttrString(collections, "deque");
    Py_DECREF(collections);
    if (!deque_type_obj) return -1;
    TracedDeque_spec.basicsize = deque_type_obj->tp_basicsize;

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
    return 0;
}
