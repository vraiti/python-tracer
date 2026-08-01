#include "ownership.h"
#include "hook.h"
#include "internal/pycore_frame.h"
#include <string.h>

/* forward declaration from containers.c */
extern PyObject *wrap_container_inner(PyObject *value, PyObject *db,
                                      PyObject *trace_hook, int obj_idx,
                                      const char *attr_name);

/* forward declaration from hook.c — access frame stack peek */
extern PyObject *py_current_record(PyObject *self, PyObject *args);

/* ========== TracedSetattr ========== */

PyTypeObject *TracedSetattrType = NULL;

static int TracedSetattr_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedSetattrObject *o = (TracedSetattrObject *)self;
    static char *kwlist[] = {"original", "db", "trace_hook", NULL};
    PyObject *original, *db, *trace_hook;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO", kwlist,
            &original, &db, &trace_hook))
        return -1;
    Py_INCREF(original); o->original = original;
    Py_INCREF(db); o->db = db;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;
    return 0;
}

static void TracedSetattr_dealloc(PyObject *self) {
    TracedSetattrObject *o = (TracedSetattrObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->original);
    Py_XDECREF(o->db);
    Py_XDECREF(o->trace_hook);
    Py_TYPE(self)->tp_free(self);
}

static int TracedSetattr_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedSetattrObject *o = (TracedSetattrObject *)self;
    Py_VISIT(o->original);
    Py_VISIT(o->db);
    Py_VISIT(o->trace_hook);
    return 0;
}

static int TracedSetattr_clear(PyObject *self) {
    TracedSetattrObject *o = (TracedSetattrObject *)self;
    Py_CLEAR(o->original);
    Py_CLEAR(o->db);
    Py_CLEAR(o->trace_hook);
    return 0;
}

