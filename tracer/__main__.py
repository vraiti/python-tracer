from __future__ import annotations

import argparse
import glob
import os
import shutil
import signal
import sqlite3
import sys
import tempfile
import threading
from typing import Any

from tracer._tracer import (
    CallRecord,
    Database,
    ObjectRecord,
    OwnershipHook,
    PathFilter,
    get_call_id,
    get_func_map,
    install,
    install_thread,
    load_ast_data,
    uninstall,
)
from tracer.ast_index import AstIndex
from tracer.ipc import patch_message_queue
from tracer.process_hook import ProcessHook


class TraceHook:
    def __init__(
        self,
        db: Database,
        path_filter: PathFilter,
    ) -> None:
        self.db = db
        self.path_filter = path_filter


def serialize(db: Database, output: str) -> None:
    if os.path.exists(output):
        os.remove(output)
    conn = sqlite3.connect(output)
    c = conn.cursor()

    c.executescript("""
        CREATE TABLE meta (pid INTEGER);
        CREATE TABLE machine (machine_id TEXT NOT NULL);
        CREATE TABLE functions (function_id INTEGER PRIMARY KEY, ref TEXT NOT NULL);
        CREATE TABLE calls (
            pid INTEGER NOT NULL,
            call_id INTEGER NOT NULL,
            function_id INTEGER NOT NULL,
            caller_id INTEGER NOT NULL,
            call_lineno INTEGER NOT NULL,
            obj_id INTEGER NOT NULL,
            control_flow BLOB,
            PRIMARY KEY (pid, call_id)
        );
        CREATE TABLE attr_reads (
            pid INTEGER NOT NULL,
            call_id INTEGER NOT NULL,
            caller_id INTEGER NOT NULL,
            write_call_lineno INTEGER NOT NULL,
            read_call_lineno INTEGER NOT NULL
        );
        CREATE TABLE objects (
            pid INTEGER NOT NULL,
            obj_idx INTEGER NOT NULL,
            call_id INTEGER NOT NULL,
            PRIMARY KEY (pid, obj_idx)
        );
        CREATE TABLE members (
            pid INTEGER NOT NULL,
            obj_idx INTEGER NOT NULL,
            attr TEXT NOT NULL,
            child_idx INTEGER NOT NULL
        );
        CREATE TABLE ipc (
            pid INTEGER NOT NULL,
            name TEXT NOT NULL,
            obj_idx INTEGER NOT NULL
        );
    """)

    c.execute("INSERT INTO meta VALUES (?)", (os.getpid(),))
    machine_id = ""
    try:
        with open("/etc/machine-id") as f:
            machine_id = f.read().strip()
    except OSError:
        pass
    c.execute("INSERT INTO machine VALUES (?)", (machine_id,))

    func_map = {v: k for k, v in get_func_map().items()}
    c.executemany(
        "INSERT INTO functions VALUES (?, ?)",
        func_map.items(),
    )

    _TAINT_ID = (1 << 64) - 1
    pid = os.getpid()

    n_calls = 0
    for rec in db.calls:
        cf = bytes(rec.control_flow) if rec.control_flow else None
        caller_id = 0 if rec.caller_id == _TAINT_ID else rec.caller_id
        c.execute(
            "INSERT INTO calls VALUES (?, ?, ?, ?, ?, ?, ?)",
            (pid, rec.call_id, rec.function_id, caller_id, rec.call_lineno, rec.obj_id, cf),
        )
        for ar in rec.attr_reads:
            ar_caller = 0 if ar.caller_id == _TAINT_ID else ar.caller_id
            c.execute(
                "INSERT INTO attr_reads VALUES (?, ?, ?, ?, ?)",
                (pid, rec.call_id, ar_caller, ar.write_call_lineno, ar.read_call_lineno),
            )
        n_calls += 1

    n_objects = 0
    for idx, obj in enumerate(db.objects):
        c.execute("INSERT INTO objects VALUES (?, ?, ?)", (pid, idx, obj.call_id))
        for attr, child_idx in dict(obj.members).items():
            c.execute("INSERT INTO members VALUES (?, ?, ?, ?)", (pid, idx, attr, child_idx))
        n_objects += 1

    n_ipc = 0
    for irec in db.ipc:
        c.execute("INSERT INTO ipc VALUES (?, ?, ?)", (pid, irec.name, irec.obj_idx))
        n_ipc += 1

    conn.commit()
    conn.close()

    print(f"Trace written to {output} ({n_calls} calls, {n_objects} objects, {n_ipc} ipc)", file=sys.stderr)


