#ifndef TRACER_CONTAINERS_DEQUE_H
#define TRACER_CONTAINERS_DEQUE_H

#include "../hashmap.h"
#include <stddef.h>

#define ARW_BLOCKLEN 64
#define ARW_BLOCK_CENTER ((ARW_BLOCKLEN - 1) / 2)
#define ARW_MAX_FREEBLOCKS 16

typedef struct ARWBlock {
    struct ARWBlock *leftlink;
    ARW data[ARW_BLOCKLEN];
    struct ARWBlock *rightlink;
} ARWBlock;

typedef struct {
    ARWBlock *leftblock;
    ARWBlock *rightblock;
    size_t leftindex;
    size_t rightindex;
    size_t len;
    size_t numfreeblocks;
    ARWBlock *freeblocks[ARW_MAX_FREEBLOCKS];
} ARWDeque;

void arwdeque_init(ARWDeque *d);
void arwdeque_free(ARWDeque *d);
int arwdeque_append(ARWDeque *d, ARW value);
int arwdeque_appendleft(ARWDeque *d, ARW value);
int arwdeque_pop(ARWDeque *d, ARW *out);
int arwdeque_popleft(ARWDeque *d, ARW *out);
int arwdeque_get(const ARWDeque *d, size_t index, ARW *out);
size_t arwdeque_len(const ARWDeque *d);

#ifdef Py_PYTHON_H

#include "../hook.h"

typedef struct {
    ObjectTraceData base;
    ARWDeque arws;
} DequeTraceData;

extern PyTypeObject *TracedDequeType;

int traced_deque_init(PyObject *module);

#endif /* Py_PYTHON_H */

#endif
