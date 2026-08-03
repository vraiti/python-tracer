#include "hook.h"
#include "containers/containers.h"
#include "internal/pycore_frame.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* forward declaration — implemented in ownership.c */
extern void ownership_patch_class(PyObject *ownership, PyObject *cls);

TraceState g_state = {0};

/* ---- object extras helpers ---- */

/* ---- frame call_id helpers ---- */

void set_frame_call_id(PyFrameObject *frame, uint64_t cid) {
    umap_set(&g_state.frame_call_ids, (uintptr_t)frame->f_frame, (intptr_t)cid);
}

uint64_t get_frame_call_id(PyFrameObject *frame) {
    intptr_t val;
    if (umap_get(&g_state.frame_call_ids, (uintptr_t)frame->f_frame, &val))
        return (uint64_t)val;
    return 0;
}

/* ---- object extras helpers ---- */

ObjectTraceData *get_trace_data(PyObject *obj) {
    intptr_t val;
    if (umap_get(&g_state.object_extras, (uintptr_t)obj, &val))
        return (ObjectTraceData *)val;
    return NULL;
}

static void free_trace_data(ObjectTraceData *data) {
    switch (data->type) {
    case CONTAINER_DICT: {
        DictTraceData *td = (DictTraceData *)data;
        arwdict_free(&td->arws);
        break;
    }
    case CONTAINER_LIST: {
        ListTraceData *td = (ListTraceData *)data;
        arwlist_free(&td->arws);
        break;
    }
    case CONTAINER_DEQUE: {
        DequeTraceData *td = (DequeTraceData *)data;
        arwdeque_free(&td->arws);
        break;
    }
    case CONTAINER_SET: {
        SetTraceData *td = (SetTraceData *)data;
        arwset_free(&td->arws);
        break;
    }
    default:
        break;
    }
    ARWMap_free(&data->attrs);
    free(data);
}

/* weakref finalizer: called when a tracked object is deallocated */

typedef struct {
    PyObject_HEAD
    uintptr_t obj_key;
} ExtraCleanupObject;

static PyObject *extra_cleanup_call(PyObject *self, PyObject *args, PyObject *kw) {
    ExtraCleanupObject *ec = (ExtraCleanupObject *)self;
    intptr_t val;
    if (umap_get(&g_state.object_extras, ec->obj_key, &val)) {
        umap_delete(&g_state.object_extras, ec->obj_key);
        free_trace_data((ObjectTraceData *)val);
    }
    Py_RETURN_NONE;
}

static PyType_Slot ExtraCleanup_slots[] = {
    {Py_tp_call, extra_cleanup_call},
    {0, NULL},
};

static PyType_Spec ExtraCleanup_spec = {
    .name = "_tracer.ExtraCleanup",
    .basicsize = sizeof(ExtraCleanupObject),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = ExtraCleanup_slots,
};

static PyTypeObject *ExtraCleanupType = NULL;


/* ---- bitset helpers ---- */

static inline int bitset_test(const Bitset *bs, int32_t line) {
    if (line < 0) return 0;
    size_t idx = (size_t)line / 64;
    if (idx >= bs->n_words) return 0;
    return (bs->words[idx] >> ((size_t)line % 64)) & 1;
}

static Bitset bitset_from_pyset(PyObject *pyset) {
    Bitset bs = {NULL, -1, 0};
    PyObject *iter = PyObject_GetIter(pyset);
    if (!iter) { PyErr_Clear(); return bs; }

    int32_t max_line = -1;
    size_t line_count = 0;
    size_t line_cap = 64;
    int32_t *lines = malloc(line_cap * sizeof(int32_t));

    PyObject *item;
    while ((item = PyIter_Next(iter))) {
        long v = PyLong_AsLong(item);
        Py_DECREF(item);
        if (v == -1 && PyErr_Occurred()) { PyErr_Clear(); continue; }
        if (line_count >= line_cap) {
            line_cap *= 2;
            lines = realloc(lines, line_cap * sizeof(int32_t));
        }
        lines[line_count++] = (int32_t)v;
        if ((int32_t)v > max_line) max_line = (int32_t)v;
    }
    Py_DECREF(iter);

    if (max_line < 0) { free(lines); return bs; }

    bs.max_line = max_line;
    bs.n_words = (size_t)max_line / 64 + 1;
    bs.words = calloc(bs.n_words, sizeof(uint64_t));
    for (size_t i = 0; i < line_count; i++)
        bs.words[lines[i] / 64] |= 1ULL << (lines[i] % 64);
    free(lines);
    return bs;
}

/* ---- per-coroutine frame stacks (thread-local) ---- */

static _Thread_local UMap tl_coro_stacks = {0};
static _Thread_local int tl_stacks_init = 0;