def _wait_for_traces(output_dir: str) -> None:
    import time
    while True:
        pids = []
        for name in os.listdir(output_dir):
            if name.endswith(".db"):
                try:
                    pids.append(int(name[:-3]))
                except ValueError:
                    continue
        alive = [pid for pid in pids if os.path.exists(f"/proc/{pid}")]
        if not alive:
            return
        print(f"Waiting for {len(alive)} traced process(es) to exit: {alive}", file=sys.stderr)
        time.sleep(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="python-tracer")
    parser.add_argument("--config", type=str, default=None, help="path to YAML config with 'modules' and 'classes' keys")
    parser.add_argument("--output", type=str, default="trace.db", help="output file")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run")

    args = parser.parse_args()

    if not args.command:
        parser.error("no command specified")

    prefixes = None
    tracked_classes = None
    taint_patterns = None
    if args.config:
        import importlib.util
        import yaml
        with open(args.config) as f:
            cfg = yaml.safe_load(f)
        modules = cfg.get("modules") or []
        tracked_classes = cfg.get("classes") or []
        taint_patterns = cfg.get("taint-functions") or None
        prefixes = []
        for mod_name in modules:
            spec = importlib.util.find_spec(mod_name)
            if spec is None:
                print(f"Warning: could not find module '{mod_name}'", file=sys.stderr)
                continue
            if spec.submodule_search_locations:
                prefixes.append(spec.submodule_search_locations[0])
            elif spec.origin:
                prefixes.append(os.path.dirname(os.path.abspath(spec.origin)))
            else:
                print(f"Warning: module '{mod_name}' has no path", file=sys.stderr)

    print("[__main__] Parsing AST")
    path_filter = PathFilter(prefixes=prefixes, tracked_classes=tracked_classes)
    ast_index = AstIndex()
    ast_index.preprocess(path_filter)

    db = Database()
    hook = TraceHook(db, path_filter)
    ownership = OwnershipHook(db, hook)

    load_ast_data(ast_index._func_to_id, ast_index._control_flow_lines)
    
    print("[__main__] Installing hooks")
    patch_message_queue(db)

    # Install trace function
    prefixes = list(path_filter._prefixes)
    install(hook, prefixes, db, ownership, path_filter, taint_patterns=taint_patterns)

    output_dir = args.output + ".d"
    os.makedirs(output_dir, exist_ok=True)

    proc_hook = ProcessHook(
        prefixes=prefixes,
        output_dir=output_dir,
        tracked_classes=tracked_classes,
        taint_patterns=taint_patterns,
    )
    proc_hook.install()

    # Monkey-patch Thread.run for new threads
    _original_run = threading.Thread.run
    def _patched_run(self: threading.Thread) -> None:
        install_thread()
        _original_run(self)
    threading.Thread.run = _patched_run  # type: ignore

    print("[__main__] launching command")
    cmd = args.command
    if cmd[0] == "--":
        cmd = cmd[1:]

    sys.argv = cmd
    script = cmd[0]
    if not os.path.exists(script):
        import shutil
        resolved = shutil.which(script)
        if resolved:
            script = resolved

    code: Any = None
    with open(script) as f:
        code = compile(f.read(), script, "exec")

    _uninstalled = False

    _orig_signal = signal.signal
    def _wrap_signal(signum: int, handler: Any) -> Any:
        if signum == signal.SIGTERM and callable(handler):
            real_handler = handler
            def _wrapper(s: int, f: Any) -> Any:
                nonlocal _uninstalled
                if not _uninstalled:
                    uninstall()
                    _uninstalled = True
                return real_handler(s, f)
            return _orig_signal(signum, _wrapper)
        return _orig_signal(signum, handler)
    signal.signal = _wrap_signal  # type: ignore

    def _default_sigterm(signum: int, frame: Any) -> None:
        nonlocal _uninstalled
        if not _uninstalled:
            uninstall()
            _uninstalled = True
        raise SystemExit(0)
    _orig_signal(signal.SIGTERM, _default_sigterm)

    try:
        exec(code, {"__name__": "__main__", "__file__": script})
    except SystemExit:
        pass
    finally:
        if not _uninstalled:
            uninstall()
            _uninstalled = True
        signal.signal = _orig_signal  # type: ignore
        proc_hook.uninstall()

        _wait_for_traces(output_dir)

        all_dbs = sorted(glob.glob(os.path.join(output_dir, "*.db")))
        if all_dbs:
            fd, tmp_path = tempfile.mkstemp(suffix=".db", dir=os.path.dirname(os.path.abspath(args.output)))
            os.close(fd)
            first, rest = all_dbs[0], all_dbs[1:]
            shutil.copy2(first, tmp_path)
            if rest:
                conn = sqlite3.connect(tmp_path)
                for db_path in rest:
                    print(f"Merging {db_path}", file=sys.stderr)
                    try:
                        conn.execute("ATTACH DATABASE ? AS child", (db_path,))
                        conn.execute("INSERT OR IGNORE INTO meta SELECT * FROM child.meta")
                        conn.execute("INSERT OR IGNORE INTO functions SELECT * FROM child.functions")
                        conn.execute("INSERT INTO calls SELECT * FROM child.calls")
                        conn.execute("INSERT INTO attr_reads SELECT * FROM child.attr_reads")
                        conn.execute("INSERT INTO objects SELECT * FROM child.objects")
                        conn.execute("INSERT INTO members SELECT * FROM child.members")
                        conn.execute("INSERT INTO ipc SELECT * FROM child.ipc")
                        conn.execute("DETACH DATABASE child")
                    except sqlite3.Error as e:
                        print(f"Failed to merge {db_path}: {e}", file=sys.stderr)
                        try:
                            conn.rollback()
                            conn.execute("DETACH DATABASE child")
                        except Exception:
                            pass
                conn.commit()
                conn.close()
            shutil.rmtree(output_dir)
            os.rename(tmp_path, args.output)


if __name__ == "__main__":
    main()