static PyObject *traced_setattr_call_impl(TracedSetattrObject *ts,
                                          PyObject *self_obj,
                                          PyObject *name,
                                          PyObject *value) {
    if (!g_state.enabled)
        return PyObject_CallFunctionObjArgs(ts->original, self_obj, name, value, NULL);

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return NULL;

    /* bypass for internal attrs */
    if (strncmp(name_str, "__tr_", 5) == 0 || strncmp(name_str, "__arw_", 6) == 0) {
        PyObject *builtins = PyImport_ImportModule("builtins");
        if (!builtins) return NULL;
        PyObject *object = PyObject_GetAttrString(builtins, "object");
        Py_DECREF(builtins);
        if (!object) return NULL;
        PyObject *r = PyObject_CallMethod(object, "__setattr__", "OOO",
                                          self_obj, name, value);
        Py_DECREF(object);
        if (!r) return NULL;
        Py_DECREF(r);
        Py_RETURN_NONE;
    }

    /* call original setattr */
    PyObject *res = PyObject_CallFunctionObjArgs(ts->original, self_obj, name, value, NULL);
    if (!res) return NULL;
    Py_DECREF(res);

    /* record the write */
    PyFrameObject *frame_ptr = PyEval_GetFrame();
    if (!frame_ptr) Py_RETURN_NONE;

    uint64_t caller_id = ((PyFrameObject *)frame_ptr)->call_id;
    int call_lineno = PyFrame_GetLineNumber(frame_ptr);

    PyObject *arw = PyObject_CallFunction((PyObject *)AttrRecordWriteType,
                                          "Ki", caller_id, call_lineno);
    if (!arw) { PyErr_Clear(); Py_RETURN_NONE; }

    /* store arw on the instance as __arw_<name> */
    char arw_key[256];
    snprintf(arw_key, sizeof(arw_key), "__arw_%s", name_str);
    PyObject *builtins = PyImport_ImportModule("builtins");
    PyObject *object_type = builtins ? PyObject_GetAttrString(builtins, "object") : NULL;
    Py_XDECREF(builtins);
    if (object_type) {
        PyObject *sa = PyObject_GetAttrString(object_type, "__setattr__");
        if (sa) {
            PyObject *arw_name = PyUnicode_FromString(arw_key);
            PyObject *r2 = PyObject_CallFunctionObjArgs(sa, self_obj, arw_name, arw, NULL);
            Py_XDECREF(r2);
            Py_DECREF(arw_name);
            Py_DECREF(sa);
        }
        Py_DECREF(object_type);
    }
    Py_DECREF(arw);

    /* track member relationships */
    PyObject *self_tr_idx = PyObject_GetAttrString(self_obj, "__tr_idx");
    if (self_tr_idx) {
        long obj_idx = PyLong_AsLong(self_tr_idx);
        Py_DECREF(self_tr_idx);
        if (!(obj_idx == -1 && PyErr_Occurred())) {
            PyObject *val_tr_idx = PyObject_GetAttrString(value, "__tr_idx");
            if (val_tr_idx) {
                long val_idx = PyLong_AsLong(val_tr_idx);
                Py_DECREF(val_tr_idx);
                if (!(val_idx == -1 && PyErr_Occurred())) {
                    DatabaseObject *db = (DatabaseObject *)ts->db;
                    PyObject *obj_rec = PyList_GetItem(db->objects, obj_idx);
                    if (obj_rec) {
                        ObjectRecordObject *orec = (ObjectRecordObject *)obj_rec;
                        PyObject *val_int = PyLong_FromLong(val_idx);
                        PyDict_SetItem(orec->members, name, val_int);
                        Py_DECREF(val_int);
                    }
                }
            } else {
                PyErr_Clear();
            }

            /* wrap containers */
            PyObject *value_type = (PyObject *)Py_TYPE(value);
            int is_dict = PyDict_Check(value);
            int is_list = PyList_Check(value);
            int is_deque = 0;
            if (!is_dict && !is_list) {
                PyObject *qn = PyObject_GetAttrString(value_type, "__qualname__");
                if (qn) {
                    const char *qns = PyUnicode_AsUTF8(qn);
                    if (qns && strcmp(qns, "deque") == 0) is_deque = 1;
                    Py_DECREF(qn);
                }
            }

            int is_wrapped = 0;
            PyObject *tw = PyObject_GetAttrString(value, "_tr_wrapped");
            if (tw) {
                is_wrapped = PyObject_IsTrue(tw);
                Py_DECREF(tw);
            } else {
                PyErr_Clear();
            }

            if ((is_dict || is_list || is_deque) && !is_wrapped) {
                PyObject *wrapped = wrap_container_inner(
                    value, ts->db, ts->trace_hook, (int)obj_idx, name_str);
                if (wrapped) {
                    if (object_type) {
                        /* object_type was already decref'd above, re-fetch */
                        PyObject *b2 = PyImport_ImportModule("builtins");
                        PyObject *ot2 = b2 ? PyObject_GetAttrString(b2, "object") : NULL;
                        Py_XDECREF(b2);
                        if (ot2) {
                            PyObject *sa2 = PyObject_GetAttrString(ot2, "__setattr__");
                            if (sa2) {
                                PyObject *r3 = PyObject_CallFunctionObjArgs(
                                    sa2, self_obj, name, wrapped, NULL);
                                Py_XDECREF(r3);
                                Py_DECREF(sa2);
                            }
                            Py_DECREF(ot2);
                        }
                    }
                    Py_DECREF(wrapped);
                } else {
                    PyErr_Clear();
                }
            }
        } else {
            PyErr_Clear();
        }
    } else {
        PyErr_Clear();
    }

    Py_RETURN_NONE;
}

static PyObject *TracedSetattr_descr_get(PyObject *self, PyObject *obj, PyObject *cls) {
    if (obj == Py_None || obj == NULL) {
        Py_INCREF(self);
        return self;
    }
    PyObject *bound = PyObject_CallFunction(
        (PyObject *)BoundSetattrType, "OO", self, obj);
    return bound;
}

static PyObject *TracedSetattr_call(PyObject *self, PyObject *args, PyObject *kw) {
    TracedSetattrObject *ts = (TracedSetattrObject *)self;
    PyObject *obj, *name, *value;
    if (!PyArg_ParseTuple(args, "OOO", &obj, &name, &value))
        return NULL;
    return traced_setattr_call_impl(ts, obj, name, value);
}

static PyType_Slot TracedSetattr_slots[] = {
    {Py_tp_init,     TracedSetattr_init},
    {Py_tp_dealloc,  TracedSetattr_dealloc},
    {Py_tp_traverse, TracedSetattr_traverse},
    {Py_tp_clear,    TracedSetattr_clear},
    {Py_tp_call,     TracedSetattr_call},
    {Py_tp_descr_get, TracedSetattr_descr_get},
    {0, NULL}
};

