#include "records.h"
#include <structmember.h>

/* ========== AttrRecordWrite ========== */

PyTypeObject *AttrRecordWriteType = NULL;

static int AttrRecordWrite_init(PyObject *self, PyObject *args, PyObject *kw) {
    AttrRecordWriteObject *o = (AttrRecordWriteObject *)self;
    static char *kwlist[] = {"caller_id", "call_lineno", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Ki", kwlist,
            &o->caller_id, &o->call_lineno))
        return -1;
    return 0;
}

static PyObject *AttrRecordWrite_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    AttrRecordWriteObject *o = (AttrRecordWriteObject *)self;
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "AttrRecordWrite");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = Py_BuildValue("(Ki)", o->caller_id, o->call_lineno);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef AttrRecordWrite_methods[] = {
    {"__reduce__", AttrRecordWrite_reduce, METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef AttrRecordWrite_members[] = {
    {"caller_id", Py_T_ULONGLONG, offsetof(AttrRecordWriteObject, caller_id), Py_READONLY, NULL},
    {"call_lineno", Py_T_INT, offsetof(AttrRecordWriteObject, call_lineno), Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot AttrRecordWrite_slots[] = {
    {Py_tp_init,    AttrRecordWrite_init},
    {Py_tp_methods, AttrRecordWrite_methods},
    {Py_tp_members, AttrRecordWrite_members},
    {0, NULL}
};

static PyType_Spec AttrRecordWrite_spec = {
    .name = "tracer._tracer.AttrRecordWrite",
    .basicsize = sizeof(AttrRecordWriteObject),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = AttrRecordWrite_slots,
};

/* ========== AttrRecordRead ========== */

PyTypeObject *AttrRecordReadType = NULL;

static int AttrRecordRead_init(PyObject *self, PyObject *args, PyObject *kw) {
    AttrRecordReadObject *o = (AttrRecordReadObject *)self;
    static char *kwlist[] = {"caller_id", "write_call_lineno", "read_call_lineno", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Kii", kwlist,
            &o->caller_id, &o->write_call_lineno, &o->read_call_lineno))
        return -1;
    return 0;
}

static PyObject *AttrRecordRead_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    AttrRecordReadObject *o = (AttrRecordReadObject *)self;
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "AttrRecordRead");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = Py_BuildValue("(Kii)", o->caller_id, o->write_call_lineno, o->read_call_lineno);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef AttrRecordRead_methods[] = {
    {"__reduce__", AttrRecordRead_reduce, METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef AttrRecordRead_members[] = {
    {"caller_id", Py_T_ULONGLONG, offsetof(AttrRecordReadObject, caller_id), Py_READONLY, NULL},
    {"write_call_lineno", Py_T_INT, offsetof(AttrRecordReadObject, write_call_lineno), Py_READONLY, NULL},
    {"read_call_lineno", Py_T_INT, offsetof(AttrRecordReadObject, read_call_lineno), Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot AttrRecordRead_slots[] = {
    {Py_tp_init,    AttrRecordRead_init},
    {Py_tp_methods, AttrRecordRead_methods},
    {Py_tp_members, AttrRecordRead_members},
    {0, NULL}
};

static PyType_Spec AttrRecordRead_spec = {
    .name = "tracer._tracer.AttrRecordRead",
    .basicsize = sizeof(AttrRecordReadObject),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = AttrRecordRead_slots,
};

/* ========== CallRecord ========== */

PyTypeObject *CallRecordType = NULL;

static int CallRecord_init(PyObject *self, PyObject *args, PyObject *kw) {
    CallRecordObject *o = (CallRecordObject *)self;
    static char *kwlist[] = {"call_id", "function_id", "caller_id", "call_lineno", "obj_id", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "KiKii", kwlist,
            &o->call_id, &o->function_id, &o->caller_id, &o->call_lineno, &o->obj_id))
        return -1;
    o->control_flow = PyByteArray_FromStringAndSize("", 0);
    if (!o->control_flow) return -1;
    o->attr_reads = PyList_New(0);
    if (!o->attr_reads) { Py_CLEAR(o->control_flow); return -1; }
    return 0;
}

static void CallRecord_dealloc(PyObject *self) {
    CallRecordObject *o = (CallRecordObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->control_flow);
    Py_XDECREF(o->attr_reads);
    Py_TYPE(self)->tp_free(self);
}

static int CallRecord_traverse(PyObject *self, visitproc visit, void *arg) {
    CallRecordObject *o = (CallRecordObject *)self;
    Py_VISIT(o->control_flow);
    Py_VISIT(o->attr_reads);
    return 0;
}

static int CallRecord_clear(PyObject *self) {
    CallRecordObject *o = (CallRecordObject *)self;
    Py_CLEAR(o->control_flow);
    Py_CLEAR(o->attr_reads);
    return 0;
}

static PyObject *CallRecord_append_branch(PyObject *self, PyObject *arg) {
    CallRecordObject *o = (CallRecordObject *)self;
    int taken = PyObject_IsTrue(arg);
    if (taken < 0) return NULL;
    char byte = taken ? 1 : 0;
    Py_ssize_t len = PyByteArray_GET_SIZE(o->control_flow);
    if (PyByteArray_Resize(o->control_flow, len + 1) < 0) return NULL;
    PyByteArray_AS_STRING(o->control_flow)[len] = byte;
    Py_RETURN_NONE;
}

static PyObject *CallRecord_append_attr_read(PyObject *self, PyObject *arg) {
    CallRecordObject *o = (CallRecordObject *)self;
    if (PyList_Append(o->attr_reads, arg) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *CallRecord_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    CallRecordObject *o = (CallRecordObject *)self;
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "CallRecord");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = Py_BuildValue("(KiKii)", o->call_id, o->function_id,
                                    o->caller_id, o->call_lineno, o->obj_id);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef CallRecord_methods[] = {
    {"append_branch", CallRecord_append_branch, METH_O, NULL},
    {"append_attr_read", CallRecord_append_attr_read, METH_O, NULL},
    {"__reduce__", CallRecord_reduce, METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef CallRecord_members[] = {
    {"call_id", Py_T_ULONGLONG, offsetof(CallRecordObject, call_id), Py_READONLY, NULL},
    {"function_id", Py_T_INT, offsetof(CallRecordObject, function_id), Py_READONLY, NULL},
    {"caller_id", Py_T_ULONGLONG, offsetof(CallRecordObject, caller_id), Py_READONLY, NULL},
    {"call_lineno", Py_T_INT, offsetof(CallRecordObject, call_lineno), Py_READONLY, NULL},
    {"obj_id", Py_T_INT, offsetof(CallRecordObject, obj_id), 0, NULL},
    {"control_flow", Py_T_OBJECT_EX, offsetof(CallRecordObject, control_flow), 0, NULL},
    {"attr_reads", Py_T_OBJECT_EX, offsetof(CallRecordObject, attr_reads), Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot CallRecord_slots[] = {
    {Py_tp_init,     CallRecord_init},
    {Py_tp_dealloc,  CallRecord_dealloc},
    {Py_tp_traverse, CallRecord_traverse},
    {Py_tp_clear,    CallRecord_clear},
    {Py_tp_methods,  CallRecord_methods},
    {Py_tp_members,  CallRecord_members},
    {0, NULL}
};

static PyType_Spec CallRecord_spec = {
    .name = "tracer._tracer.CallRecord",
    .basicsize = sizeof(CallRecordObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = CallRecord_slots,
};

/* ========== ObjectRecord ========== */

PyTypeObject *ObjectRecordType = NULL;

static int ObjectRecord_init(PyObject *self, PyObject *args, PyObject *kw) {
    ObjectRecordObject *o = (ObjectRecordObject *)self;
    static char *kwlist[] = {"call_id", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "K", kwlist, &o->call_id))
        return -1;
    o->members = PyDict_New();
    if (!o->members) return -1;
    return 0;
}

static void ObjectRecord_dealloc(PyObject *self) {
    ObjectRecordObject *o = (ObjectRecordObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->members);
    Py_TYPE(self)->tp_free(self);
}

static int ObjectRecord_traverse(PyObject *self, visitproc visit, void *arg) {
    ObjectRecordObject *o = (ObjectRecordObject *)self;
    Py_VISIT(o->members);
    return 0;
}

static int ObjectRecord_clear(PyObject *self) {
    ObjectRecordObject *o = (ObjectRecordObject *)self;
    Py_CLEAR(o->members);
    return 0;
}

static PyObject *ObjectRecord_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    ObjectRecordObject *o = (ObjectRecordObject *)self;
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "ObjectRecord");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = Py_BuildValue("(K)", o->call_id);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef ObjectRecord_methods[] = {
    {"__reduce__", ObjectRecord_reduce, METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef ObjectRecord_members[] = {
    {"call_id", Py_T_ULONGLONG, offsetof(ObjectRecordObject, call_id), Py_READONLY, NULL},
    {"members", Py_T_OBJECT_EX, offsetof(ObjectRecordObject, members), Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot ObjectRecord_slots[] = {
    {Py_tp_init,     ObjectRecord_init},
    {Py_tp_dealloc,  ObjectRecord_dealloc},
    {Py_tp_traverse, ObjectRecord_traverse},
    {Py_tp_clear,    ObjectRecord_clear},
    {Py_tp_methods,  ObjectRecord_methods},
    {Py_tp_members,  ObjectRecord_members},
    {0, NULL}
};

static PyType_Spec ObjectRecord_spec = {
    .name = "tracer._tracer.ObjectRecord",
    .basicsize = sizeof(ObjectRecordObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = ObjectRecord_slots,
};

/* ========== IpcRecord ========== */

PyTypeObject *IpcRecordType = NULL;

static int IpcRecord_init(PyObject *self, PyObject *args, PyObject *kw) {
    IpcRecordObject *o = (IpcRecordObject *)self;
    static char *kwlist[] = {"name", "obj_idx", NULL};
    PyObject *name = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Ol", kwlist, &name, &o->obj_idx))
        return -1;
    Py_INCREF(name);
    o->name = name;
    return 0;
}

static void IpcRecord_dealloc(PyObject *self) {
    IpcRecordObject *o = (IpcRecordObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->name);
    Py_TYPE(self)->tp_free(self);
}

static int IpcRecord_traverse(PyObject *self, visitproc visit, void *arg) {
    IpcRecordObject *o = (IpcRecordObject *)self;
    Py_VISIT(o->name);
    return 0;
}

static int IpcRecord_clear(PyObject *self) {
    IpcRecordObject *o = (IpcRecordObject *)self;
    Py_CLEAR(o->name);
    return 0;
}

static PyObject *IpcRecord_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    IpcRecordObject *o = (IpcRecordObject *)self;
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "IpcRecord");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = Py_BuildValue("(Ol)", o->name, o->obj_idx);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef IpcRecord_methods[] = {
    {"__reduce__", IpcRecord_reduce, METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef IpcRecord_members[] = {
    {"name", Py_T_OBJECT_EX, offsetof(IpcRecordObject, name), Py_READONLY, NULL},
    {"obj_idx", Py_T_LONGLONG, offsetof(IpcRecordObject, obj_idx), Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot IpcRecord_slots[] = {
    {Py_tp_init,     IpcRecord_init},
    {Py_tp_dealloc,  IpcRecord_dealloc},
    {Py_tp_traverse, IpcRecord_traverse},
    {Py_tp_clear,    IpcRecord_clear},
    {Py_tp_methods,  IpcRecord_methods},
    {Py_tp_members,  IpcRecord_members},
    {0, NULL}
};

static PyType_Spec IpcRecord_spec = {
    .name = "tracer._tracer.IpcRecord",
    .basicsize = sizeof(IpcRecordObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = IpcRecord_slots,
};

/* ========== Database ========== */

PyTypeObject *DatabaseType = NULL;

static int Database_init(PyObject *self, PyObject *args, PyObject *kw) {
    DatabaseObject *o = (DatabaseObject *)self;
    o->calls = PyList_New(0);
    if (!o->calls) return -1;
    o->objects = PyList_New(0);
    if (!o->objects) { Py_CLEAR(o->calls); return -1; }
    o->ipc = PyList_New(0);
    if (!o->ipc) { Py_CLEAR(o->calls); Py_CLEAR(o->objects); return -1; }
    return 0;
}

static void Database_dealloc(PyObject *self) {
    DatabaseObject *o = (DatabaseObject *)self;
    PyObject_GC_UnTrack(self);
    Py_XDECREF(o->calls);
    Py_XDECREF(o->objects);
    Py_XDECREF(o->ipc);
    Py_TYPE(self)->tp_free(self);
}

static int Database_traverse(PyObject *self, visitproc visit, void *arg) {
    DatabaseObject *o = (DatabaseObject *)self;
    Py_VISIT(o->calls);
    Py_VISIT(o->objects);
    Py_VISIT(o->ipc);
    return 0;
}

static int Database_clear(PyObject *self) {
    DatabaseObject *o = (DatabaseObject *)self;
    Py_CLEAR(o->calls);
    Py_CLEAR(o->objects);
    Py_CLEAR(o->ipc);
    return 0;
}

static PyObject *Database_add_call(PyObject *self, PyObject *arg) {
    DatabaseObject *o = (DatabaseObject *)self;
    Py_ssize_t idx = PyList_GET_SIZE(o->calls);
    if (PyList_Append(o->calls, arg) < 0) return NULL;
    return PyLong_FromSsize_t(idx);
}

static PyObject *Database_add_object(PyObject *self, PyObject *arg) {
    DatabaseObject *o = (DatabaseObject *)self;
    Py_ssize_t idx = PyList_GET_SIZE(o->objects);
    if (PyList_Append(o->objects, arg) < 0) return NULL;
    return PyLong_FromSsize_t(idx);
}

static PyObject *Database_add_ipc(PyObject *self, PyObject *arg) {
    DatabaseObject *o = (DatabaseObject *)self;
    Py_ssize_t idx = PyList_GET_SIZE(o->ipc);
    if (PyList_Append(o->ipc, arg) < 0) return NULL;
    return PyLong_FromSsize_t(idx);
}

static PyObject *Database_reduce(PyObject *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *mod = PyImport_ImportModule("tracer._tracer");
    if (!mod) return NULL;
    PyObject *cls = PyObject_GetAttrString(mod, "Database");
    Py_DECREF(mod);
    if (!cls) return NULL;
    PyObject *args = PyTuple_New(0);
    if (!args) { Py_DECREF(cls); return NULL; }
    PyObject *result = PyTuple_Pack(2, cls, args);
    Py_DECREF(cls);
    Py_DECREF(args);
    return result;
}

static PyMethodDef Database_methods[] = {
    {"add_call",   Database_add_call,   METH_O, NULL},
    {"add_object", Database_add_object, METH_O, NULL},
    {"add_ipc",    Database_add_ipc,    METH_O, NULL},
    {"__reduce__", Database_reduce,     METH_NOARGS, NULL},
    {NULL}
};

static PyMemberDef Database_members[] = {
    {"calls",   Py_T_OBJECT_EX, offsetof(DatabaseObject, calls),   Py_READONLY, NULL},
    {"objects", Py_T_OBJECT_EX, offsetof(DatabaseObject, objects), Py_READONLY, NULL},
    {"ipc",     Py_T_OBJECT_EX, offsetof(DatabaseObject, ipc),     Py_READONLY, NULL},
    {NULL}
};

static PyType_Slot Database_slots[] = {
    {Py_tp_init,     Database_init},
    {Py_tp_dealloc,  Database_dealloc},
    {Py_tp_traverse, Database_traverse},
    {Py_tp_clear,    Database_clear},
    {Py_tp_methods,  Database_methods},
    {Py_tp_members,  Database_members},
    {0, NULL}
};

static PyType_Spec Database_spec = {
    .name = "tracer._tracer.Database",
    .basicsize = sizeof(DatabaseObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = Database_slots,
};

/* ========== Module registration ========== */

int records_init(PyObject *module) {
#define REGISTER(Name, spec, typevar) do { \
    typevar = (PyTypeObject *)PyType_FromSpec(&spec); \
    if (!typevar) return -1; \
    if (PyModule_AddObject(module, #Name, (PyObject *)typevar) < 0) { \
        Py_DECREF(typevar); \
        return -1; \
    } \
} while(0)

    REGISTER(AttrRecordWrite, AttrRecordWrite_spec, AttrRecordWriteType);
    REGISTER(AttrRecordRead,  AttrRecordRead_spec,  AttrRecordReadType);
    REGISTER(CallRecord,      CallRecord_spec,      CallRecordType);
    REGISTER(ObjectRecord,    ObjectRecord_spec,     ObjectRecordType);
    REGISTER(IpcRecord,       IpcRecord_spec,        IpcRecordType);
    REGISTER(Database,        Database_spec,          DatabaseType);

#undef REGISTER
    return 0;
}
