#include "containers.h"
#include "internal/pycore_frame.h"
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* ARWDict — compact string-keyed hash table for ARW values                  */
/* ======================================================================== */

#define USABLE_FRACTION(n) (((n) << 1) / 3)

static size_t
arwdict_hash(const char *key)
{
    size_t h = 5381;
    for (const char *p = key; *p; p++)
        h = ((h << 5) + h) ^ (unsigned char)*p;
    return h;
}

static int8_t
calculate_log2_size(size_t minsize)
{
    int8_t log2 = 3;
    while (((size_t)1 << log2) < minsize)
        log2++;
    return log2;
}

static int arwdict_resize(ARWDict *d, size_t minsize);

void
arwdict_init(ARWDict *d)
{
    d->log2_size = 0;
    d->usable = 0;
    d->nentries = 0;
    d->used = 0;
    d->indices = NULL;
    d->entries = NULL;
}

void
arwdict_free(ARWDict *d)
{
    for (size_t i = 0; i < d->nentries; i++)
        free(d->entries[i].key);
    free(d->entries);
    free(d->indices);
    d->indices = NULL;
    d->entries = NULL;
    d->log2_size = 0;
    d->usable = 0;
    d->nentries = 0;
    d->used = 0;
}

static int
arwdict_ensure_alloc(ARWDict *d)
{
    if (d->indices != NULL)
        return 0;
    int8_t log2 = 3;
    size_t size = (size_t)1 << log2;
    d->indices = malloc(size * sizeof(int32_t));
    if (!d->indices)
        return -1;
    memset(d->indices, 0xff, size * sizeof(int32_t));
    d->entries = malloc(USABLE_FRACTION(size) * sizeof(ARWDictEntry));
    if (!d->entries) {
        free(d->indices);
        d->indices = NULL;
        return -1;
    }
    d->log2_size = log2;
    d->usable = USABLE_FRACTION(size);
    d->nentries = 0;
    d->used = 0;
    return 0;
}

static size_t
arwdict_lookup(const ARWDict *d, const char *key, size_t hash)
{
    size_t mask = ((size_t)1 << d->log2_size) - 1;
    size_t i = hash & mask;
    size_t perturb = hash;

    for (;;) {
        int32_t ix = d->indices[i];
        if (ix == ARWDICT_EMPTY)
            return i;
        if (ix != ARWDICT_DUMMY && strcmp(d->entries[ix].key, key) == 0)
            return i;
        perturb >>= 5;
        i = (5 * i + perturb + 1) & mask;
    }
}

int
arwdict_get(const ARWDict *d, const char *key, ARW *out)
{
    if (d->indices == NULL || d->used == 0)
        return -1;
    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];
    if (ix < 0)
        return -1;
    *out = d->entries[ix].value;
    return 0;
}

int
arwdict_set(ARWDict *d, const char *key, ARW value)
{
    if (arwdict_ensure_alloc(d) < 0)
        return -1;

    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];

    if (ix >= 0) {
        d->entries[ix].value = value;
        return 0;
    }

    if (d->usable == 0) {
        if (arwdict_resize(d, d->used * 3) < 0)
            return -1;
        slot = arwdict_lookup(d, key, hash);
    }

    char *dup = strdup(key);
    if (!dup)
        return -1;

    int32_t new_ix = (int32_t)d->nentries;
    d->indices[slot] = new_ix;
    d->entries[new_ix].key = dup;
    d->entries[new_ix].value = value;
    d->nentries++;
    d->usable--;
    d->used++;
    return 0;
}

int
arwdict_del(ARWDict *d, const char *key)
{
    if (d->indices == NULL || d->used == 0)
        return -1;

    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];
    if (ix < 0)
        return -1;

    d->indices[slot] = ARWDICT_DUMMY;
    free(d->entries[ix].key);
    d->entries[ix].key = NULL;
    d->used--;
    return 0;
}

