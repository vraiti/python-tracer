#include "filter.h"
#include <string.h>

PyTypeObject *PathFilterType = NULL;

static int PathFilter_init(PyObject *self, PyObject *args, PyObject *kw) {
    PathFilterObject *o = (PathFilterObject *)self;
    static char *kwlist[] = {"prefixes", "tracked_file", "tracked_classes", NULL};
    PyObject *prefixes_obj = Py_None;
    const char *tracked_file = NULL;
    PyObject *tracked_classes_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "|OzO", kwlist,
            &prefixes_obj, &tracked_file, &tracked_classes_obj))
        return -1;

    o->prefixes = NULL;
    o->prefix_count = 0;
    umap_init(&o->scope_cache, 256);
    smap_init(&o->tracked_classes, 64);

    if (prefixes_obj != Py_None && prefixes_obj != NULL) {
        if (!PyList_Check(prefixes_obj)) {
            PyErr_SetString(PyExc_TypeError, "prefixes must be a list");
            return -1;
        }
        Py_ssize_t n = PyList_GET_SIZE(prefixes_obj);
        o->prefixes = malloc(n * sizeof(char *));
        if (!o->prefixes) { PyErr_NoMemory(); return -1; }
        o->prefix_count = n;
        for (Py_ssize_t i = 0; i < n; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(prefixes_obj, i));
            if (!s) return -1;
            o->prefixes[i] = strdup(s);
        }
    } else {
        /* auto-detect from vllm_omni, vllm packages */
        const char *pkgs[] = {"vllm_omni", "vllm"};
        size_t cap = 4;
        o->prefixes = malloc(cap * sizeof(char *));
        if (!o->prefixes) { PyErr_NoMemory(); return -1; }

        for (int pi = 0; pi < 2; pi++) {
            PyObject *mod = PyImport_ImportModule(pkgs[pi]);
            if (!mod) { PyErr_Clear(); continue; }
            PyObject *path = PyObject_GetAttrString(mod, "__path__");
            if (path && PyList_Check(path)) {
                Py_ssize_t pn = PyList_GET_SIZE(path);
                for (Py_ssize_t j = 0; j < pn; j++) {
                    const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(path, j));
                    if (!s) { Py_DECREF(path); Py_DECREF(mod); return -1; }
                    if ((size_t)o->prefix_count >= cap) {
                        cap *= 2;
                        o->prefixes = realloc(o->prefixes, cap * sizeof(char *));
                    }
                    o->prefixes[o->prefix_count++] = strdup(s);
                }
                Py_DECREF(path);
            } else {
                PyErr_Clear();
                PyObject *file = PyObject_GetAttrString(mod, "__file__");
                if (file) {
                    const char *fstr = PyUnicode_AsUTF8(file);
                    if (fstr) {
                        const char *slash = strrchr(fstr, '/');
                        if (slash) {
                            size_t dirlen = slash - fstr;
                            if ((size_t)o->prefix_count >= cap) {
                                cap *= 2;
                                o->prefixes = realloc(o->prefixes, cap * sizeof(char *));
                            }
                            o->prefixes[o->prefix_count] = malloc(dirlen + 1);
                            memcpy(o->prefixes[o->prefix_count], fstr, dirlen);
                            o->prefixes[o->prefix_count][dirlen] = '\0';
                            o->prefix_count++;
                        }
                    }
                    Py_DECREF(file);
                } else {
                    PyErr_Clear();
                }
            }
            Py_DECREF(mod);
        }
    }

    if (tracked_file) {
        FILE *f = fopen(tracked_file, "r");
        if (!f) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, tracked_file);
            return -1;
        }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            /* trim whitespace */
            char *start = line;
            while (*start == ' ' || *start == '\t') start++;
            char *end = start + strlen(start);
            while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
                end--;
            *end = '\0';
            if (*start && *start != '#')
                smap_set(&o->tracked_classes, start, NULL);
        }
        fclose(f);
    }

    if (tracked_classes_obj != Py_None && tracked_classes_obj != NULL) {
        if (!PyList_Check(tracked_classes_obj)) {
            PyErr_SetString(PyExc_TypeError, "tracked_classes must be a list");
            return -1;
        }
        Py_ssize_t n = PyList_GET_SIZE(tracked_classes_obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(tracked_classes_obj, i));
            if (!s) return -1;
            smap_set(&o->tracked_classes, s, NULL);
        }
    }

    return 0;
}