static PyType_Spec TracedSetattr_spec = {
    .name = "tracer._tracer.TracedSetattr",
    .basicsize = sizeof(TracedSetattrObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedSetattr_slots,
};

/* ========== BoundSetattr ========== */

PyTypeObject *BoundSetattrType = NULL;

static int BoundSetattr_init(PyObject *self, PyObject *args, PyObject *kw) {
    BoundSetattrObject *o = (BoundSetattrObject *)self;
    static char *kwlist[] = {"inner", "instance", NULL};
    PyObject *inner, *instance;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO", kwlist, &inner, &instance))
        return -1;
    Py_INCREF(inner); o->inner = inner;
    Py_INCREF(instance); o->instance = instance;
    return 0;
}

static void BoundSetattr_dealloc(PyObject *self) {
    BoundSetattrObject *o = (BoundSetattrObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->inner);
    Py_XDECREF(o->instance);
    Py_TYPE(self)->tp_free(self);
}

static int BoundSetattr_traverse(PyObject *self, visitproc visit, void *arg) {
    BoundSetattrObject *o = (BoundSetattrObject *)self;
    Py_VISIT(o->inner);
    Py_VISIT(o->instance);
    return 0;
}

static int BoundSetattr_clear(PyObject *self) {
    BoundSetattrObject *o = (BoundSetattrObject *)self;
    Py_CLEAR(o->inner);
    Py_CLEAR(o->instance);
    return 0;
}

static PyObject *BoundSetattr_call(PyObject *self, PyObject *args, PyObject *kw) {
    BoundSetattrObject *o = (BoundSetattrObject *)self;
    PyObject *name, *value;
    if (!PyArg_ParseTuple(args, "OO", &name, &value))
        return NULL;
    return traced_setattr_call_impl(
        (TracedSetattrObject *)o->inner, o->instance, name, value);
}

static PyType_Slot BoundSetattr_slots[] = {
    {Py_tp_init,     BoundSetattr_init},
    {Py_tp_dealloc,  BoundSetattr_dealloc},
    {Py_tp_traverse, BoundSetattr_traverse},
    {Py_tp_clear,    BoundSetattr_clear},
    {Py_tp_call,     BoundSetattr_call},
    {0, NULL}
};

static PyType_Spec BoundSetattr_spec = {
    .name = "tracer._tracer.BoundSetattr",
    .basicsize = sizeof(BoundSetattrObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = BoundSetattr_slots,
};

/* ========== TracedGetattr ========== */

PyTypeObject *TracedGetattrType = NULL;

static int TracedGetattr_init(PyObject *self, PyObject *args, PyObject *kw) {
    TracedGetattrObject *o = (TracedGetattrObject *)self;
    static char *kwlist[] = {"original", "trace_hook", NULL};
    PyObject *original, *trace_hook;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO", kwlist,
            &original, &trace_hook))
        return -1;
    Py_INCREF(original); o->original = original;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;
    return 0;
}

static void TracedGetattr_dealloc(PyObject *self) {
    TracedGetattrObject *o = (TracedGetattrObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->original);
    Py_XDECREF(o->trace_hook);
    Py_TYPE(self)->tp_free(self);
}

static int TracedGetattr_traverse(PyObject *self, visitproc visit, void *arg) {
    TracedGetattrObject *o = (TracedGetattrObject *)self;
    Py_VISIT(o->original);
    Py_VISIT(o->trace_hook);
    return 0;
}

static int TracedGetattr_clear(PyObject *self) {
    TracedGetattrObject *o = (TracedGetattrObject *)self;
    Py_CLEAR(o->original);
    Py_CLEAR(o->trace_hook);
    return 0;
}

static PyObject *traced_getattr_call_impl(TracedGetattrObject *tg,
                                          PyObject *self_obj,
                                          PyObject *name) {
    PyObject *value = PyObject_CallFunctionObjArgs(tg->original, self_obj, name, NULL);
    if (!value) return NULL;

    if (!g_state.enabled) return value;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return value;

    if (name_str[0] == '_' && name_str[1] == '_')
        return value;

    /* look up __arw_<name> */
    char arw_key[256];
    snprintf(arw_key, sizeof(arw_key), "__arw_%s", name_str);
    PyObject *arw_name = PyUnicode_FromString(arw_key);
    PyObject *arw = PyObject_CallFunctionObjArgs(tg->original, self_obj, arw_name, NULL);
    Py_DECREF(arw_name);
    if (!arw) { PyErr_Clear(); return value; }

    /* check it's an AttrRecordWrite */
    if (!Py_IS_TYPE(arw, AttrRecordWriteType)) {
        Py_DECREF(arw);
        return value;
    }

    PyFrameObject *frame_ptr = PyEval_GetFrame();
    if (frame_ptr) {
        uint64_t caller_id = ((PyFrameObject *)frame_ptr)->call_id;
        int read_lineno = PyFrame_GetLineNumber(frame_ptr);
        int write_lineno = ((AttrRecordWriteObject *)arw)->call_lineno;

        PyObject *rec = py_current_record(NULL, NULL);
        if (rec && rec != Py_None) {
            CallRecordObject *cr = (CallRecordObject *)rec;
            PyObject *attr_read = PyObject_CallFunction(
                (PyObject *)AttrRecordReadType,
                "Kii", caller_id, write_lineno, read_lineno);
            if (attr_read) {
                PyList_Append(cr->attr_reads, attr_read);
                Py_DECREF(attr_read);
            } else {
                PyErr_Clear();
            }
            Py_DECREF(rec);
        } else {
            Py_XDECREF(rec);
        }
    }

    Py_DECREF(arw);
    return value;
}