static int
arwdict_resize(ARWDict *d, size_t minsize)
{
    if (minsize < ARWDICT_MINSIZE)
        minsize = ARWDICT_MINSIZE;
    int8_t log2 = calculate_log2_size(minsize);
    size_t newsize = (size_t)1 << log2;

    int32_t *new_indices = malloc(newsize * sizeof(int32_t));
    if (!new_indices)
        return -1;
    memset(new_indices, 0xff, newsize * sizeof(int32_t));

    size_t new_usable = USABLE_FRACTION(newsize);
    ARWDictEntry *new_entries = malloc(new_usable * sizeof(ARWDictEntry));
    if (!new_entries) {
        free(new_indices);
        return -1;
    }

    size_t new_nentries = 0;
    size_t mask = newsize - 1;
    for (size_t i = 0; i < d->nentries; i++) {
        if (d->entries[i].key == NULL)
            continue;
        size_t hash = arwdict_hash(d->entries[i].key);
        size_t slot = hash & mask;
        size_t perturb = hash;
        while (new_indices[slot] != ARWDICT_EMPTY) {
            perturb >>= 5;
            slot = (5 * slot + perturb + 1) & mask;
        }
        new_indices[slot] = (int32_t)new_nentries;
        new_entries[new_nentries] = d->entries[i];
        new_nentries++;
    }

    free(d->indices);
    free(d->entries);
    d->indices = new_indices;
    d->entries = new_entries;
    d->log2_size = log2;
    d->usable = new_usable - new_nentries;
    d->nentries = new_nentries;
    d->used = new_nentries;
    return 0;
}

int
arwdict_contains(const ARWDict *d, const char *key)
{
    if (d->indices == NULL || d->used == 0)
        return 0;
    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    return d->indices[slot] >= 0;
}

size_t
arwdict_len(const ARWDict *d)
{
    return d->used;
}

/* ======================================================================== */
/* TracedDict  (subclasses dict)                                             */
/* ======================================================================== */

PyTypeObject *TracedDictType = NULL;

static inline DictTraceData *get_dict_trace(PyObject *self) {
    return (DictTraceData *)PyObject_GetExtra(self);
}

static int TracedDict_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedDictObject *o = (TracedDictObject *)self;
    static char *kwlist[] = {"source", "db", "owner_idx", NULL};
    PyObject *source, *db;
    int owner_idx;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi", kwlist,
            &source, &db, &owner_idx))
        return -1;

    PyObject *init_args = PyTuple_Pack(1, source);
    if (!init_args) return -1;
    if (PyDict_Type.tp_init(self, init_args, NULL) < 0) {
        Py_DECREF(init_args);
        return -1;
    }
    Py_DECREF(init_args);

    Py_INCREF(db); o->db = db;

    DictTraceData *td = malloc(sizeof(DictTraceData));
    if (!td) return -1;
    td->base.id = (uint64_t)owner_idx;
    ARWMap_init(&td->base.attrs, 0);
    td->base.type = CONTAINER_DICT;
    arwdict_init(&td->arws);
    PyObject_SetExtra(self, td);
    return 0;
}

static void TracedDict_dealloc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->db);
    PyDict_Type.tp_dealloc(self);
}

static int TracedDict_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_VISIT(o->db);
    return PyDict_Type.tp_traverse(self, visit, arg);
}

static int TracedDict_clear_gc(PyObject *self) {
    TracedDictObject *o = (TracedDictObject *)self;
    Py_CLEAR(o->db);
    return PyDict_Type.tp_clear(self);
}

static Py_ssize_t TracedDict_len(PyObject *self) {
    return PyDict_Size(self);
}

static int TracedDict_ass_sub(PyObject *self, PyObject *key, PyObject *value) {
    if (value == NULL) {
        DictTraceData *td = get_dict_trace(self);
        if (td) {
            const char *ks = PyUnicode_AsUTF8(key);
            if (ks) arwdict_del(&td->arws, ks);
            else PyErr_Clear();
        }
        return PyDict_DelItem(self, key);
    }
    if (PyDict_SetItem(self, key, value) < 0) return -1;
    DictTraceData *td = get_dict_trace(self);
    if (td) {
        const char *ks = PyUnicode_AsUTF8(key);
        if (ks) arwdict_set(&td->arws, ks, caller_arw());
        else PyErr_Clear();
    }
    return 0;
}