static void PathFilter_dealloc(PyObject *self) {
    PathFilterObject *o = (PathFilterObject *)self;
    if (o->prefixes) {
        for (Py_ssize_t i = 0; i < o->prefix_count; i++)
            free(o->prefixes[i]);
        free(o->prefixes);
    }
    umap_free(&o->scope_cache);
    smap_free(&o->tracked_classes);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *PathFilter_is_in_scope(PyObject *self, PyObject *arg) {
    PathFilterObject *o = (PathFilterObject *)self;
    if (!PyUnicode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return NULL;
    }
    uintptr_t ptr = (uintptr_t)arg;
    intptr_t cached;
    if (umap_get(&o->scope_cache, ptr, &cached))
        return PyBool_FromLong(cached);

    const char *fname = PyUnicode_AsUTF8(arg);
    if (!fname) return NULL;
    int result = 0;
    for (Py_ssize_t i = 0; i < o->prefix_count; i++) {
        if (strncmp(fname, o->prefixes[i], strlen(o->prefixes[i])) == 0) {
            result = 1;
            break;
        }
    }
    umap_set(&o->scope_cache, ptr, result);
    return PyBool_FromLong(result);
}

static PyObject *PathFilter_is_tracked_class(PyObject *self, PyObject *arg) {
    PathFilterObject *o = (PathFilterObject *)self;
    if (!PyType_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected type");
        return NULL;
    }
    PyObject *module = PyObject_GetAttrString(arg, "__module__");
    if (!module) return NULL;
    PyObject *qualname = PyObject_GetAttrString(arg, "__qualname__");
    if (!qualname) { Py_DECREF(module); return NULL; }

    const char *mod_str = PyUnicode_AsUTF8(module);
    const char *qual_str = PyUnicode_AsUTF8(qualname);
    int found = 0;
    if (mod_str && qual_str) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s.%s", mod_str, qual_str);
        found = smap_contains(&o->tracked_classes, buf);
    }
    Py_DECREF(module);
    Py_DECREF(qualname);
    return PyBool_FromLong(found);
}

static PyObject *PathFilter_get_prefixes(PyObject *self, void *Py_UNUSED(closure)) {
    PathFilterObject *o = (PathFilterObject *)self;
    PyObject *list = PyList_New(o->prefix_count);
    if (!list) return NULL;
    for (Py_ssize_t i = 0; i < o->prefix_count; i++) {
        PyObject *s = PyUnicode_FromString(o->prefixes[i]);
        if (!s) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i, s);
    }
    return list;
}

static PyMethodDef PathFilter_methods[] = {
    {"is_in_scope",      PathFilter_is_in_scope,      METH_O, NULL},
    {"is_tracked_class", PathFilter_is_tracked_class,  METH_O, NULL},
    {NULL}
};

static PyGetSetDef PathFilter_getset[] = {
    {"_prefixes", PathFilter_get_prefixes, NULL, NULL, NULL},
    {NULL}
};

static PyType_Slot PathFilter_slots[] = {
    {Py_tp_init,    PathFilter_init},
    {Py_tp_dealloc, PathFilter_dealloc},
    {Py_tp_methods, PathFilter_methods},
    {Py_tp_getset,  PathFilter_getset},
    {0, NULL}
};

static PyType_Spec PathFilter_spec = {
    .name = "tracer._tracer.PathFilter",
    .basicsize = sizeof(PathFilterObject),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = PathFilter_slots,
};

int filter_init(PyObject *module) {
    PathFilterType = (PyTypeObject *)PyType_FromSpec(&PathFilter_spec);
    if (!PathFilterType) return -1;
    if (PyModule_AddObject(module, "PathFilter", (PyObject *)PathFilterType) < 0) {
        Py_DECREF(PathFilterType);
        return -1;
    }
    return 0;
}