static uintptr_t get_coroutine_id(PyFrameObject *frame) {
    _PyInterpreterFrame *iframe = frame->f_frame;
    if (iframe->owner == FRAME_OWNED_BY_GENERATOR)
        return (uintptr_t)_PyFrame_GetGenerator(iframe);
    return 0;
}

static FrameStack *get_frame_stack(PyFrameObject *frame) {
    if (!tl_stacks_init) {
        umap_init(&tl_coro_stacks, 4);
        tl_stacks_init = 1;
    }
    uintptr_t coro_id = get_coroutine_id(frame);
    intptr_t val;
    if (umap_get(&tl_coro_stacks, coro_id, &val))
        return (FrameStack *)val;
    FrameStack *stack = calloc(1, sizeof(FrameStack));
    umap_set(&tl_coro_stacks, coro_id, (intptr_t)stack);
    return stack;
}

static void frame_stack_push(PyFrameObject *frame, FrameEntry *entry) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count >= stack->cap) {
        stack->cap = stack->cap ? stack->cap * 2 : 64;
        stack->entries = realloc(stack->entries, stack->cap * sizeof(FrameEntry));
    }
    stack->entries[stack->count++] = *entry;
}

static FrameEntry *frame_stack_peek(PyFrameObject *frame) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count == 0) return NULL;
    return &stack->entries[stack->count - 1];
}

static void frame_stack_pop(PyFrameObject *frame) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count > 0) {
        FrameEntry *e = &stack->entries[--stack->count];
        free(e->branch_buf);
        free(e->cf_bits.words);
    }
}

/* ---- scope check ---- */

static int check_scope(uintptr_t filename_ptr, const char *filename) {
    intptr_t cached;
    if (umap_get(&g_state.scope_cache, filename_ptr, &cached))
        return (int)cached;
    int result = 0;
    for (Py_ssize_t i = 0; i < g_state.prefix_count; i++) {
        if (strncmp(filename, g_state.prefixes[i], strlen(g_state.prefixes[i])) == 0) {
            result = 1;
            break;
        }
    }
    umap_set(&g_state.scope_cache, filename_ptr, result);
    return result;
}

/* ---- get_or_assign_function_id ---- */

static int32_t get_or_assign_function_id(const char *ref_str) {
    void *val;
    if (SMap_get(&g_state.func_to_id, ref_str, &val))
        return (int32_t)(intptr_t)val;
    int32_t id = g_state.next_func_id++;
    SMap_set(&g_state.func_to_id, ref_str, (void *)(intptr_t)id);
    return id;
}

/* ---- get_self_obj_id ---- */

static PyObject *get_self_obj(PyObject *py_frame, PyCodeObject *code) {
    if (code->co_argcount < 1)
        return NULL;
    PyFrameObject *frame = (PyFrameObject *)py_frame;
    _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame->f_frame;
    if (!iframe) return NULL;
    PyObject *self_obj = iframe->localsplus[0];
    return self_obj;
}

static int32_t get_obj_id(PyObject *self_obj) {
    if (!self_obj) return 0;
    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td) return 0;
    return (int32_t)td->id;
}

static int is_tracked_class(PyObject *self_obj, PyCodeObject *code) {
    PathFilterObject *pf = (PathFilterObject *)g_state.filter;
    if (!pf) return 0;

    /* Check if the __init__'s source file is under a traced prefix */
    const char *filename = PyUnicode_AsUTF8(code->co_filename);
    if (filename) {
        for (Py_ssize_t i = 0; i < pf->prefix_count; i++) {
            if (strncmp(filename, pf->prefixes[i], strlen(pf->prefixes[i])) == 0)
                return 1;
        }
    }

    /* Check explicit tracked_classes list */
    PyObject *cls = (PyObject *)Py_TYPE(self_obj);
    PyObject *module = PyObject_GetAttrString(cls, "__module__");
    PyObject *qualname_attr = PyObject_GetAttrString(cls, "__qualname__");
    int result = 0;
    if (module && qualname_attr) {
        const char *mod_str = PyUnicode_AsUTF8(module);
        const char *qual_str = PyUnicode_AsUTF8(qualname_attr);
        if (mod_str && qual_str) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s.%s", mod_str, qual_str);
            result = SMap_contains(&pf->tracked_classes, buf);
        }
    }
    Py_XDECREF(module);
    Py_XDECREF(qualname_attr);
    return result;
}

/* ---- handle_init: create ObjectRecord, attach trace data ---- */

