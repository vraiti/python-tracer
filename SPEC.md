# D3G Specification

**Status:** Draft, derived from source inspection. Not yet reviewed against
project intent. Statements without a file citation are inference.

## 1. Purpose

D3G (Dynamic Data Dependency Graph) is a modified CPython interpreter and an
associated postprocessing pipeline that reconstructs, for a single execution
of a Python program, a complete data dependency graph: for every value
produced by every function call, method call, or object, the graph records
its origin and every downstream consumer.

This is a superset of a call graph. A call graph records *who called whom*.
D3G additionally resolves, per call, which specific argument, attribute
read, return value, or object member produced each value consumed
downstream.

## 2. Design

D3G separates data collection from data-dependency resolution into two
phases.

### 2.1 Runtime phase (C, in-process)

Hooks embedded in the CPython evaluation loop and a fixed set of
object/type slots record a minimal trace during execution: call/return
events, branch outcomes, attribute and item reads/writes, global and
closure-cell accesses, object construction, and IPC/IO channel usage. This
trace is written incrementally to an in-memory structure and serialized to
a per-process SQLite database at interpreter shutdown. The runtime phase
performs no dependency resolution; it only records the raw events required
to reconstruct it later.

### 2.2 Postprocessing phase (Python, offline)

`Lib/d3g/postprocess.py` parses the AST of every source file referenced by
the trace, replays each call's recorded control-flow bitstring against its
function's AST to determine the exact sequence of executed statements, and
performs symbolic dataflow analysis over that sequence to produce a graph
of typed edges between values. Per-call subgraphs are then merged into a
single dependency graph using object-membership and cross-process IPC
data.

## 3. Component Map

| Path | Role |
|---|---|
| `Modules/_tracer/hook.c`, `hook.h` | Hook implementations (`d3g_*_hook`), global trace state (`g_state`), `install()`/`install_thread()`/`uninstall()` |
| `Modules/_tracer/records.c`, `records.h` | SQLite schema, in-memory record structures (`CallRecordData`, `ObjectRecordData`, `DatabaseObject`), serialization |
| `Modules/_tracer/filter.c`, `filter.h` | `PathFilterObject`: compiled form of the trace config (traced path prefixes, tracked class names) |
| `Modules/_tracer/hashmap.c`, `hashmap.h` | Generic pointer-keyed hashmap (object extras, scope cache, function-id interning) |
| `Modules/_tracer/ownership.c`, `ownership.h` | Object ownership helper (not fully reviewed) |
| `Modules/_tracer/containers/` | Per-container-type (list/dict/set/deque) tracking scaffolding; see §7.1 |
| `Include/tracer_hooks.h`, `Modules/_tracer/tracer_hooks.h` | Public hook declarations (identical copies) |
| `Python/bytecodes.c`, `Objects/abstract.c`, `Objects/object.c`, `Objects/typeobject.c`, `Objects/dictobject.c`, `Objects/listobject.c`, `Objects/setobject.c`, `Modules/_collectionsmodule.c`, `Modules/main.c` | Interpreter-core hook call sites |
| `Modules/_io/fileio.c`, `Modules/posixmodule.c`, `Modules/socketmodule.c`, `Modules/mmapmodule.c`, `Modules/signalmodule.c`, `Modules/_multiprocessing/` | I/O and IPC hook call sites |
| `Lib/d3g/_bootstrap.py` | Config load, `PathFilter`/`Database` construction, `_tracer.install()` invocation |
| `Lib/d3g/__main__.py` | `python -m d3g -- script.py` entry point; child-process wait and raw-row merge |
| `Lib/d3g/postprocess.py` | AST-based dependency-graph reconstruction |

## 4. Activation

Tracing is opt-in per process, gated on the `PYTHON_TRACER_CONFIG`
environment variable (`Modules/main.c: pymain_run_python`). When set, the
interpreter imports `d3g._bootstrap` and calls `init()` before executing
user code, and serializes the in-memory database to
`$PYTHON_TRACER_OUTDIR/{getpid()}.db` at shutdown.

`init()` (`Lib/d3g/_bootstrap.py`) reads the YAML config at
`PYTHON_TRACER_CONFIG`:

- `modules`: list of importable module names; resolved via
  `importlib.util.find_spec` to filesystem path prefixes. Only calls whose
  code originates under one of these prefixes are traced.
