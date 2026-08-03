#include "containers.h"
#include <stdlib.h>
#include <string.h>

extern ObjectTraceData *get_trace_data(PyObject *obj);

/* ======================================================================== */
/* ARWSet — open-addressing hash table keyed by Py_hash_t                    */
/* ======================================================================== */

#define USABLE_FRACTION(n) (((n) << 1) / 3)

static size_t
arwset_lookup(const ARWSet *s, Py_hash_t hash)
{
    size_t i = (size_t)hash & s->mask;
    size_t perturb = (size_t)hash;

    for (;;) {
        ARWSetEntry *entry = &s->table[i];
        if (entry->hash == ARWSET_EMPTY)
            return i;
        if (entry->hash == hash)
            return i;
        if (entry->hash == ARWSET_DUMMY) {
            perturb >>= 5;
            i = (5 * i + perturb + 1) & s->mask;
            continue;
        }
        perturb >>= 5;
        i = (5 * i + perturb + 1) & s->mask;
    }
}

static int arwset_resize(ARWSet *s, size_t minused);

void
arwset_init(ARWSet *s)
{
    memset(s->smalltable, 0, sizeof(s->smalltable));
    s->table = s->smalltable;
    s->mask = ARWSET_MINSIZE - 1;
    s->fill = 0;
    s->used = 0;
    s->finger = 0;
}

void
arwset_free(ARWSet *s)
{
    if (s->table != s->smalltable)
        free(s->table);
    s->table = NULL;
    s->mask = 0;
    s->fill = 0;
    s->used = 0;
}

int
arwset_add(ARWSet *s, Py_hash_t hash, ARW value)
{
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];

    if (entry->hash == hash) {
        entry->value = value;
        return 0;
    }

    if (entry->hash == ARWSET_EMPTY)
        s->fill++;
    entry->hash = hash;
    entry->value = value;
    s->used++;

    if (s->fill * 3 > (s->mask + 1) * 2) {
        if (arwset_resize(s, s->used > 50000 ? s->used * 2 : s->used * 4) < 0)
            return -1;
    }
    return 0;
}

int
arwset_get(const ARWSet *s, Py_hash_t hash, ARW *out)
{
    if (s->used == 0)
        return -1;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];
    if (entry->hash != hash)
        return -1;
    *out = entry->value;
    return 0;
}

int
arwset_del(ARWSet *s, Py_hash_t hash)
{
    if (s->used == 0)
        return -1;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];
    if (entry->hash != hash)
        return -1;
    entry->hash = ARWSET_DUMMY;
    s->used--;
    return 0;
}

int
arwset_contains(const ARWSet *s, Py_hash_t hash)
{
    if (s->used == 0)
        return 0;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;
    size_t slot = arwset_lookup(s, hash);
    return s->table[slot].hash == hash;
}

int
arwset_pop(ARWSet *s, ARW *out)
{
    if (s->used == 0)
        return -1;
    size_t i = s->finger;
    while (i <= s->mask) {
        if (s->table[i].hash != ARWSET_EMPTY &&
            s->table[i].hash != ARWSET_DUMMY) {
            *out = s->table[i].value;
            s->table[i].hash = ARWSET_DUMMY;
            s->used--;
            s->finger = i + 1;
            return 0;
        }
        i++;
    }
    i = 0;
    while (i < s->finger) {
        if (s->table[i].hash != ARWSET_EMPTY &&
            s->table[i].hash != ARWSET_DUMMY) {
            *out = s->table[i].value;
            s->table[i].hash = ARWSET_DUMMY;
            s->used--;
            s->finger = i + 1;
            return 0;
        }
        i++;
    }
    return -1;
}

size_t
arwset_len(const ARWSet *s)
{
    return s->used;
}

static int
arwset_resize(ARWSet *s, size_t minused)
{
    size_t newsize = ARWSET_MINSIZE;
    while (newsize <= minused)
        newsize <<= 1;

    ARWSetEntry *oldtable = s->table;
    size_t oldmask = s->mask;

    ARWSetEntry *newtable;
    if (newsize == ARWSET_MINSIZE && oldtable != s->smalltable) {
        newtable = s->smalltable;
    } else {
        newtable = malloc(newsize * sizeof(ARWSetEntry));
        if (!newtable)
            return -1;
    }
    memset(newtable, 0, newsize * sizeof(ARWSetEntry));

    s->table = newtable;
    s->mask = newsize - 1;
    s->fill = s->used;
    s->finger = 0;

    for (size_t i = 0; i <= oldmask; i++) {
        if (oldtable[i].hash != ARWSET_EMPTY &&
            oldtable[i].hash != ARWSET_DUMMY) {
            size_t j = (size_t)oldtable[i].hash & s->mask;
            while (newtable[j].hash != ARWSET_EMPTY)
                j = (j + 1) & s->mask;
            newtable[j] = oldtable[i];
        }
    }

    if (oldtable != s->smalltable)
        free(oldtable);
    return 0;
}

