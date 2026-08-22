# D3G Specification

**Status:** Describes the tree as of 2026-08-22 (CPython branch `d3g-3.14`).
Statements carry file citations where the behaviour is non-obvious.

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
closure-cell accesses, object construction, and IPC/IO channel usage.
Records are built in process memory only while they are open (a call until
it returns, an object until it is deallocated); each completed record is
handed to a writer thread that appends it to a per-process SQLite database
while the program runs (§8). The runtime phase performs no dependency
resolution; it only records the raw events required to reconstruct it
later.

### 2.2 Postprocessing phase (Python, offline)

`Lib/d3g/postprocess.py` unions the per-process databases, parses the AST
of every source file referenced by the trace, replays each call's recorded
control-flow bitstring against its function's AST to determine the exact
sequence of executed statements, and performs symbolic dataflow analysis
over that sequence to produce a graph of typed edges between values.
Per-call subgraphs are then merged into a single dependency graph using
object-membership and cross-process IPC data.

## 3. Component Map

Paths are relative to `cpython/` unless stated otherwise.

| Path | Role |
|---|---|
| `Modules/_tracer/hook.c`, `hook.h` | Hook implementations (`d3g_*_hook`), global trace state (`g_state`), per-thread frame stacks, `install()`/`uninstall()`, `d3g_flush_trace()` |
| `Modules/_tracer/records.c`, `records.h` | In-memory record structures (`CallRecordData`, `IoObjectRecord`, `DatabaseObject` with the live-call list) |
| `Modules/_tracer/writer.c`, `writer.h` | Ring buffer, writer thread, SQLite schema and per-process `{pid}.db` output (§8) |
| `Modules/_tracer/signals.c` | Transparent interception of `SIG_DFL` terminate-class signals so the trace is flushed before the process dies (§9) |
| `Modules/_tracer/filter.c`, `filter.h` | `PathFilterObject`: compiled form of the trace config (traced path prefixes, tracked class names) |
| `Modules/_tracer/hashmap.c`, `hashmap.h` | Pointer-keyed (`UMap`) and string-keyed (`SMap`) hashmaps |
| `Modules/_tracer/ownership.c`, `ownership.h` | Module registration helper for the ownership type |
| `Modules/_tracer/containers/` | Per-container-type (list/dict/set/deque) access-read/write tracking (`ARW*`); see §7 |
| `Include/tracer_hooks.h`, `Modules/_tracer/tracer_hooks.h` | Public hook declarations. Two copies that must be kept identical; `Modules/_tracer/*.c` resolve to the local one |
| `Python/bytecodes.c` (and the regenerated `generated_cases.c.h`, `executor_cases.c.h`), `Objects/abstract.c`, `object.c`, `typeobject.c`, `dictobject.c`, `listobject.c`, `setobject.c`, `tupleobject.c`, `bytearrayobject.c`, `Modules/_collectionsmodule.c` | Interpreter-core hook call sites |
| `Modules/main.c`, `Python/pylifecycle.c`, `Modules/signalmodule.c`, `Modules/posixmodule.c` | Activation, flush-at-exit, and signal chokepoints (§4, §9) |
| `Modules/_io/fileio.c`, `Modules/posixmodule.c`, `Modules/socketmodule.c`, `Modules/mmapmodule.c`, `Modules/signalmodule.c`, `Modules/_multiprocessing/posixshmem.c`, `semaphore.c` | I/O and IPC hook call sites |
| `Lib/d3g/_bootstrap.py` | Config load, `PathFilter`/`Database` construction, `_tracer.install()`; patches `threading.Thread.run` to call `install_thread()` |
| `Lib/d3g/__main__.py` | `python -m d3g -- script.py [args]` entry point; runs the script and calls `uninstall()` in a `finally` |
| `Lib/d3g/postprocess.py` | Offline merge of per-process traces into `trace.db`, then AST-based dependency-graph reconstruction |
| `../tests/test.py`, `../tests/testapp/`, `../tests/testapp-config.yaml` | End-to-end test (§13) |
| `../configs/*.yaml`, `../scripts/*.sh` | Configurations and helper scripts for tracing a vLLM-Omni server (§13) |

