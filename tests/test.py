"""End-to-end check of the d3g tracer and postprocessor.

Runs tests/testapp under the instrumented interpreter, postprocesses the
merged trace, and asserts structural properties of the result: the
expected calls, objects and memberships are present, and data dependencies
are recorded across call, thread and process boundaries. No baseline
database is used; every expectation is derived from the test application's
source so that the checks survive unrelated changes in ids and ordering.
"""

import os
import signal
import sqlite3
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPYTHON = os.path.join(ROOT, "cpython", "python")
TESTS = os.path.join(ROOT, "tests")
CONFIG = os.path.join(TESTS, "testapp-config.yaml")
SCRIPT = os.path.join(TESTS, "testapp", "main.py")
CORE = os.path.join(TESTS, "testapp", "core.py")

IO_OP_READ, IO_OP_WRITE = 0, 1


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

def run_trace(output_dir):
    env = os.environ.copy()
    env["PYTHONPATH"] = TESTS
    env["PYTHON_TRACER_CONFIG"] = CONFIG
    env["PYTHON_TRACER_OUTDIR"] = output_dir
    result = subprocess.run(
        [CPYTHON, "-m", "d3g", "--", SCRIPT],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    # The application ends itself with SIGTERM under SIG_DFL; the tracer must
    # preserve the default action (death by signal) after flushing.
    if result.returncode != -signal.SIGTERM:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"tracer exited with {result.returncode}, expected -SIGTERM")
    # The runner leaves one {pid}.db per process; postprocess merges the
    # directory into trace.db offline and then builds the dependency graph.
    result = subprocess.run(
        [CPYTHON, "-m", "d3g.postprocess", output_dir],
        cwd=ROOT, capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"postprocess exited with {result.returncode}")
    db_path = os.path.join(output_dir, "trace.db")
    if not os.path.exists(db_path):
        raise RuntimeError("no trace.db produced")
    return db_path


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def line_of(path, snippet, exact=False):
    """1-based line number of the unique source line containing snippet
    (or, with exact=True, equal to it, indentation included)."""
    with open(path) as f:
        hits = [i for i, text in enumerate(f, 1)
                if (text.rstrip("\n") == snippet if exact else snippet in text)]
    if len(hits) != 1:
        raise RuntimeError(f"{snippet!r} matched {len(hits)} lines in {path}")
    return hits[0]