/* ======================================================================== */
/* TracedSet  (subclasses set)                                               */
/* ======================================================================== */

PyTypeObject *TracedSetType = NULL;

static inline SetTraceData *get_set_trace(PyObject *self) {
    return (SetTraceData *)get_trace_data(self);
}

static int TracedSet_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedSetObject *o = (TracedSetObject *)self;
    static char *kwlist[] = {"source", "db", "owner_idx", NULL};
    PyObject *source, *db;
    int owner_idx;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOi", kwlist,
            &source, &db, &owner_idx))
        return -1;

    PyObject *init_args = PyTuple_Pack(1, source);
    if (!init_args) return -1;
    if (PySet_Type.tp_init(self, init_args, NULL) < 0) {
        Py_DECREF(init_args);
        return -1;
    }
    Py_DECREF(init_args);

    Py_INCREF(db); o->db = db;

    SetTraceData *td = malloc(sizeof(SetTraceData));
    if (!td) return -1;
    td->base.id = (uint64_t)owner_idx;
    ARWMap_init(&td->base.attrs, 0);
    td->base.type = CONTAINER_SET;
    arwset_init(&td->arws);

    PyObject *iter = PyObject_GetIter(self);
    if (iter) {
        ARW zero = {0, 0};
        PyObject *item;
        while ((item = PyIter_Next(iter)) != NULL) {
            Py_hash_t h = PyObject_Hash(item);
            Py_DECREF(item);
            if (h == -1) { PyErr_Clear(); continue; }
            arwset_add(&td->arws, h, zero);
        }
        Py_DECREF(iter);
    }

    umap_set(&g_state.object_extras, (uintptr_t)self, (intptr_t)td);
    return 0;
}

static void TracedSet_dealloc(PyObject *self) {
    TracedSetObject *o = (TracedSetObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->db);
    PySet_Type.tp_dealloc(self);
}

static int TracedSet_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedSetObject *o = (TracedSetObject *)self;
    Py_VISIT(o->db);
    return PySet_Type.tp_traverse(self, visit, arg);
}

static int TracedSet_clear_gc(PyObject *self) {
    TracedSetObject *o = (TracedSetObject *)self;
    Py_CLEAR(o->db);
    return PySet_Type.tp_clear(self);
}

static Py_ssize_t TracedSet_len(PyObject *self) {
    return PySet_GET_SIZE(self);
}

static PyObject *TracedSet_repr(PyObject *self) {
    PyObject *r = PySet_Type.tp_repr(self);
    if (!r) return NULL;
    PyObject *result = PyUnicode_FromFormat("TracedSet(%U)", r);
    Py_DECREF(r);
    return result;
}

static int TracedSet_contains(PyObject *self, PyObject *key) {
    int result = PySet_Contains(self, key);
    if (result > 0) {
        SetTraceData *td = get_set_trace(self);
        if (td) {
            Py_hash_t h = PyObject_Hash(key);
            if (h != -1) {
                ARW arw;
                if (arwset_get(&td->arws, h, &arw) == 0)
                    emit_read(&arw);
            } else {
                PyErr_Clear();
            }
        }
    }
    return result;
}

static PyObject *TracedSet_add(PyObject *self, PyObject *value) {
    Py_hash_t h = PyObject_Hash(value);
    if (h == -1) return NULL;
    if (PySet_Add(self, value) < 0) return NULL;
    SetTraceData *td = get_set_trace(self);
    if (td)
        arwset_add(&td->arws, h, caller_arw());
    Py_RETURN_NONE;
}