static void handle_init(PyObject *self_obj, PyCodeObject *code, uint64_t call_id) {
    PyObject *cls = (PyObject *)Py_TYPE(self_obj);
    if (!cls) return;

    PyObject *cls_init = PyObject_GetAttrString(cls, "__init__");
    if (!cls_init) { PyErr_Clear(); return; }
    PyObject *cls_code = PyObject_GetAttrString(cls_init, "__code__");
    Py_DECREF(cls_init);
    if (!cls_code) { PyErr_Clear(); return; }

    int matches = (PyCodeObject *)cls_code == code;
    Py_DECREF(cls_code);
    if (!matches) return;

    /* create ObjectRecord and add to db */
    DatabaseObject *db = (DatabaseObject *)g_state.db;
    PyObject *obj_rec = PyObject_CallFunction((PyObject *)ObjectRecordType, "K", call_id);
    if (!obj_rec) { PyErr_Clear(); return; }

    Py_ssize_t obj_idx = PyList_GET_SIZE(db->objects);
    if (PyList_Append(db->objects, obj_rec) < 0) {
        Py_DECREF(obj_rec);
        PyErr_Clear();
        return;
    }
    Py_DECREF(obj_rec);

    ObjectTraceData *trace_data = malloc(sizeof(ObjectTraceData));
    trace_data->id = (uint64_t)obj_idx;
    ARWMap_init(&trace_data->attrs, 16);
    trace_data->type = CONTAINER_NONE;
    umap_set(&g_state.object_extras, (uintptr_t)self_obj, (intptr_t)trace_data);

    ExtraCleanupObject *ec = (ExtraCleanupObject *)PyObject_CallNoArgs(
        (PyObject *)ExtraCleanupType);
    if (ec) {
        ec->obj_key = (uintptr_t)self_obj;
        PyObject *weakref = PyWeakref_NewRef(self_obj, (PyObject *)ec);
        Py_XDECREF(weakref);
        Py_DECREF(ec);
    } else {
        PyErr_Clear();
    }
}

/* ---- push a traced frame ---- */

static void push_traced_frame(PyFrameObject *frame, uint64_t call_id,
                              PyObject *record, const char *ref_str) {
    FrameEntry entry;
    entry.call_id = call_id;
    entry.record = record;
    entry.pending_cf = 0;
    entry.branch_buf = NULL;
    entry.branch_len = 0;
    entry.branch_cap = 0;

    void *bits_ptr;
    if (SMap_get(&g_state.cf_bits, ref_str, &bits_ptr)) {
        Bitset *src = (Bitset *)bits_ptr;
        entry.cf_bits.max_line = src->max_line;
        entry.cf_bits.n_words = src->n_words;
        entry.cf_bits.words = malloc(src->n_words * sizeof(uint64_t));
        memcpy(entry.cf_bits.words, src->words, src->n_words * sizeof(uint64_t));
    } else {
        entry.cf_bits.words = NULL;
        entry.cf_bits.max_line = -1;
        entry.cf_bits.n_words = 0;
    }

    frame_stack_push(frame, &entry);
}

/* ---- trace callback ---- */