`Modules/_tracer/` is built into the interpreter as a `*static*` entry in
`Modules/Setup.bootstrap.in` (linked with `-lsqlite3`). The tree is
currently built and tested as a release (non-debug) build.

## 4. Activation

Tracing is opt-in per process, gated on the `PYTHON_TRACER_CONFIG`
environment variable (`Modules/main.c: pymain_run_python`). When set, the
interpreter imports `d3g._bootstrap` and calls `init()` before executing
user code, then calls `d3g_enumerate_fds()` so descriptors already open at
startup (inherited pipes, sockets, files) appear in `io_objects`.

`init()` (`Lib/d3g/_bootstrap.py`) reads the YAML config at
`PYTHON_TRACER_CONFIG`, creates `PYTHON_TRACER_OUTDIR`, and calls
`_tracer.install()`:

- `modules`: list of importable module names; resolved via
  `importlib.util.find_spec` to filesystem path prefixes. Only calls whose
  code originates under one of these prefixes are traced.
- `classes`: list of class names. Instances of matching classes receive
  object-level tracking (construction, member graph) regardless of the
  module filter, if their `__init__` is defined under a traced prefix.
- `taint-functions`: optional list of qualname substring patterns; see §6.
- `traceall`: optional boolean. When true, every call is traced and every
  class receives object-level tracking, regardless of `modules` and
  `classes`; `taint-functions` still excludes matching subtrees.

`install()` starts the writer thread (§8) and substitutes the `SIG_DFL`
signal handlers (§9) before enabling the hooks.

Every process that inherits `PYTHON_TRACER_CONFIG` — forked children and
`exec`ed subprocesses alike — traces independently. A process that never
records a call never creates a database (§8), so incidental helper
processes do not litter the output directory. Cross-process reconciliation
happens offline (§11).

### 4.1 Exit paths that complete the trace

`d3g_flush_trace()` (`hook.c`) disables the hooks, restores the signal
dispositions, hands every still-open call and object record to the writer,
and joins the writer thread, so `{pid}.db` is complete on return. It is
idempotent and is reached from:

| Path | Site |
|---|---|
| Normal return, `sys.exit`, uncaught exception | `Modules/main.c: pymain_run_python` (`done:` label, before finalization) and, for embedders that bypass `Py_RunMain`, `Python/pylifecycle.c: _Py_Finalize` (after `atexit` callbacks) |
| `os._exit()` | `Modules/posixmodule.c: os__exit_impl` |
| Death by a `SIG_DFL` signal | `Modules/_tracer/signals.c` (§9) |
| `_tracer.uninstall()` | `Lib/d3g/__main__.py` `finally`, or explicit call |

`SIGKILL`, `SIGSTOP`, fault signals (`SIGSEGV` etc.) and hard crashes are not
covered; the loss in that case is bounded by §8.

## 5. Hook Inventory