class Trace:
    def __init__(self, db_path):
        self.c = sqlite3.connect(db_path)
        self.pids = sorted(r[0] for r in self.c.execute("SELECT pid FROM meta"))
        self.funcs = {fid: ref for fid, ref in self.c.execute(
            "SELECT function_id, ref FROM functions")}
        # (pid, call_id) -> (ref, caller_id, call_lineno, obj_id)
        self.calls = {
            (pid, cid): (self.funcs[fid], caller, lineno, obj)
            for pid, cid, fid, caller, lineno, obj in self.c.execute(
                "SELECT pid, call_id, function_id, caller_id, call_lineno, obj_id "
                "FROM calls")
        }

    def q(self, sql, *args):
        return self.c.execute(sql, args).fetchall()

    def calls_of(self, suffix, pid=None):
        """[(pid, call_id, caller_id, obj_id)] for calls whose ref ends with suffix."""
        out = []
        for (p, cid), (ref, caller, _lineno, obj) in self.calls.items():
            if ref.endswith(suffix) and (pid is None or p == pid):
                out.append((p, cid, caller, obj))
        return sorted(out)

    def one_call(self, suffix, pid):
        calls = self.calls_of(suffix, pid)
        if len(calls) != 1:
            raise AssertionError(f"expected exactly one {suffix} in pid {pid}, got {calls}")
        return calls[0]

    def attr_reads(self, pid, call_id):
        """{(write_call_lineno, read_call_lineno)} recorded for a call."""
        return {(w, r) for w, r in self.q(
            "SELECT write_call_lineno, read_call_lineno FROM attr_reads "
            "WHERE pid = ? AND call_id = ?", pid, call_id)}

    def members(self, pid, obj_idx):
        return {attr: child for attr, child in self.q(
            "SELECT attr, child_idx FROM members WHERE pid = ? AND obj_idx = ?",
            pid, obj_idx)}

    def executed(self, pid, call_id):
        return [r[0] for r in self.q(
            "SELECT lineno FROM executed_lines WHERE pid = ? AND call_id = ? "
            "ORDER BY line_order", pid, call_id)]


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_trace(db_path):
    t = Trace(db_path)
    failures = []

    def expect(cond, msg):
        if not cond:
            failures.append(msg)

    L = lambda s: line_of(CORE, s)  # noqa: E731

    # --- processes ---------------------------------------------------------
    expect(len(t.pids) == 2, f"expected parent and one forked child, got pids {t.pids}")
    parent, child = t.pids[0], t.pids[-1]

    # --- calls and call graph ---------------------------------------------
    expect(not any("scratch_noise" in ref for ref in t.funcs.values()),
           "taint-excluded function scratch_noise appears in functions")
    expect(len(set(t.funcs.values())) == len(t.funcs),
           "function refs are not unique after merge")

    mod = t.one_call("main.py:<module>", parent)
    main = t.one_call("main.py:main", parent)
    run = t.one_call("core.py:run", parent)
    exchange = t.one_call("ipc.py:exchange", parent)
    expect(main[2] == mod[1], "main() is not called from main.py:<module>")
    expect(run[2] == main[1], "run() is not called from main()")
    expect(exchange[2] == main[1], "exchange() is not called from main()")

    records = t.calls_of("core.py:Payload.record", parent)
    expect(len(records) == 4,
           f"expected 4 Payload.record calls (3 from run, 1 from the thread), got {len(records)}")
    from_run = [r for r in records if r[2] == run[1]]
    from_thread = [r for r in records if r[2] == 0]
    expect(len(from_run) == 3, f"expected 3 record calls from run(), got {len(from_run)}")
    expect(len(from_thread) == 1, f"expected 1 record call from the worker thread, got {len(from_thread)}")

    adds = t.calls_of("make_accumulator.<locals>.add", parent)
    expect(len(adds) == 4, f"expected 4 add() calls, got {len(adds)}")
    expect(len(t.calls_of("core.py:Node.__init__", parent)) == 2, "expected 2 Node.__init__ calls")
    expect(len(t.calls_of("ipc.py:_on_usr1", parent)) == 1, "signal handler was not traced")
    # terminate(), main() and <module> are still active when SIGTERM arrives;
    # they reach the database only through the SIG_DFL flush.
    terminate = t.one_call("ipc.py:terminate", parent)
    expect(terminate[2] == main[1], "terminate() is not called from main()")

    # The child inherits the calls active at fork and no others.
    child_refs = sorted(ref.rsplit("/", 1)[-1] for (p, _), (ref, *_rest) in t.calls.items() if p == child)
    expect(child_refs == ["ipc.py:exchange", "main.py:<module>", "main.py:main"],
           f"unexpected child call set {child_refs}")
    expect(t.one_call("ipc.py:exchange", child)[1] == exchange[1],
           "child's exchange call does not keep the parent's call_id")

    # --- objects and membership -------------------------------------------
    payload = t.one_call("core.py:Payload.__init__", parent)[3]
    nodes = sorted(obj for _, _, _, obj in t.calls_of("core.py:Node.__init__", parent))
    expect(payload > 0 and len(nodes) == 2 and nodes[0] != nodes[1],
           f"expected one Payload and two distinct Node objects, got {payload}, {nodes}")

    pm = t.members(parent, payload)
    expect(set(pm) == {"history", "meta", "seen", "queue", "shape"},
           f"Payload members {sorted(pm)} do not cover every container type")
    expect(len(set(pm.values())) == 5, "Payload's container members are not distinct objects")

    for n, other in ((nodes[0], nodes[1]), (nodes[1], nodes[0])):
        nm = t.members(parent, n)
        expect(nm.get("payload") == payload, f"Node {n} does not reference the shared Payload")
        expect(nm.get("peer") == other, f"Node {n}.peer does not reference the other Node")
        expect("nested" in nm, f"Node {n} lacks its nested container")

    link = t.one_call("core.py:Node.link", parent)
    expect(link[3] in nodes, "Node.link was not attributed to a Node instance")

    owners = {obj: (owner, attr) for obj, owner, attr in t.q(
        "SELECT obj_idx, owner_idx, attr FROM default_owner WHERE pid = ?", parent)}
    expect(owners.get(payload, (None,))[0] in nodes,
           "shared Payload was not assigned a single Node as default owner")
    owned_nodes = [n for n in nodes if n in owners]
    expect(len(owned_nodes) == 1, "peer cycle between the two Nodes was not broken to a single owner edge")
    for n in nodes:
        seen, cur = set(), n
        while cur in owners and cur not in seen:
            seen.add(cur)
            cur = owners[cur][0]
        expect(cur not in seen, f"default_owner contains a cycle through object {n}")

    # --- control flow (postprocessed replay) -------------------------------
    branch = {L('= "neg"'): -1, L('= "zero"'): 0, L('= "pos"'): None}
    taken = sorted(
        next(l for l in t.executed(parent, cid) if l in branch) for _, cid, _, _ in records)
    expect(taken == sorted([L('= "neg"'), L('= "zero"'), L('= "pos"'), L('= "pos"')]),
           f"record() branch replay is wrong: executed branch lines {taken}")

    drain = t.one_call("core.py:Payload.drain", parent)
    body = L("total += self.queue.popleft()")
    expect(t.executed(parent, drain[1]).count(body) == 5,
           "drain's while loop did not replay 5 iterations (4 traced records + 1 from the excluded call)")

    # Bytecode-level replay: the compound condition, ternary and
    # comprehension emit branch bytes of their own, and `async for` is
    # driven by the GET_ANEXT/END_ASYNC_FOR bytes.
    consume = t.one_call("core.py:consume", parent)
    consume_lines = t.executed(parent, consume[1])
    expect(consume_lines.count(L("acc += i")) == 2 and consume_lines.count(L("acc -= 1")) == 2,
           f"consume(): async for body did not replay 4 iterations with 2 per branch: {consume_lines}")
    expect(L('return {"kind": kind') in consume_lines, "consume(): return statement not replayed")

    safe_div = t.one_call("core.py:safe_div", parent)
    expect(L("return a / b") in t.executed(parent, safe_div[1]), "safe_div try body not replayed")

    run_lines = t.executed(parent, run[1])
    expect(run_lines.count(L("shared.record(value)")) == 3, "run()'s first loop did not replay 3 iterations")
    expect(run_lines.count(L("acc(i)")) == 3, "run()'s second loop did not replay 3 iterations")

    # --- data dependencies across call boundaries ---------------------------
    # Attribute written in __init__, read in a method (call -> call).
    for _, cid, _, _ in records:
        ar = t.attr_reads(parent, cid)
        expect((L("self.history = []"), L("self.history.append(value)")) in ar,
               f"record call {cid}: history read is not linked to its write in __init__")
        expect((L("self.meta = {}"), L('return self.meta["sign"]')) in ar,
               f"record call {cid}: meta read is not linked to its write in __init__")
    # The same dependency from the worker thread (thread -> main-thread writer).
    thread_call = from_thread[0][1] if from_thread else None
    expect(thread_call is not None and
           (L("self.queue = deque()"), L("self.queue.append(value)")) in t.attr_reads(parent, thread_call),
           "worker-thread record call lacks the cross-thread queue dependency")

    # Item written under a key in one call, read under that key in another.
    run_ar = t.attr_reads(parent, run[1])
    expect((L('= "pos"'), L('"sign": shared.meta["sign"]')) in run_ar,
           'run(): meta["sign"] read is not linked to the write in the last record() call')

    # Container mutated through a C method in one call, consumed in another.
    expect((L("self.queue.append(value)"), L("total += self.queue.popleft()")) in t.attr_reads(parent, drain[1]),
           "drain(): popleft is not linked to record()'s append")

    # Attribute written in Node.link, read in run().
    expect({(L("self.peer = other"), L("left.peer.peer is left")),
            (L("other.peer = self"), L("left.peer.peer is left"))} <= run_ar,
           "run(): peer reads are not linked to the writes in Node.link")

    # `shared = Payload(...)` binds shared to the constructor call's result;
    # `shared.record(value)` then receives it as `self`.
    init_call = t.one_call("core.py:Payload.__init__", parent)[1]
    self_edges = {(src, tgt) for src, tgt in t.q(
        "SELECT source_call_id, target_call_id FROM dataflow_edges WHERE pid = ? "
        "AND source_type = 'call_result' AND target_type = 'arg' AND target_name = 'self'", parent)}
    expect(all((init_call, cid) in self_edges for _, cid, _, _ in from_run),
           "run(): record() calls do not receive shared as the result of Payload()")

    # Module global written at import, read in run().
    expect((L("counter = 0"), L("counter += 1")) in run_ar,
           "run(): first read of global counter is not linked to its module-level definition")

    # Closure cell written in the enclosing call, read in the inner call.
    first_add = min(cid for _, cid, _, _ in adds) if adds else None
    expect(first_add is not None and
           (line_of(CORE, "    total = 0", exact=True), L("total += n")) in t.attr_reads(parent, first_add),
           "first add(): closure read of total is not linked to make_accumulator's definition")

    # --- data dependencies across process boundaries -------------------------
    edges = {(src_pid, dst_pid, name.split(":")[0]) for src_pid, dst_pid, name in t.q(
        "SELECT pid, target_pid, source_name FROM dataflow_edges WHERE source_type = 'ipc'")}
    for channel in ("sem", "shm", "sock"):
        expect((parent, child, channel) in edges and (child, parent, channel) in edges,
               f"no bidirectional cross-process edge for {channel}")

    # Shared-memory traffic goes through SharedMemory.buf (a memoryview over
    # the mmap), so the ops come from the memoryview hooks; the write in the
    # child and the overlapping read in the parent are paired into an io edge.
    expect(any(src == child and dst == parent and name == "io" for src, dst, name in edges),
           "no child->parent io edge for the shared-memory write/read pair")

    for suffix in ("/map", "/data", "/fifo", "/psm_%"):
        ops = {}
        for pid, op in t.q(
                "SELECT o.pid, p.op_type FROM io_ops p JOIN io_objects o "
                "ON o.pid = p.pid AND o.io_object_id = p.io_object_id WHERE o.name LIKE ?",
                "%" + suffix):
            ops.setdefault(pid, set()).add(op)
        expect(IO_OP_WRITE in ops.get(child, ()) and IO_OP_READ in ops.get(parent, ()),
               f"{suffix}: expected a child write and a parent read, got {ops}")

    return failures


def test_trace():
    with tempfile.TemporaryDirectory() as d:
        db_path = run_trace(d)
        failures = check_trace(db_path)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    test_trace()
