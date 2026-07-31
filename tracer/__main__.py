from __future__ import annotations

import argparse
import os
import sqlite3
import sys
import threading
from typing import Any

from tracer._tracer import (
    Database,
    OwnershipHook,
    PathFilter,
    install,
    install_thread,
    load_ast_data,
    uninstall,
)
from tracer.ast_processor import AstProcessor
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
        for cls_name in tracked_classes:
            if len(cls_name) >= 512:
                parser.error(f"tracked class name too long (>= 512 chars): {cls_name[:64]}...")
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
    ast_processor = AstProcessor()
    ast_processor.preprocess(path_filter)

    db = Database()
    hook = TraceHook(db, path_filter)
    ownership = OwnershipHook(db, hook)

    load_ast_data(ast_processor._func_to_id, ast_processor._control_flow_lines)
    
    print("[__main__] Installing hooks")
    patch_message_queue(db)

    # Install trace function
    prefixes = list(path_filter._prefixes)
    install(hook, prefixes, db, ownership, path_filter, taint_patterns=taint_patterns)

    # Monkey-patch multiprocessing to trace child processes
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

    try:
        exec(code, {"__name__": "__main__", "__file__": script})
    except SystemExit:
        pass
    finally:
        uninstall()
        proc_hook.uninstall()
        proc_hook.join_children()
        db.serialize(args.output)

        child_dbs = proc_hook.child_trace_paths()
        if child_dbs:
            import time as _time
            conn = sqlite3.connect(args.output)
            for child_db in child_dbs:
                print(f"Merging child trace {child_db}", file=sys.stderr)
                for attempt in range(10):
                    try:
                        conn.execute("ATTACH DATABASE ? AS child", (child_db,))
                        conn.execute("INSERT OR IGNORE INTO meta SELECT * FROM child.meta")
                        conn.execute("INSERT OR IGNORE INTO functions SELECT * FROM child.functions")
                        conn.execute("INSERT INTO calls SELECT * FROM child.calls")
                        conn.execute("INSERT INTO attr_reads SELECT * FROM child.attr_reads")
                        conn.execute("INSERT INTO objects SELECT * FROM child.objects")
                        conn.execute("INSERT INTO members SELECT * FROM child.members")
                        conn.execute("INSERT INTO ipc SELECT * FROM child.ipc")
                        conn.execute("DETACH DATABASE child")
                        break
                    except sqlite3.OperationalError:
                        try:
                            conn.execute("DETACH DATABASE child")
                        except Exception:
                            pass
                        if attempt < 9:
                            _time.sleep(1)
                        else:
                            print(f"Failed to merge {child_db} after 10 attempts", file=sys.stderr)
            conn.commit()
            conn.close()


if __name__ == "__main__":
    main()