| Hook | Trigger | Effect |
|---|---|---|
| `d3g_py_call_hook` / `d3g_py_return_hook` | `RESUME` / `RETURN_VALUE` | Opens / completes a `calls` row; establishes the call-graph edge via `caller_id` |
| `d3g_c_call_hook` / `d3g_c_return_hook` | Call into a C-implemented callable | Tracks calls into tracked-container methods that bypass the Python call path (`list.append`, `deque.popleft`, …) |
| `d3g_branch_hook` | `_POP_JUMP_IF_TRUE/_FALSE`, `_FOR_ITER` family | Appends one bit to the current call's control-flow bitstring |
| `d3g_gen_iter_hook` | `YIELD_VALUE` / `RETURN_VALUE` of a generator frame inlined by `FOR_ITER_GEN` | Supplies the `for` bit that `FOR_ITER_GEN` cannot record itself (1 = exhausted) |
| `d3g_object_new_hook` | End of `object.__new__`, for a type matching `classes` or whose `__init__` is under a traced prefix (or any type under `traceall`) | Begins member tracking; the `objects` row is emitted at deallocation |
| `d3g_getattr_hook` / `d3g_setattr_hook` | `PyObject_GenericGetAttr` / `SetAttr` | Attribute read/write tracking; feeds `attr_reads` and `members`; attaches container tracking to newly assigned container values (§7) |
| `d3g_getitem_hook` / `d3g_setitem_hook` | `PyObject_GetItem` / `SetItem` | Equivalent tracking for subscript access, including container attachment |
| `d3g_global_load_hook` / `_store_hook` / `_delete_hook` | `LOAD_GLOBAL`, `STORE_GLOBAL`, `DELETE_GLOBAL`, `LOAD_FROM_DICT_OR_GLOBALS` | Treats each module `__dict__` as an implicitly tracked object |
| `d3g_deref_load_hook` / `_store_hook` | `LOAD_DEREF`, `STORE_DEREF` | Equivalent tracking for closure cells |
| `d3g_container_dealloc_hook` | `tp_dealloc` of `dict`, `list`, `set`, `deque`, `tuple`, `bytearray`, and `subtype_dealloc` for heap-type instances | Emits the `objects`/`members` rows for a tracked object and releases its tracking state (§7.2) |
| `d3g_shm_open_hook`, `d3g_pipe_hook`, `d3g_mkfifo_hook`, `d3g_socket_hook`, `d3g_sem_acquire_hook`, `d3g_sem_release_hook`, `d3g_signal_hook` | Corresponding library entry point | Row in `ipc`, keyed by channel name (`shm:`, `pipe:`, `fifo:`, `sock:`, `sem:`, `signal:N`) |
| `d3g_mmap_create_hook`, `d3g_mmap_read_hook`, `d3g_mmap_write_hook`, `d3g_fileio_open_hook`, `d3g_fileio_read_hook`, `d3g_fileio_write_hook` | `mmap` / `FileIO` methods | Rows in `io_objects` / `io_ops` |
| `d3g_enumerate_fds` | Once, after `init()` | `io_objects` rows for every descriptor in `/proc/self/fd` |
| `d3g_after_fork_child_hook` | `os.fork()` child (`posixmodule.c`) | Discards the parent's history in the child and re-creates records for the calls that are active across the fork, keeping their `call_id`s (§11) |

Thread support: `_bootstrap.init()` patches `threading.Thread.run` to call
`install_thread()`; per-thread frame stacks are thread-local in `hook.c`.
Calls made from a non-main thread record `caller_id = 0`.

## 6. Scope Exclusion ("Taint")

`handle_call` (`hook.c`) implements a call-subtree exclusion mechanism. If
a call's qualified name matches a `taint-functions` pattern, that call and
every descendant call are assigned the sentinel `call_id` `UINT64_MAX` and
excluded from tracing, propagated via inherited `call_id` on the caller's
frame. Writes to tracked objects made inside an excluded subtree are still
observed by the object, but attributed to caller 0 when serialized
(`writer.c`), so a later read depends on "an untraced writer" rather than
on the excluded call. The test application exercises this with
`scratch_noise` (`tests/testapp-config.yaml`).

## 7. Container-Instance Tracking

### 7.1 Attachment

`classify_container_call` / `handle_c_call` (`hook.c`) dispatch container
mutation methods (`.append()`, `.add()`, `.popleft()`, …) keyed on a
per-object `type` field (`CONTAINER_LIST`/`DICT`/`SET`/`DEQUE`/`TUPLE`/
`BYTEARRAY`). Typed trace data is attached lazily at the moment a container
becomes reachable from a tracked object:

- attribute assignment on a tracked instance (`d3g_setattr_hook`), e.g.
  `self.tags = {}`;
- item assignment into a tracked container (`d3g_setitem_hook`), e.g. a
  container stored inside another container;
- module globals, which are treated as a tracked `dict`.

`classify_container_type` uses `PyDict_Check`, `PyList_Check`,
`PyAnySet_Check`, `PyTuple_Check`, `PyByteArray_Check`, and a `tp_name`
comparison for `deque` (which has no public type-check macro). Container
data is allocated at the correct subtype size (`DictTraceData`,
`ListTraceData`, `SetTraceData`, `DequeTraceData`) with its ARW structure
initialised; `tuple` and `bytearray` receive plain trace data (read-only
membership, no mutation dispatch).

