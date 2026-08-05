from __future__ import annotations

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
    Database,
    PathFilter,
    get_call_id,
    install,
    install_thread,
    uninstall,
)
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


def _load_config() -> dict:
    config_path = os.environ.get("PYTHON_TRACER_CONFIG")
    if not config_path:
        return {}
    import yaml
    with open(config_path) as f:
        return yaml.safe_load(f) or {}


def main() -> None:
    cfg = _load_config()
    output_dir = os.environ.get("PYTHON_TRACER_OUTDIR", "traces")

    sys.argv = sys.argv[1:]
    if sys.argv and sys.argv[0] == "--":
        sys.argv = sys.argv[1:]

    if not sys.argv:
        print("Usage: PYTHON_TRACER_CONFIG=config.yaml python -m tracer -- script.py [args...]", file=sys.stderr)
        sys.exit(1)

    prefixes = None
    tracked_classes = None
    taint_patterns = None
    modules = cfg.get("modules") or []
    tracked_classes = cfg.get("classes") or []
    taint_patterns = cfg.get("taint-functions") or None
    if modules:
        import importlib.util
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

    path_filter = PathFilter(prefixes=prefixes, tracked_classes=tracked_classes)

    db = Database()
    hook = TraceHook(db, path_filter)

    print("[__main__] Installing hooks")
    patch_message_queue(db)

    prefixes = list(path_filter._prefixes)
    install(hook, prefixes, db, path_filter, taint_patterns=taint_patterns)

    os.makedirs(output_dir, exist_ok=True)

    proc_hook = ProcessHook(
        prefixes=prefixes,
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
    script = sys.argv[0]
    if not os.path.exists(script):
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

        db.serialize(os.path.join(output_dir, f"{os.getpid()}.db"))

        all_dbs = sorted(glob.glob(os.path.join(output_dir, "*.db")))
        if len(all_dbs) > 1:
            merged_path = os.path.join(output_dir, "trace.db")
            fd, tmp_path = tempfile.mkstemp(suffix=".db", dir=output_dir)
            os.close(fd)
            first, rest = all_dbs[0], all_dbs[1:]
            shutil.copy2(first, tmp_path)
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
            for db_path in all_dbs:
                os.remove(db_path)
            os.rename(tmp_path, merged_path)


if __name__ == "__main__":
    main()
