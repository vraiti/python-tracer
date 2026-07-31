#include <Python.h>
#include "records.h"
#include "filter.h"
#include "hook.h"
#include "ownership.h"
#include "containers.h"

static struct PyModuleDef tracer_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_tracer",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit__tracer(void) {
    PyObject *m = PyModule_Create(&tracer_module);
    if (!m) return NULL;

    if (records_init(m) < 0) goto fail;
    if (filter_init(m) < 0) goto fail;
    if (ownership_init(m) < 0) goto fail;
    if (containers_init(m) < 0) goto fail;
    if (hook_init(m) < 0) goto fail;

    return m;

fail:
    Py_DECREF(m);
    return NULL;
}