`str`, `bytes` and other immutable scalars are deliberately not containers:
every operation on them yields a new object whose dependencies are already
carried by the value-level dataflow, their elements have no identity, and
interning would alias unrelated sites.

### 7.2 Finalization

All tracked objects are finalised from their type's `tp_dealloc`
(`d3g_container_dealloc_hook`): the per-type dealloc functions for
`dict`/`list`/`set`/`deque`/`tuple`/`bytearray`, and `subtype_dealloc` for
heap-type instances. The `objects` and `members` rows are emitted at that
point (the member map is moved to the writer). Weakref callbacks are not
used: `dict` and `list` do not support weakrefs, and the earlier
implementation released its weakref immediately so the callback never
ran.

## 8. Online Serialization

`Modules/_tracer/writer.c` implements a single-producer-side, single-consumer
pipeline:

- **Ring.** A static ring of 65,536 `TraceEvent` slots (`EV_CALL`,
  `EV_OBJECT`, `EV_FUNCTION`, `EV_IPC`, `EV_IO_OBJECT`, `EV_IO_OP`,
  `EV_STOP`). Producers are the hooks, which always run with the GIL held;
  a full ring blocks the producer until the writer drains it (bounded
  memory, back-pressure on the program). Every push transfers ownership of
  the payload to the writer.
- **Writer thread.** Started by `install()`; never touches the interpreter.
  It drains up to 4,096 events per SQLite transaction
  (`PRAGMA synchronous=OFF`) into `$PYTHON_TRACER_OUTDIR/{getpid()}.db`.