static PyObject *TracedGetattr_descr_get(PyObject *self, PyObject *obj, PyObject *cls) {
    if (obj == Py_None || obj == NULL) {
        Py_INCREF(self);
        return self;
    }
    return PyObject_CallFunction((PyObject *)BoundGetattrType, "OO", self, obj);
}

static PyObject *TracedGetattr_call(PyObject *self, PyObject *args, PyObject *kw) {
    TracedGetattrObject *tg = (TracedGetattrObject *)self;
    PyObject *obj, *name;
    if (!PyArg_ParseTuple(args, "OO", &obj, &name))
        return NULL;
    return traced_getattr_call_impl(tg, obj, name);
}

static PyType_Slot TracedGetattr_slots[] = {
    {Py_tp_init,      TracedGetattr_init},
    {Py_tp_dealloc,   TracedGetattr_dealloc},
    {Py_tp_traverse,  TracedGetattr_traverse},
    {Py_tp_clear,     TracedGetattr_clear},
    {Py_tp_call,      TracedGetattr_call},
    {Py_tp_descr_get, TracedGetattr_descr_get},
    {0, NULL}
};

static PyType_Spec TracedGetattr_spec = {
    .name = "tracer._tracer.TracedGetattr",
    .basicsize = sizeof(TracedGetattrObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = TracedGetattr_slots,
};

/* ========== BoundGetattr ========== */

PyTypeObject *BoundGetattrType = NULL;

static int BoundGetattr_init(PyObject *self, PyObject *args, PyObject *kw) {
    BoundGetattrObject *o = (BoundGetattrObject *)self;
    static char *kwlist[] = {"inner", "instance", NULL};
    PyObject *inner, *instance;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO", kwlist, &inner, &instance))
        return -1;
    Py_INCREF(inner); o->inner = inner;
    Py_INCREF(instance); o->instance = instance;
    return 0;
}

static void BoundGetattr_dealloc(PyObject *self) {
    BoundGetattrObject *o = (BoundGetattrObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->inner);
    Py_XDECREF(o->instance);
    Py_TYPE(self)->tp_free(self);
}

static int BoundGetattr_traverse(PyObject *self, visitproc visit, void *arg) {
    BoundGetattrObject *o = (BoundGetattrObject *)self;
    Py_VISIT(o->inner);
    Py_VISIT(o->instance);
    return 0;
}

static int BoundGetattr_clear(PyObject *self) {
    BoundGetattrObject *o = (BoundGetattrObject *)self;
    Py_CLEAR(o->inner);
    Py_CLEAR(o->instance);
    return 0;
}

static PyObject *BoundGetattr_call(PyObject *self, PyObject *args, PyObject *kw) {
    BoundGetattrObject *o = (BoundGetattrObject *)self;
    PyObject *name;
    if (!PyArg_ParseTuple(args, "O", &name))
        return NULL;
    return traced_getattr_call_impl(
        (TracedGetattrObject *)o->inner, o->instance, name);
}

static PyType_Slot BoundGetattr_slots[] = {
    {Py_tp_init,     BoundGetattr_init},
    {Py_tp_dealloc,  BoundGetattr_dealloc},
    {Py_tp_traverse, BoundGetattr_traverse},
    {Py_tp_clear,    BoundGetattr_clear},
    {Py_tp_call,     BoundGetattr_call},
    {0, NULL}
};

static PyType_Spec BoundGetattr_spec = {
    .name = "tracer._tracer.BoundGetattr",
    .basicsize = sizeof(BoundGetattrObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = BoundGetattr_slots,
};

/* ========== OwnershipHook ========== */

PyTypeObject *OwnershipHookType = NULL;

static int OwnershipHook_init(PyObject *self, PyObject *args, PyObject *kw) {
    OwnershipHookObject *o = (OwnershipHookObject *)self;
    static char *kwlist[] = {"db", "trace_hook", NULL};
    PyObject *db, *trace_hook;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO", kwlist, &db, &trace_hook))
        return -1;
    Py_INCREF(db); o->db = db;
    Py_INCREF(trace_hook); o->trace_hook = trace_hook;
    umap_init(&o->patched_classes, 64);
    return 0;
}

static void OwnershipHook_dealloc(PyObject *self) {
    OwnershipHookObject *o = (OwnershipHookObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->db);
    Py_XDECREF(o->trace_hook);
    umap_free(&o->patched_classes);
    Py_TYPE(self)->tp_free(self);
}

static int OwnershipHook_traverse(PyObject *self, visitproc visit, void *arg) {
    OwnershipHookObject *o = (OwnershipHookObject *)self;
    Py_VISIT(o->db);
    Py_VISIT(o->trace_hook);
    return 0;
}

static int OwnershipHook_clear(PyObject *self) {
    OwnershipHookObject *o = (OwnershipHookObject *)self;
    Py_CLEAR(o->db);
    Py_CLEAR(o->trace_hook);
    return 0;
}

void ownership_patch_class(PyObject *ownership, PyObject *cls) {
    OwnershipHookObject *o = (OwnershipHookObject *)ownership;
    uintptr_t cls_id = (uintptr_t)cls;
    if (umap_contains(&o->patched_classes, cls_id))
        return;
    umap_set(&o->patched_classes, cls_id, 1);

    PyObject *sa_name = PyUnicode_InternFromString("__setattr__");
    PyObject *ga_name = PyUnicode_InternFromString("__getattribute__");

    PyObject *orig_setattr = _PyType_Lookup((PyTypeObject *)cls, sa_name);
    Py_DECREF(sa_name);
    if (!orig_setattr) { Py_DECREF(ga_name); return; }
    Py_INCREF(orig_setattr);

    PyObject *orig_getattr = _PyType_Lookup((PyTypeObject *)cls, ga_name);
    Py_DECREF(ga_name);
    if (!orig_getattr) { Py_DECREF(orig_setattr); return; }
    Py_INCREF(orig_getattr);

    PyObject *traced_set = PyObject_CallFunction(
        (PyObject *)TracedSetattrType, "OOO",
        orig_setattr, o->db, o->trace_hook);
    Py_DECREF(orig_setattr);
    if (!traced_set) { Py_DECREF(orig_getattr); PyErr_Clear(); return; }

    PyObject *traced_get = PyObject_CallFunction(
        (PyObject *)TracedGetattrType, "OO",
        orig_getattr, o->trace_hook);
    Py_DECREF(orig_getattr);
    if (!traced_get) { Py_DECREF(traced_set); PyErr_Clear(); return; }

    PyObject_SetAttrString(cls, "__setattr__", traced_set);
    /* skip __getattribute__ patch — diagnosing segfault */
    Py_DECREF(traced_set);
    Py_DECREF(traced_get);
}

static PyObject *OwnershipHook_patch_class_py(PyObject *self, PyObject *arg) {
    ownership_patch_class(self, arg);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef OwnershipHook_methods[] = {
    {"patch_class", OwnershipHook_patch_class_py, METH_O, NULL},
    {NULL}
};

static PyType_Slot OwnershipHook_slots[] = {
    {Py_tp_init,     OwnershipHook_init},
    {Py_tp_dealloc,  OwnershipHook_dealloc},
    {Py_tp_traverse, OwnershipHook_traverse},
    {Py_tp_clear,    OwnershipHook_clear},
    {Py_tp_methods,  OwnershipHook_methods},
    {0, NULL}
};

static PyType_Spec OwnershipHook_spec = {
    .name = "tracer._tracer.OwnershipHook",
    .basicsize = sizeof(OwnershipHookObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = OwnershipHook_slots,
};

/* ========== Module registration ========== */

int ownership_init(PyObject *module) {
#define REGISTER(Name, spec, typevar) do { \
    typevar = (PyTypeObject *)PyType_FromSpec(&spec); \
    if (!typevar) return -1; \
    if (PyModule_AddObject(module, #Name, (PyObject *)typevar) < 0) { \
        Py_DECREF(typevar); \
        return -1; \
    } \
} while(0)

    REGISTER(TracedSetattr,  TracedSetattr_spec,  TracedSetattrType);
    REGISTER(BoundSetattr,   BoundSetattr_spec,   BoundSetattrType);
    REGISTER(TracedGetattr,  TracedGetattr_spec,   TracedGetattrType);
    REGISTER(BoundGetattr,   BoundGetattr_spec,    BoundGetattrType);
    REGISTER(OwnershipHook,  OwnershipHook_spec,   OwnershipHookType);

#undef REGISTER
    return 0;
}