static PyObject *TracedSet_discard(PyObject *self, PyObject *value) {
    Py_hash_t h = PyObject_Hash(value);
    if (h == -1) return NULL;
    int contained = PySet_Contains(self, value);
    if (contained < 0) return NULL;
    if (contained) {
        if (PySet_Discard(self, value) < 0) return NULL;
        SetTraceData *td = get_set_trace(self);
        if (td)
            arwset_del(&td->arws, h);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedSet_remove(PyObject *self, PyObject *value) {
    Py_hash_t h = PyObject_Hash(value);
    if (h == -1) return NULL;
    int contained = PySet_Contains(self, value);
    if (contained < 0) return NULL;
    if (!contained) {
        PyErr_SetObject(PyExc_KeyError, value);
        return NULL;
    }
    if (PySet_Discard(self, value) < 0) return NULL;
    SetTraceData *td = get_set_trace(self);
    if (td)
        arwset_del(&td->arws, h);
    Py_RETURN_NONE;
}

static PyObject *TracedSet_pop(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *meth = PyObject_GetAttrString((PyObject *)&PySet_Type, "pop");
    if (!meth) return NULL;
    PyObject *result = PyObject_CallFunctionObjArgs(meth, self, NULL);
    Py_DECREF(meth);
    if (!result) return NULL;

    SetTraceData *td = get_set_trace(self);
    if (td) {
        Py_hash_t h = PyObject_Hash(result);
        if (h != -1) {
            ARW arw;
            if (arwset_get(&td->arws, h, &arw) == 0)
                emit_read(&arw);
            arwset_del(&td->arws, h);
        } else {
            PyErr_Clear();
        }
    }
    return result;
}

static PyObject *TracedSet_update(PyObject *self, PyObject *other) {
    ARW arw = caller_arw();
    PyObject *iter = PyObject_GetIter(other);
    if (!iter) return NULL;
    PyObject *item;
    while ((item = PyIter_Next(iter)) != NULL) {
        Py_hash_t h = PyObject_Hash(item);
        if (h == -1) {
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }
        if (PySet_Add(self, item) < 0) {
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }
        SetTraceData *td = get_set_trace(self);
        if (td && !arwset_contains(&td->arws, h))
            arwset_add(&td->arws, h, arw);
        Py_DECREF(item);
    }
    Py_DECREF(iter);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *TracedSet_clear(PyObject *self, PyObject *Py_UNUSED(args)) {
    if (PySet_Clear(self) < 0) return NULL;
    SetTraceData *td = get_set_trace(self);
    if (td) {
        arwset_free(&td->arws);
        arwset_init(&td->arws);
    }
    Py_RETURN_NONE;
}

static PyObject *TracedSet_copy(PyObject *self, PyObject *Py_UNUSED(args)) {
    return PySet_New(self);
}

static PyObject *TracedSet_reduce(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *builtins = PyImport_ImportModule("builtins");
    if (!builtins) return NULL;
    PyObject *set_type = PyObject_GetAttrString(builtins, "set");
    Py_DECREF(builtins);
    if (!set_type) return NULL;
    PyObject *as_list = PySequence_List(self);
    if (!as_list) { Py_DECREF(set_type); return NULL; }
    PyObject *t_args = PyTuple_Pack(1, as_list);
    Py_DECREF(as_list);
    if (!t_args) { Py_DECREF(set_type); return NULL; }
    PyObject *result = PyTuple_Pack(2, set_type, t_args);
    Py_DECREF(set_type); Py_DECREF(t_args);
    return result;
}

static PyObject *TracedSet_iter(PyObject *self) {
    return PySet_Type.tp_iter(self);
}

static PyMethodDef TracedSet_methods[] = {
    {"add",        TracedSet_add,     METH_O, NULL},
    {"discard",    TracedSet_discard, METH_O, NULL},
    {"remove",     TracedSet_remove,  METH_O, NULL},
    {"pop",        TracedSet_pop,     METH_NOARGS, NULL},
    {"update",     TracedSet_update,  METH_O, NULL},
    {"clear",      TracedSet_clear,   METH_NOARGS, NULL},
    {"copy",       TracedSet_copy,    METH_NOARGS, NULL},
    {"__reduce__", TracedSet_reduce,  METH_NOARGS, NULL},
    {NULL}
};

static PyType_Slot TracedSet_slots[] = {
    {Py_tp_init,      TracedSet_init},
    {Py_tp_dealloc,   TracedSet_dealloc},
    {Py_tp_traverse,  TracedSet_traverse},
    {Py_tp_clear,     TracedSet_clear_gc},
    {Py_tp_repr,      TracedSet_repr},
    {Py_tp_iter,      TracedSet_iter},
    {Py_tp_methods,   TracedSet_methods},
    {Py_sq_length,    TracedSet_len},
    {Py_sq_contains,  TracedSet_contains},
    {0, NULL}
};

static PyType_Spec TracedSet_spec = {
    .name = "tracer._tracer.TracedSet",
    .basicsize = sizeof(TracedSetObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedSet_slots,
};

int traced_set_init(PyObject *module) {
    PyObject *set_bases = PyTuple_Pack(1, (PyObject *)&PySet_Type);
    if (!set_bases) return -1;
    TracedSetType = (PyTypeObject *)PyType_FromSpecWithBases(
        &TracedSet_spec, set_bases);
    Py_DECREF(set_bases);
    if (!TracedSetType) return -1;
    if (PyModule_AddObject(module, "TracedSet",
                           (PyObject *)TracedSetType) < 0) {
        Py_DECREF(TracedSetType);
        return -1;
    }
    return 0;
}