- **Lazy open.** Events that arrive before the first `EV_CALL` are buffered
  privately; the database is created on the first call and the buffer is
  discarded at stop if no call ever arrived. Processes that merely inherit
  the environment (e.g. `multiprocessing`'s resource tracker) therefore
  leave no file.
- **Record lifetimes.** A `CallRecordData` lives in `DatabaseObject`'s
  doubly linked live list from `RESUME` until return, then is unlinked and
  pushed. Object records are pushed at deallocation. `d3g_flush_trace()`
  pushes whatever is still open, then pushes `EV_STOP` and joins.
- **Fork.** `pthread_atfork` handlers park the writer in the parent around
  the fork; the child resets the ring and thread state and abandons (never
  closes) the inherited SQLite handle, which remains the parent's. The
  child starts its own writer on its next `install()`-equivalent path via
  `d3g_after_fork_child_hook`.

Consequence: a process killed by `SIGKILL` still leaves every returned call
and every deallocated object in its database; the loss is bounded to the
ring contents, the open transaction, and the records that were still open.

## 9. Signal Handling

Terminating signals would otherwise bypass every flush path in §4.1.
`Modules/_tracer/signals.c` intercepts them while remaining invisible to
the program:

- **Scope.** Only `SIGTERM`, `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGUSR1`,
  `SIGUSR2`, `SIGALRM`, `SIGPIPE`, `SIGXCPU`, and only while their kernel
  disposition is `SIG_DFL`. Python-level handlers, `SIG_IGN`, and
  `sigaction` calls made directly by C extensions are never touched.
- **Chokepoints.** `d3g_signals_install()` (from `install()`) queries each
  candidate with `sigaction` and substitutes `dfl_handler` where the
  disposition is `SIG_DFL`. `PyOS_setsig` (`pylifecycle.c`) routes every
  later `SIG_DFL` request through `d3g_setsig_substitute`, so a handler
  that restores the default and re-raises is still intercepted, and maps
  the returned old handler back to `SIG_DFL` via `d3g_setsig_report`. The
  signal module's `Handlers[]` table is untouched, so
  `signal.getsignal()` reports `SIG_DFL` exactly as before.
- **Delivery.** `dfl_handler` is async-signal-safe: it marks the signal
  pending and pokes the eval breaker (`_PyEval_SignalReceived`). On the
  main thread, `_PyErr_CheckSignalsTstate` (`signalmodule.c`) calls
  `d3g_check_pending_signals()`, which runs `d3g_flush_trace()`, restores
  `SIG_DFL`, unblocks the signal, and `raise()`s it, so the process dies
  with the same status (`128+n`, `WIFSIGNALED`) as an uninstrumented one. A
  second delivery while the first is pending dies immediately without
  flushing.
- **Teardown.** `d3g_flush_trace()` restores `SIG_DFL` on every substituted
  signal before `_PySignal_Fini` runs.

Out of scope: `SIGKILL`/`SIGSTOP`, fault signals, and dispositions set by C
code that bypasses `PyOS_setsig`.

## 10. Trace Data Model

All tables except `meta`, `machine`, and `functions` are keyed first by
`pid`, since a traced execution may span multiple processes. The schema is
created by `writer.c` (`SCHEMA_SQL`).

```
meta(pid)
machine(machine_id)
functions(function_id PK, ref)
calls(pid, call_id, function_id, caller_id, call_lineno, obj_id,
      control_flow, PK(pid, call_id))
attr_reads(pid, call_id, caller_id, write_call_lineno, read_call_lineno)
objects(pid, obj_idx, call_id, PK(pid, obj_idx))
members(pid, obj_idx, attr, child_idx)
ipc(pid, name, call_id)
io_objects(pid, io_object_id, name, offset, PK(pid, io_object_id))
io_ops(pid, io_object_id, call_id, offset, length, op_type)
```

- `functions.ref` — `"{absolute_filepath}:{qualname}"`; `function_id` is
  interned per process.
- `calls.control_flow` — a packed, LSB-first bitstring; one bit per branch
  decision (`if`/`while`-iteration/`for`-iteration) taken during the call.
- `calls.obj_id` — the tracked `self` of a method call, or 0.
- `io_ops.op_type` — 0 read, 1 write.
- `machine.machine_id` — provenance check; postprocessing refuses to run
  against a trace collected on a different machine, since `functions.ref`
  contains absolute paths re-parsed from disk during postprocessing.

Postprocessing adds three tables to the merged file:

```
dataflow_edges(pid, source_call_id, source_type, source_name,
               target_pid, target_call_id, target_type, target_name,
               member_path)
executed_lines(pid, call_id, line_order, lineno)
default_owner(pid, obj_idx, owner_idx, attr, PK(pid, obj_idx))
```

## 11. Multi-Process Execution Model

Every traced process writes its own `{pid}.db` (§8). The runner performs
no reconciliation. Across `os.fork()`, `d3g_after_fork_child_hook` clears
the parent's live records in the child and re-creates records for the
calls that are active at the fork with their original `call_id`s, so the
child's database is self-contained while still sharing the identity of the
enclosing calls with the parent (the test asserts this for `exchange()`).

Two merge operations then exist, both offline in `postprocess.py`:

- **Raw-row merge** (`merge_traces`). Unions every `{pid}.db` in the
  output directory into `trace.db` by SQL `ATTACH DATABASE`, keyed by the
  existing `pid` column. `functions` is re-keyed by `ref` and each input's
  `calls` rewritten accordingly. The inputs are left in place and
  `trace.db` is regenerated into a temporary file and atomically replaced
  on every run, so the step is repeatable and a process that exited after
  an earlier merge is included by running again.
- **IPC dataflow merge** (`resolve_ipc_edges`, §12 step 7). The only step
  that produces cross-process dependency edges.

## 12. Postprocessing Algorithm

Entry point: `python -m d3g.postprocess <output_dir | trace.db>`. Given a
directory, `merge_traces()` runs first and `postprocess()` then runs on
`trace.db`; given a file, only `postprocess()` runs.

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
   `DataflowEdge` is added between the calls that used that channel.
8. **Default ownership.** For each object with more than one referencing
   parent in `members`, the parent whose call constructed the object is
   preferred as the "default owner"; otherwise the first-observed parent
   is used. Ownership edges are then treated as a graph and cycles are
   broken by removing the most-recently-created edge in each cycle, so
   `default_owner` is acyclic.
9. **Write.** `dataflow_edges`, `executed_lines`, and `default_owner` are
   written back into `trace.db`.

## 13. Testing and Usage

### 13.1 End-to-end test

`tests/test.py` runs `tests/testapp/main.py` under
`cpython/python -m d3g` with `tests/testapp-config.yaml`, postprocesses the
output directory, and asserts structural properties of `trace.db` derived
from the application's source (no baseline database). `testapp` covers:

- `core.py` — calls, branches and loops, every container type as a `Payload` member, closures, module
  globals, a worker thread, a `Node` peer cycle, and a taint-excluded call.
- `ipc.py` — one `os.fork()` with pipe, FIFO, shared memory, semaphore,
  Unix socket, `mmap`, file I/O and `SIGUSR1` between parent and child; the
  child exits via `os._exit`.
- `ipc.py: terminate()` — the parent asserts `signal.getsignal(SIGTERM) is
  SIG_DFL` and kills itself with `SIGTERM`. The test requires exit status
  `-SIGTERM` and the presence of the `terminate`, `main`, and `<module>`
  call rows, which can only reach the database through the §9 flush.

Run: `cpython/python tests/test.py` (prints `PASS`).

### 13.2 Tracing an application

```
PYTHON_TRACER_CONFIG=configs/vllm-omni.yaml PYTHON_TRACER_OUTDIR=traces/vllm-omni \
  python -m d3g -- $(which vllm) serve tiny-random/Qwen-Image --omni --port 8000 --enforce-eager
python -m d3g.postprocess traces/vllm-omni
```

`configs/vllm-omni.yaml` traces the `vllm` and `vllm_omni` packages;
`configs/vllm-omni-traceall.yaml` sets `traceall: true`.
`scripts/poll-health-pid.sh` waits for the server's `/health` endpoint (or
its death) and `scripts/query-qwen-image.sh` issues one text-to-image
request.