- `classes`: list of class names. Instances of matching classes receive
  object-level tracking (construction, member graph) regardless of the
  module filter, if their `__init__` is defined under a traced prefix.
- `taint-functions`: optional list of qualname substring patterns; see §6.

Every process that inherits `PYTHON_TRACER_CONFIG` — including forked
children — traces and serializes independently. Cross-process trace
reconciliation happens outside the runtime (§8).

## 5. Hook Inventory

| Hook | Trigger | Effect |
|---|---|---|
| `d3g_py_call_hook` / `d3g_py_return_hook` | `RESUME` / `RETURN_VALUE` | Row in `calls`; establishes call-graph edge via `caller_id` |
| `d3g_c_call_hook` / `d3g_c_return_hook` | Call into a C-implemented callable | Tracks calls into tracked-object methods that bypass the Python call path (e.g. `list.append`) |
| `d3g_branch_hook` | `POP_JUMP_IF_*`, `FOR_ITER` family | Appends one bit to the current call's control-flow bitstring |
| `d3g_object_new_hook` | End of `object.__new__`, for a type matching `classes` or whose `__init__` is under a traced prefix | Row in `objects`; begins member tracking |
| `d3g_getattr_hook` / `d3g_setattr_hook` | `PyObject_GenericGetAttr` / `SetAttr` | Attribute read/write tracking; feeds `attr_reads` and `members` |
| `d3g_getitem_hook` / `d3g_setitem_hook` | `PyObject_GetItem` / `SetItem` | Equivalent tracking for subscript access |
| `d3g_global_load_hook` / `_store_hook` / `_delete_hook` | `LOAD_GLOBAL`, `STORE_GLOBAL`, `DELETE_GLOBAL`, `LOAD_FROM_DICT_OR_GLOBALS` | Treats each module `__dict__` as an implicitly tracked object |
| `d3g_deref_load_hook` / `_store_hook` | `LOAD_DEREF`, `STORE_DEREF` | Equivalent tracking for closure cells |
| `d3g_container_dealloc_hook` | `dict`/`list`/`set`/`deque` deallocation | Releases tracking state for a tracked container; see §7.2 |
| `d3g_shm_open_hook`, `d3g_pipe_hook`, `d3g_mkfifo_hook`, `d3g_socket_hook`, `d3g_mmap_create_hook`, `d3g_mmap_read_hook`, `d3g_mmap_write_hook`, `d3g_fileio_open_hook`, `d3g_fileio_read_hook`, `d3g_fileio_write_hook`, `d3g_sem_acquire_hook`, `d3g_sem_release_hook`, `d3g_signal_hook`, `d3g_after_fork_child_hook` | Corresponding syscall/library entry point | Rows in `ipc` / `io_objects` / `io_ops`, keyed by channel name, for cross-process dataflow resolution |

## 6. Scope Exclusion ("Taint")

`handle_call` (`hook.c`) implements a call-subtree exclusion mechanism. If
a call's qualified name matches a `taint-functions` pattern, that call and
every descendant call are assigned the sentinel `call_id` `UINT64_MAX` and
excluded from tracing (`hook.c:563–604`), propagated via inherited
`call_id` on the caller's frame. No usage example or test coverage for
this mechanism has been found; its intended application (e.g. excluding
known-noisy library subtrees) is inferred from behavior, not documented
elsewhere.

## 7. Container-Instance Tracking

### 7.1 Attachment

`classify_container_call` / `handle_c_call` (`hook.c`) implement dispatch
for container mutation methods (`.append()`, `.add()`, etc.) keyed on a
per-object `type` field (`CONTAINER_LIST`/`DICT`/`SET`/`DEQUE`). This
dispatch requires trace data typed to the correct `ContainerType` to have
been attached to the container instance in advance; originally, no code
path set this field to anything other than `CONTAINER_NONE` except the
module-globals-as-dict path, leaving per-instance container tracking
unreachable.