static int handle_call(PyObject *py_frame, PyFrameObject *frame_obj) {
    /* taint propagation */
    PyFrameObject *back = PyFrame_GetBack(frame_obj);
    if (back) {
        uint64_t caller_cid = get_frame_call_id(back);
        Py_DECREF((PyObject *)back);
        if (caller_cid == UINT64_MAX) {
            set_frame_call_id((PyFrameObject *)py_frame, UINT64_MAX);
            ((PyFrameObject *)py_frame)->f_trace_lines = 0;
            return 0;
        }
    }

    PyCodeObject *code = PyFrame_GetCode(frame_obj);
    if (!code) return 0;

    PyObject *filename_obj = code->co_filename;
    uintptr_t filename_ptr = (uintptr_t)filename_obj;
    Py_ssize_t fname_size;
    const char *filename = PyUnicode_AsUTF8AndSize(filename_obj, &fname_size);
    if (!filename) {
        PyErr_Clear();
        Py_DECREF(code);
        return 0;
    }

    int in_scope = check_scope(filename_ptr, filename);

    /* taint origination */
    if (g_state.taint_count > 0) {
        PyObject *qualname_obj = code->co_qualname;
        Py_ssize_t qn_size;
        const char *qualname = PyUnicode_AsUTF8AndSize(qualname_obj, &qn_size);
        if (qualname) {
            for (Py_ssize_t i = 0; i < g_state.taint_count; i++) {
                if (strstr(qualname, g_state.taint_patterns[i])) {
                    set_frame_call_id((PyFrameObject *)py_frame, UINT64_MAX);
                    ((PyFrameObject *)py_frame)->f_trace_lines = 0;
                    Py_DECREF(code);
                    return 0;
                }
            }
        }
    }

    if (!in_scope) {
        set_frame_call_id((PyFrameObject *)py_frame, 0);
        ((PyFrameObject *)py_frame)->f_trace_lines = 0;

        /* check for __init__ on tracked class */
        PyObject *co_name = code->co_name;
        Py_ssize_t name_size;
        const char *name = PyUnicode_AsUTF8AndSize(co_name, &name_size);
        if (name && name_size == 8 && memcmp(name, "__init__", 8) == 0) {
            PyObject *self_obj = get_self_obj(py_frame, code);
            if (self_obj && is_tracked_class(self_obj, code)) {
                uint64_t call_id = g_state.next_call_id++;
                uint64_t caller_id = 0;
                int call_lineno = 0;
                PyFrameObject *back2 = PyFrame_GetBack(frame_obj);
                if (back2) {
                    caller_id = get_frame_call_id(back2);
                    call_lineno = PyFrame_GetLineNumber(back2);
                    Py_DECREF((PyObject *)back2);
                }

                Py_ssize_t qn_sz;
                const char *qn = PyUnicode_AsUTF8AndSize(code->co_qualname, &qn_sz);
                if (qn) {
                    char ref_buf[1024];
                    snprintf(ref_buf, sizeof(ref_buf), "%s:%s", filename, qn);
                    int32_t function_id = get_or_assign_function_id(ref_buf);

                    set_frame_call_id((PyFrameObject *)py_frame, call_id);
                    ((PyFrameObject *)py_frame)->f_trace_lines = 1;

                    DatabaseObject *db = (DatabaseObject *)g_state.db;
                    PyObject *rec = PyObject_CallFunction(
                        (PyObject *)CallRecordType,
                        "KiKii", call_id, function_id, caller_id, call_lineno, 0);
                    if (rec) {
                        PyList_Append(db->calls, rec);

                        handle_init(self_obj, code, call_id);
                        int32_t obj_id = get_obj_id(self_obj);
                        ((CallRecordObject *)rec)->obj_id = obj_id;

                        push_traced_frame(frame_obj, call_id, rec, ref_buf);
                        Py_DECREF(rec);
                    } else {
                        PyErr_Clear();
                    }
                }
            }
        }

        Py_DECREF(code);
        return 0;
    }

    /* in-scope call */
    uint64_t call_id = g_state.next_call_id++;
    set_frame_call_id((PyFrameObject *)py_frame, call_id);

    uint64_t caller_id = 0;
    int call_lineno = 0;
    PyFrameObject *back3 = PyFrame_GetBack(frame_obj);
    if (back3) {
        caller_id = get_frame_call_id(back3);
        call_lineno = PyFrame_GetLineNumber(back3);
        Py_DECREF((PyObject *)back3);
    }

    Py_ssize_t qn_size;
    const char *qualname = PyUnicode_AsUTF8AndSize(code->co_qualname, &qn_size);
    if (!qualname) {
        PyErr_Clear();
        Py_DECREF(code);
        return 0;
    }

    char ref_buf[1024];
    snprintf(ref_buf, sizeof(ref_buf), "%s:%s", filename, qualname);
    int32_t function_id = get_or_assign_function_id(ref_buf);

    PyObject *self_obj = get_self_obj(py_frame, code);
    int32_t obj_id = get_obj_id(self_obj);

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    PyObject *rec = PyObject_CallFunction(
        (PyObject *)CallRecordType,
        "KiKii", call_id, function_id, caller_id, call_lineno, obj_id);
    if (!rec) { PyErr_Clear(); Py_DECREF(code); return 0; }
    PyList_Append(db->calls, rec);

    /* __init__ handling for in-scope calls (tracked classes only) */
    Py_ssize_t name_size;
    const char *co_name_str = PyUnicode_AsUTF8AndSize(code->co_name, &name_size);
    if (co_name_str && self_obj &&
        name_size == 8 && memcmp(co_name_str, "__init__", 8) == 0 &&
        is_tracked_class(self_obj, code)) {
        handle_init(self_obj, code, call_id);
        int32_t new_obj_id = get_obj_id(self_obj);
        ((CallRecordObject *)rec)->obj_id = new_obj_id;
    }

    push_traced_frame(frame_obj, call_id, rec, ref_buf);
    Py_DECREF(rec);

    ((PyFrameObject *)py_frame)->f_trace_lines = 1;
    Py_DECREF(code);
    return 0;
}

static int handle_line(PyObject *py_frame, PyFrameObject *frame_obj) {
    int lineno = PyFrame_GetLineNumber(frame_obj);
    FrameEntry *entry = frame_stack_peek(frame_obj);
    if (!entry) return 0;

    if (entry->pending_cf > 0) {
        int taken = (lineno == entry->pending_cf + 1);
        if (entry->branch_len >= entry->branch_cap) {
            entry->branch_cap = entry->branch_cap ? entry->branch_cap * 2 : 32;
            entry->branch_buf = realloc(entry->branch_buf, entry->branch_cap);
        }
        entry->branch_buf[entry->branch_len++] = taken ? 1 : 0;
        entry->pending_cf = 0;
    }

    if (entry->cf_bits.words &&
        lineno <= entry->cf_bits.max_line &&
        bitset_test(&entry->cf_bits, lineno)) {
        entry->pending_cf = lineno;
    }

    return 0;
}

