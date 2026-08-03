#ifndef TRACER_CONTAINERS_H
#define TRACER_CONTAINERS_H

#include <Python.h>
#include <stdint.h>
#include "../hook.h"

typedef enum {
    CONTAINER_NONE,
    CONTAINER_DICT,
    CONTAINER_LIST,
    CONTAINER_DEQUE,
    CONTAINER_SET,
} ContainerType;

#include "dict.h"
#include "list.h"
#include "deque.h"
#include "set.h"

ARW caller_arw(void);
void emit_read(const ARW *arw);

PyObject *wrap_container(PyObject *value, PyObject *db, int obj_idx);

int containers_init(PyObject *module);

#endif