This has been resolved: `d3g_setattr_hook` now classifies a newly assigned
attribute value via `classify_container_type` (`PyDict_Check`,
`PyList_Check`, `PyAnySet_Check`, and a `tp_name` comparison for `deque`,
which has no public type-check macro) and, if the value is an untracked
container, attaches typed trace data via
`attach_typed_container_trace_data` before recording the membership edge.
This activates container tracking at the point a container becomes
reachable as an attribute of a tracked object — e.g. `self.tags = {}` on a
tracked instance. Container-typed trace data is allocated at the correct
subtype size (`DictTraceData`/`ListTraceData`/`SetTraceData`/
`DequeTraceData`) with its type-specific ARW structure initialized
(`arwdict_init`/`arwlist_init`/`arwset_init`/`arwdeque_init`), consistent
with what `free_trace_data`'s existing per-type switch already expected on
deallocation.

Attachment via item assignment (`d3g_setitem_hook`, e.g. a container
stored inside another container) and at construction time
(`d3g_object_new_hook`) is not yet implemented; only the attribute-assignment
path is covered.

### 7.2 Finalization

Object lifetime tracking originally used a `PyWeakref_NewRef` finalizer
uniformly. Built-in `dict` and `list` do not support weakrefs; this
crashed (`TypeError: cannot create weak reference to 'dict' object`) the
first time traced code touched a module global. Finalization for `dict`,
`list`, `set`, and `deque` has been moved to direct hooks in each type's
`tp_dealloc` (`d3g_container_dealloc_hook`); weakref-based finalization is
retained only for arbitrary heap-type instances tracked via
`object_new_hook`, which always support weakrefs. Resolved during the
3.12 port (§11); carried forward into 3.14.

## 8. Trace Data Model

All tables except `meta`, `machine`, and `functions` are keyed first by
`pid`, since a traced execution may span multiple processes.

```
meta(pid)
machine(machine_id)
functions(function_id PK, ref)
calls(pid, call_id, function_id, caller_id, call_lineno, obj_id,
      control_flow, PK(pid, call_id))
attr_reads(pid, call_id, caller_id, write_call_lineno, read_call_lineno)
objects(pid, obj_idx, call_id, PK(pid, obj_idx))
members(pid, obj_idx, attr, child_idx)
ipc(pid, name, obj_idx)
io_objects(pid, io_object_id, name, offset, PK(pid, io_object_id))
io_ops(pid, io_object_id, call_id, offset, length, op_type)
```

- `functions.ref` — `"{absolute_filepath}:{qualname}"`.
- `calls.control_flow` — a packed, LSB-first bitstring; one bit per branch
  decision (`if`/`while`-iteration/`for`-iteration) taken during the call.
- `machine.machine_id` — provenance check; postprocessing refuses to run
  against a trace collected on a different machine, since `functions.ref`
  contains absolute paths re-parsed from disk during postprocessing.

Postprocessing adds three tables to the same file:

```
dataflow_edges(pid, source_call_id, source_type, source_name,
               target_pid, target_call_id, target_type, target_name,
               member_path)
executed_lines(pid, call_id, line_order, lineno)
default_owner(pid, obj_idx, owner_idx, attr, PK(pid, obj_idx))
```

## 9. Postprocessing Algorithm

Entry point: `postprocess(db_path)`, invocable as
`python -m d3g.postprocess <db>`.

1. **Load.** All tables loaded into memory, keyed by `(pid, ...)`.
2. **Machine check.** Abort if `machine.machine_id` does not match
   `/etc/machine-id` of the executing host.
3. **AST resolution.** For each call, the source file is parsed (cached
   per file) and the matching `FunctionDef`/`AsyncFunctionDef` node is
   located by walking the dotted qualname (`Class.method`,
   `outer.<locals>.inner`, etc.) through the module AST.
4. **Control-flow replay** (`reconstruct_executed_stmts`). The call's
   `control_flow` bitstring is consumed one bit per branch decision to
   produce the ordered list of statements that executed, recursing into
   the taken branch of each `if`/`for`/`while`/`with`/`try`.
5. **Intra-call dataflow resolution** (`resolve_intra_call`). The executed
   statement sequence is walked as a symbolic interpreter: each local name
   is bound to a `ValueSource` (`param`, `attr_read`, `call_result`,
   `literal`, or `composite`). Assignments, augmented assignments,
   attribute writes, call arguments, and return values each emit a
   `DataflowEdge` from the resolved source to the consuming call/attribute.
6. **Cross-call merge** (`merge_graphs`). Edges whose source is an
   attribute read are resolved against `members` to link the edge to the
   call that actually constructed the value stored at that attribute,
   rather than terminating at the attribute name.