static int handle_return(PyObject *py_frame, PyFrameObject *frame_obj) {
    uint64_t cid = get_frame_call_id((PyFrameObject *)py_frame);
    if (cid == 0 || cid == UINT64_MAX) return 0;

    FrameStack *stack = get_frame_stack(frame_obj);
    while (stack->count > 0) {
        FrameEntry *entry = &stack->entries[stack->count - 1];

        if (entry->pending_cf > 0) {
            if (entry->branch_len >= entry->branch_cap) {
                entry->branch_cap = entry->branch_cap ? entry->branch_cap * 2 : 32;
                entry->branch_buf = realloc(entry->branch_buf, entry->branch_cap);
            }
            entry->branch_buf[entry->branch_len++] = 0;
            entry->pending_cf = 0;
        }

        uint64_t entry_cid = entry->call_id;

        if (entry->branch_len > 0) {
            PyObject *ba = PyByteArray_FromStringAndSize(
                (const char *)entry->branch_buf, entry->branch_len);
            if (ba) {
                ((CallRecordObject *)entry->record)->control_flow = ba;
            }
        }

        frame_stack_pop(frame_obj);
        if (entry_cid == cid) break;
    }
    return 0;
}

/* ---- profile hook for C_CALL/C_RETURN container dispatch ---- */

typedef enum {
    STASH_NONE = 0,
    STASH_LIST_APPEND,
    STASH_LIST_EXTEND,
    STASH_LIST_INSERT,
    STASH_LIST_POP,
    STASH_LIST_REMOVE,
    STASH_LIST_CLEAR,
    STASH_DICT_CLEAR,
    STASH_SET_ADD,
    STASH_SET_DISCARD,
    STASH_SET_REMOVE,
    STASH_SET_POP,
    STASH_SET_UPDATE,
    STASH_SET_CLEAR,
    STASH_DEQUE_APPEND,
    STASH_DEQUE_APPENDLEFT,
    STASH_DEQUE_EXTEND,
    STASH_DEQUE_EXTENDLEFT,
    STASH_DEQUE_POP,
    STASH_DEQUE_POPLEFT,
    STASH_DEQUE_REMOVE,
    STASH_DEQUE_CLEAR,
} StashType;

typedef struct {
    StashType type;
    PyObject *self_obj;
    Py_ssize_t pre_len;
} CCallStash;

static _Thread_local CCallStash tl_stash = {0};

static StashType classify_container_call(ObjectTraceData *td, const char *name) {
    switch (td->type) {
    case CONTAINER_LIST:
        if (strcmp(name, "append") == 0) return STASH_LIST_APPEND;
        if (strcmp(name, "extend") == 0) return STASH_LIST_EXTEND;
        if (strcmp(name, "insert") == 0) return STASH_LIST_INSERT;
        if (strcmp(name, "pop") == 0)    return STASH_LIST_POP;
        if (strcmp(name, "remove") == 0) return STASH_LIST_REMOVE;
        if (strcmp(name, "clear") == 0)  return STASH_LIST_CLEAR;
        break;
    case CONTAINER_DICT:
        if (strcmp(name, "clear") == 0)  return STASH_DICT_CLEAR;
        break;
    case CONTAINER_SET:
        if (strcmp(name, "add") == 0)     return STASH_SET_ADD;
        if (strcmp(name, "discard") == 0) return STASH_SET_DISCARD;
        if (strcmp(name, "remove") == 0)  return STASH_SET_REMOVE;
        if (strcmp(name, "pop") == 0)     return STASH_SET_POP;
        if (strcmp(name, "update") == 0)  return STASH_SET_UPDATE;
        if (strcmp(name, "clear") == 0)   return STASH_SET_CLEAR;
        break;
    case CONTAINER_DEQUE:
        if (strcmp(name, "append") == 0)      return STASH_DEQUE_APPEND;
        if (strcmp(name, "appendleft") == 0)  return STASH_DEQUE_APPENDLEFT;
        if (strcmp(name, "extend") == 0)      return STASH_DEQUE_EXTEND;
        if (strcmp(name, "extendleft") == 0)  return STASH_DEQUE_EXTENDLEFT;
        if (strcmp(name, "pop") == 0)         return STASH_DEQUE_POP;
        if (strcmp(name, "popleft") == 0)     return STASH_DEQUE_POPLEFT;
        if (strcmp(name, "remove") == 0)      return STASH_DEQUE_REMOVE;
        if (strcmp(name, "clear") == 0)       return STASH_DEQUE_CLEAR;
        break;
    default:
        break;
    }
    return STASH_NONE;
}