static PyObject *TracedDict_subscript(PyObject *self, PyObject *key) {
    PyObject *val = PyDict_GetItemWithError(self, key);
    if (!val) {
        if (!PyErr_Occurred())
            PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    DictTraceData *td = get_dict_trace(self);
    if (td) {
        const char *ks = PyUnicode_AsUTF8(key);
        if (ks) {
            ARW arw;
            if (arwdict_get(&td->arws, ks, &arw) == 0)
                emit_read(&arw);
        } else {
            PyErr_Clear();
        }
    }
    Py_INCREF(val);
    return val;
}

static int TracedDict_contains(PyObject *self, PyObject *key) {
    return PyDict_Contains(self, key);
}

static PyObject *TracedDict_repr(PyObject *self) {
    PyObject *r = PyDict_Type.tp_repr(self);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedDict(%U)", r);
    Py_DECREF(r);
    return result;
}

static PyObject *TracedDict_get(PyObject *self, PyObject *args) {
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    PyObject *val = PyDict_GetItemWithError(self, key);
    if (val) {
        DictTraceData *td = get_dict_trace(self);
        if (td) {
            const char *ks = PyUnicode_AsUTF8(key);
            if (ks) {
                ARW arw;
                if (arwdict_get(&td->arws, ks, &arw) == 0)
                    emit_read(&arw);
            } else {
                PyErr_Clear();
            }
        }
        Py_INCREF(val);
        return val;
    }
    if (PyErr_Occurred()) return NULL;
    Py_INCREF(def);
    return def;
}

static PyObject *TracedDict_pop(PyObject *self, PyObject *args) {
    PyObject *key;
    PyObject *def = NULL;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;

    DictTraceData *td = get_dict_trace(self);
    if (td) {
        const char *ks = PyUnicode_AsUTF8(key);
        if (ks) {
            ARW arw;
            if (arwdict_get(&td->arws, ks, &arw) == 0)
                emit_read(&arw);
        } else {
            PyErr_Clear();
        }
    }

    PyObject *val = PyDict_GetItemWithError(self, key);
    if (val) {
        Py_INCREF(val);
        PyDict_DelItem(self, key);
        if (td) {
            const char *ks2 = PyUnicode_AsUTF8(key);
            if (ks2) arwdict_del(&td->arws, ks2);
            else PyErr_Clear();
        }
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
    PyObject *other = NULL;
    if (PyTuple_GET_SIZE(args) > 0)
        other = PyTuple_GET_ITEM(args, 0);
    if (other && PyDict_Check(other)) {
        if (PyDict_Merge(self, other, 1) < 0) return NULL;
    } else if (other) {
        if (PyDict_MergeFromSeq2(self, other, 1) < 0) return NULL;
    }
    if (kw && PyDict_Size(kw) > 0) {
        if (PyDict_Merge(self, kw, 1) < 0) return NULL;
    }

    DictTraceData *td = get_dict_trace(self);
    if (td) {
        ARW arw = caller_arw();
        PyObject *keys = PyDict_Keys(self);
        if (keys) {
            Py_ssize_t n = PyList_GET_SIZE(keys);
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject *k = PyList_GET_ITEM(keys, i);
                const char *ks = PyUnicode_AsUTF8(k);
                if (ks && !arwdict_contains(&td->arws, ks))
                    arwdict_set(&td->arws, ks, arw);
                if (!ks) PyErr_Clear();
            }
            Py_DECREF(keys);
        }
    }
    Py_RETURN_NONE;
}

static PyObject *TracedDict_setdefault(PyObject *self, PyObject *args) {
    PyObject *key, *def = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return NULL;
    if (PyDict_Contains(self, key))
        return TracedDict_subscript(self, key);
    TracedDict_ass_sub(self, key, def);
    Py_INCREF(def);
    return def;
}

static PyObject *TracedDict_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyDict_Clear(self);
    DictTraceData *td = get_dict_trace(self);
    if (td)
        arwdict_free(&td->arws);
    Py_RETURN_NONE;
}

static PyObject *TracedDict_keys(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Keys(self);
}

static PyObject *TracedDict_values(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Values(self);
}

static PyObject *TracedDict_items(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PyDict_Items(self);
}

static PyObject *TracedDict_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *dict_type = PyObject_GetAttrString(builtins, "dict");
    Py_DECREF(builtins);
    if (!dict_type) return NULL;
    PyObject *items = PyDict_Items(self);
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
    return PyDict_Copy(self);
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

static PyType_Slot TracedDict_slots[] = {
    {Py_tp_init,      TracedDict_init},
    {Py_tp_dealloc,   TracedDict_dealloc},
    {Py_tp_traverse,  TracedDict_traverse},
    {Py_tp_clear,     TracedDict_clear_gc},
    {Py_tp_repr,      TracedDict_repr},
    {Py_tp_methods,   TracedDict_methods},
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

int traced_dict_init(PyObject *module) {
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
    return 0;
}