## 14. CPython Integration Notes

| Version | Status | Reference |
|---|---|---|
| 3.12 | Complete, not maintained since the writer thread and signal work | Tag `d3g-3.12-port` |
| 3.14 | Current | Branch `d3g-3.14` |

All eval-loop hook sites live in `Python/bytecodes.c`; the generated files
regenerate byte-identically. The 3.14 port accounted for:

- `_PyInterpreterFrame` relocation (`pycore_frame.h` →
  `pycore_interpframe_structs.h`); `f_code` → `f_executable`; locals/stack
  slots as `_PyStackRef`.
- `RESUME` restructured into a `macro()` of composed `op()`s.
- `RETURN_CONST` removed (folded into `RETURN_VALUE`).
- The four `POP_JUMP_IF_*` opcodes collapsed onto `_POP_JUMP_IF_TRUE` /
  `_FALSE`.
- `FOR_ITER` and `CALL` rebuilt as macros, including the `FOR_ITER_GEN`
  inlining that `d3g_gen_iter_hook` compensates for, and a
  free-threaded (`Py_GIL_DISABLED`) code path.
- `mmapmodule.c` methods split into `_lock_held` helpers.

## 15. Open Issues

- Construction-time container attachment (`d3g_object_new_hook` for a
  bare `list()`/`dict()` that is never assigned to a tracked object) is not
  implemented; only containers reachable from a tracked object are tracked.
- `ObjectTraceData.call_id` (the constructing call) is always 0, so
  postprocessing's default-owner preference for the constructing call
  falls back to first-observed parent.
- `g_state.io_object_records` entries are never freed.
- Under `python -m d3g` the first flush happens at the end of
  `pymain_run_python`, before finalization, so `atexit` callbacks and
  module-teardown code run untraced, and a `SIG_DFL` signal arriving after
  that point takes the genuine default action without further flushing.
- Signals arriving while the main thread is blocked in a long C call are
  acted on only when the eval loop resumes, as with Python-level handlers.
- Substring provenance for `str`/`bytes` (which call produced a slice of a
  string) is not modelled; see §7.1.
- The 3.12 tag predates online serialization and signal interception.