static void handle_c_call(PyObject *arg) {
    if (!PyCFunction_Check(arg)) return;
    PyCFunctionObject *cfunc = (PyCFunctionObject *)arg;
    PyObject *self_obj = cfunc->m_self;
    if (!self_obj) return;

    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td || td->type == CONTAINER_NONE) return;

    const char *name = cfunc->m_ml->ml_name;
    StashType st = classify_container_call(td, name);
    if (st == STASH_NONE) return;

    tl_stash.type = st;
    tl_stash.self_obj = self_obj;
    switch (td->type) {
    case CONTAINER_LIST:
        tl_stash.pre_len = PyList_GET_SIZE(self_obj);
        break;
    case CONTAINER_SET:
        tl_stash.pre_len = PySet_GET_SIZE(self_obj);
        break;
    default:
        tl_stash.pre_len = 0;
        break;
    }
}

static void handle_c_return(PyFrameObject *frame_obj) {
    StashType st = tl_stash.type;
    if (st == STASH_NONE) return;
    tl_stash.type = STASH_NONE;

    PyObject *self_obj = tl_stash.self_obj;
    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td) return;

    ARW arw = {0, 0};
    arw.caller_id = get_frame_call_id(frame_obj);
    arw.call_lineno = PyFrame_GetLineNumber(frame_obj);

    switch (st) {
    case STASH_LIST_APPEND: {
        ListTraceData *lt = (ListTraceData *)td;
        arwlist_append(&lt->arws, arw);
        break;
    }
    case STASH_LIST_EXTEND: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        for (Py_ssize_t i = tl_stash.pre_len; i < new_len; i++)
            arwlist_append(&lt->arws, arw);
        break;
    }
    case STASH_LIST_INSERT: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        if (new_len > tl_stash.pre_len) {
            Py_ssize_t idx = new_len - 1;
            for (Py_ssize_t i = (Py_ssize_t)lt->arws.len; i > idx && i > 0; i--) {}
            arwlist_append(&lt->arws, arw);
        }
        break;
    }
    case STASH_LIST_POP: {
        ListTraceData *lt = (ListTraceData *)td;
        if (lt->arws.len > 0) {
            ARW read_arw = arwlist_pop(&lt->arws);
            emit_read(&read_arw);
        }
        break;
    }
    case STASH_LIST_REMOVE: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        if (new_len < tl_stash.pre_len && lt->arws.len > (size_t)new_len) {
            lt->arws.len = (size_t)new_len;
        }
        break;
    }
    case STASH_LIST_CLEAR: {
        ListTraceData *lt = (ListTraceData *)td;
        arwlist_free(&lt->arws);
        arwlist_init(&lt->arws);
        break;
    }
    case STASH_DICT_CLEAR: {
        DictTraceData *dt = (DictTraceData *)td;
        arwdict_free(&dt->arws);
        arwdict_init(&dt->arws);
        break;
    }
    case STASH_SET_ADD: {
        SetTraceData *st_data = (SetTraceData *)td;
        if (PySet_GET_SIZE(self_obj) > tl_stash.pre_len) {
            /* New element was added — we don't have the hash,
               but the set grew, so record a generic ARW */
        }
        break;
    }
    case STASH_SET_CLEAR: {
        SetTraceData *st_data = (SetTraceData *)td;
        arwset_free(&st_data->arws);
        arwset_init(&st_data->arws);
        break;
    }
    case STASH_DEQUE_APPEND: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_append(&dq->arws, arw);
        break;
    }
    case STASH_DEQUE_APPENDLEFT: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_appendleft(&dq->arws, arw);
        break;
    }
    case STASH_DEQUE_EXTEND: {
        DequeTraceData *dq = (DequeTraceData *)td;
        Py_ssize_t added = (Py_ssize_t)arwdeque_len(&dq->arws);
        /* Can't determine exact count without pre-len for deques,
           but the deque has already grown */
        break;
    }
    case STASH_DEQUE_POP: {
        DequeTraceData *dq = (DequeTraceData *)td;
        if (dq->arws.len > 0) {
            ARW read_arw;
            if (arwdeque_pop(&dq->arws, &read_arw) == 0)
                emit_read(&read_arw);
        }
        break;
    }
    case STASH_DEQUE_POPLEFT: {
        DequeTraceData *dq = (DequeTraceData *)td;
        if (dq->arws.len > 0) {
            ARW read_arw;
            if (arwdeque_popleft(&dq->arws, &read_arw) == 0)
                emit_read(&read_arw);
        }
        break;
    }
    case STASH_DEQUE_CLEAR: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_free(&dq->arws);
        arwdeque_init(&dq->arws);
        break;
    }
    default:
        break;
    }
}