7. **IPC edge resolution** (`resolve_ipc_edges`). For each named channel
   in `ipc` with endpoints in more than one process, a bidirectional
   `DataflowEdge` is added between the calls that used that channel. This
   is the only step that produces cross-process edges.
8. **Default ownership.** For each object with more than one referencing
   parent in `members`, the parent whose call constructed the object
   (i.e. wrote to it during the child's own `__init__` call) is preferred
   as the "default owner"; otherwise the first-observed parent is used.
   Ownership edges are then treated as a graph and any cycles are broken
   by removing the most-recently-created edge in each cycle, guaranteeing
   `default_owner` is acyclic.
9. **Write.** `dataflow_edges`, `executed_lines`, and `default_owner` are
   written back into the same database file.

## 10. Multi-Process Execution Model

Two independent merge operations exist:

- **Raw-row merge** (`Lib/d3g/__main__.py`). After the top-level script
  exits, `_wait_for_traces` polls until every PID with a `.db` file in the
  output directory has exited, then performs a SQL `ATTACH DATABASE` union
  of every process's `{pid}.db` into a single `trace.db`, keyed by the
  existing `pid` column, and removes the originals. This step performs no
  dependency resolution; it exists to present postprocessing with one file
  instead of many.
- **IPC dataflow merge** (`postprocess.resolve_ipc_edges`, §9 step 7).
  The only step that produces cross-process dependency edges.

## 11. CPython Integration Status

| Version | Status | Reference |
|---|---|---|
| 3.12 | Complete | Tag `d3g-3.12-port` |
| 3.14 | Complete | Branch `d3g-3.14` |

Both hook sets were originally hand-patched into
`Python/generated_cases.c.h`, the *generated* eval-loop file, rather than
into `Python/bytecodes.c`, the DSL source `make regen-cases` regenerates
from. This has been corrected: all hook sites now live in `bytecodes.c`
and have been verified to regenerate byte-identically.

`Modules/_tracer/` was not registered in the build system for either
version (absent from `configure.ac` and `Modules/Setup*`) despite being
called directly, at static-link time, from core interpreter files. It has
been added as a `*static*` entry in `Modules/Setup.bootstrap.in` with
`-lsqlite3` for both versions.

The 3.14 port required accounting for the following upstream changes:

- `_PyInterpreterFrame` relocation (`pycore_frame.h` →
  `pycore_interpframe_structs.h`); `f_code` → `f_executable`
  (`_PyFrame_GetCode(frame)` required in place of direct field access);
  locals/stack slots changed from `PyObject *` to `_PyStackRef`.
- `RESUME` restructured from a single `inst()` to a `macro()` of composed
  `op()`s.
- `RETURN_CONST` removed (folded into `RETURN_VALUE`).
- The four `POP_JUMP_IF_*` opcodes collapsed onto two shared primitives
  (`_POP_JUMP_IF_TRUE`/`_FALSE`), reducing branch-hook sites from four to
  two with unchanged coverage.
- `FOR_ITER` and `CALL` rebuilt as macros of reusable `op()`s, including a
  free-threaded (`Py_GIL_DISABLED`) code path requiring an additional
  hook call for parity.
- `mmapmodule.c` methods split into `_lock_held` helpers for
  free-threading and renamed via Argument Clinic.

Both versions are currently built and verified only with
`--with-pydebug`. A release (non-debug) build has not been verified for
either version.

## 12. Open Issues

- Test harness (`tests/test.py`) selects the trace database via
  lexicographically-last filename among per-process `.db` files, which is
  incorrect whenever a forked child process has a numerically larger PID
  than the parent. The merged `trace.db` produced by `__main__.py` (§10)
  should be used instead. Not yet fixed. The current test application has
  been reduced to single-process (no forking) to avoid this issue pending
  a fix.
- Container-instance tracking (§7.1) is attached only via attribute
  assignment; item assignment and construction-time attachment are not
  yet implemented.
- The taint-exclusion mechanism (§6) has no test coverage.
- No release-build verification (§11) for either supported CPython
  version.
- This document has not been reviewed against the project owner's
  intended design; sections describing observed behavior as "unfinished"
  may instead reflect intentional scope.