static int profile_func(
    PyObject *obj,
    PyFrameObject *frame_obj,
    int what,
    PyObject *arg
) {
    if (!g_state.enabled) return 0;
    switch (what) {
        case PyTrace_C_CALL:
            handle_c_call(arg);
            break;
        case PyTrace_C_RETURN:
            handle_c_return(frame_obj);
            break;
        default:
            break;
    }
    return 0;
}

/* ---- trace callback ---- */

static int trace_func(
    PyObject *obj,
    PyFrameObject *frame_obj,
    int what,
    PyObject *arg
) {
    if (!g_state.enabled) return 0;

    PyObject *py_frame = (PyObject *)frame_obj;
    switch (what) {
        case PyTrace_CALL:   return handle_call(py_frame, frame_obj);
        case PyTrace_LINE:   return handle_line(py_frame, frame_obj);
        case PyTrace_RETURN: return handle_return(py_frame, frame_obj);
        default: return 0;
    }
}

/* ---- Python-visible functions ---- */

static PyObject *py_install(PyObject *self, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"hook", "prefixes", "db", "ownership",
                             "path_filter", "taint_patterns", NULL};
    PyObject *hook, *prefixes_list, *db, *ownership, *path_filter;
    PyObject *taint_patterns = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOOOO|O", kwlist,
            &hook, &prefixes_list, &db, &ownership, &path_filter, &taint_patterns))
        return NULL;

    /* clean up old state */
    Py_XDECREF(g_state.hook_obj);
    Py_XDECREF(g_state.db);
    Py_XDECREF(g_state.ownership);
    Py_XDECREF(g_state.filter);
    if (g_state.prefixes) {
        for (Py_ssize_t i = 0; i < g_state.prefix_count; i++)
            free(g_state.prefixes[i]);
        free(g_state.prefixes);
    }
    if (g_state.taint_patterns) {
        for (Py_ssize_t i = 0; i < g_state.taint_count; i++)
            free(g_state.taint_patterns[i]);
        free(g_state.taint_patterns);
    }
    umap_free(&g_state.scope_cache);
    umap_free(&g_state.object_extras);
    umap_free(&g_state.frame_call_ids);

    /* set up new state */
    Py_INCREF(hook);
    g_state.hook_obj = hook;
    Py_INCREF(db);
    g_state.db = db;
    Py_INCREF(ownership);
    g_state.ownership = ownership;
    Py_INCREF(path_filter);
    g_state.filter = path_filter;

    Py_ssize_t n = PyList_GET_SIZE(prefixes_list);
    g_state.prefixes = malloc(n * sizeof(char *));
    g_state.prefix_count = n;
    for (Py_ssize_t i = 0; i < n; i++) {
        const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(prefixes_list, i));
        g_state.prefixes[i] = strdup(s);
    }

    umap_init(&g_state.scope_cache, 256);
    umap_init(&g_state.object_extras, 1024);
    umap_init(&g_state.frame_call_ids, 1024);

    g_state.taint_patterns = NULL;
    g_state.taint_count = 0;
    if (taint_patterns != Py_None && PyList_Check(taint_patterns)) {
        Py_ssize_t tn = PyList_GET_SIZE(taint_patterns);
        g_state.taint_patterns = malloc(tn * sizeof(char *));
        for (Py_ssize_t i = 0; i < tn; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(taint_patterns, i));
            if (s && *s) {
                g_state.taint_patterns[g_state.taint_count++] = strdup(s);
            }
        }
    }

    g_state.next_call_id = 1;
    g_state.enabled = 1;

    PyEval_SetTrace((Py_tracefunc)trace_func, g_state.hook_obj);
    PyEval_SetProfile((Py_tracefunc)profile_func, g_state.hook_obj);
    Py_RETURN_NONE;
}

static PyObject *py_install_thread(PyObject *self, PyObject *Py_UNUSED(args)) {
    if (!g_state.hook_obj) {
        PyErr_SetString(PyExc_RuntimeError, "tracer not installed");
        return NULL;
    }
    PyEval_SetTrace((Py_tracefunc)trace_func, g_state.hook_obj);
    PyEval_SetProfile((Py_tracefunc)profile_func, g_state.hook_obj);
    Py_RETURN_NONE;
}

static PyObject *py_uninstall(PyObject *self, PyObject *Py_UNUSED(args)) {
    g_state.enabled = 0;
    PyEval_SetTrace(NULL, NULL);
    PyEval_SetProfile(NULL, NULL);
    Py_RETURN_NONE;
}

static PyObject *py_get_call_id(PyObject *self, PyObject *frame) {
    uint64_t cid = get_frame_call_id((PyFrameObject *)frame);
    return PyLong_FromUnsignedLongLong(cid);
}

static PyObject *py_set_call_id(PyObject *self, PyObject *args) {
    PyObject *frame;
    uint64_t cid;
    if (!PyArg_ParseTuple(args, "OK", &frame, &cid))
        return NULL;
    set_frame_call_id((PyFrameObject *)frame, cid);
    Py_RETURN_NONE;
}

PyObject *py_current_record(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) Py_RETURN_NONE;
    FrameEntry *entry = frame_stack_peek(frame);
    if (!entry) Py_RETURN_NONE;
    Py_INCREF(entry->record);
    return entry->record;
}

static PyObject *py_load_ast_data(PyObject *self, PyObject *args) {
    PyObject *func_map_dict, *cf_lines_dict;
    if (!PyArg_ParseTuple(args, "OO", &func_map_dict, &cf_lines_dict))
        return NULL;

    /* clear old data */
    SMap_free(&g_state.func_to_id);
    /* free bitsets in cf_bits before freeing the map */
    if (g_state.cf_bits.entries) {
        for (size_t i = 0; i < g_state.cf_bits.capacity; i++) {
            if (g_state.cf_bits.entries[i].occupied) {
                Bitset *bs = (Bitset *)g_state.cf_bits.entries[i].value;
                if (bs) { free(bs->words); free(bs); }
            }
        }
    }
    SMap_free(&g_state.cf_bits);

    SMap_init(&g_state.func_to_id, 512);
    SMap_init(&g_state.cf_bits, 256);
    g_state.next_func_id = 0;

    /* load func_map */
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(func_map_dict, &pos, &key, &value)) {
        const char *ref_str = PyUnicode_AsUTF8(key);
        long id = PyLong_AsLong(value);
        if (ref_str && !(id == -1 && PyErr_Occurred())) {
            SMap_set(&g_state.func_to_id, ref_str, (void *)(intptr_t)(int32_t)id);
            if ((int32_t)id >= g_state.next_func_id)
                g_state.next_func_id = (int32_t)id + 1;
        } else {
            PyErr_Clear();
        }
    }

    /* load cf_lines */
    pos = 0;
    while (PyDict_Next(cf_lines_dict, &pos, &key, &value)) {
        const char *ref_str = PyUnicode_AsUTF8(key);
        if (!ref_str) { PyErr_Clear(); continue; }
        Bitset bs = bitset_from_pyset(value);
        if (bs.max_line >= 0) {
            Bitset *heap_bs = malloc(sizeof(Bitset));
            *heap_bs = bs;
            SMap_set(&g_state.cf_bits, ref_str, heap_bs);
        }
    }

    Py_RETURN_NONE;
}

static PyObject *py_get_func_map(PyObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;
    if (!g_state.func_to_id.entries) return dict;

    for (size_t i = 0; i < g_state.func_to_id.capacity; i++) {
        if (g_state.func_to_id.entries[i].occupied) {
            PyObject *key = PyUnicode_FromString(g_state.func_to_id.entries[i].key);
            int32_t id = (int32_t)(intptr_t)g_state.func_to_id.entries[i].value;
            PyObject *val = PyLong_FromLong(id);
            PyDict_SetItem(dict, key, val);
            Py_DECREF(key);
            Py_DECREF(val);
        }
    }
    return dict;
}

static PyMethodDef hook_methods[] = {
    {"install",        (PyCFunction)py_install,        METH_VARARGS | METH_KEYWORDS, NULL},
    {"install_thread", py_install_thread,              METH_NOARGS, NULL},
    {"uninstall",      py_uninstall,                   METH_NOARGS, NULL},
    {"get_call_id",    py_get_call_id,                 METH_O, NULL},
    {"set_call_id",    (PyCFunction)py_set_call_id,    METH_VARARGS, NULL},
    {"current_record", py_current_record,              METH_NOARGS, NULL},
    {"load_ast_data",  (PyCFunction)py_load_ast_data,  METH_VARARGS, NULL},
    {"get_func_map",   py_get_func_map,                METH_NOARGS, NULL},
    {NULL}
};

int hook_init(PyObject *module) {
    ExtraCleanupType = (PyTypeObject *)PyType_FromSpec(&ExtraCleanup_spec);
    if (!ExtraCleanupType) return -1;

    for (PyMethodDef *m = hook_methods; m->ml_name; m++) {
        PyObject *func = PyCFunction_NewEx(m, NULL, NULL);
        if (!func) return -1;
        if (PyModule_AddObject(module, m->ml_name, func) < 0) {
            Py_DECREF(func);
            return -1;
        }
    }
    return 0;
}
