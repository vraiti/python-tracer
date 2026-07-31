from __future__ import annotations

import atexit
import multiprocessing
import multiprocessing.process
import os
import sys
from typing import Any


_original_process_init = multiprocessing.process.BaseProcess.__init__


class ProcessHook:
    def __init__(
        self,
        prefixes: list[str],
        tracked_classes: list[str] | None = None,
        taint_patterns: list[str] | None = None,
    ) -> None:
        self.prefixes = prefixes
        self.tracked_classes = tracked_classes
        self.taint_patterns = taint_patterns
        self._children: list[multiprocessing.process.BaseProcess] = []

    def install(self) -> None:
        hook = self

        def _patched_init(
            proc_self: multiprocessing.process.BaseProcess,
            group: Any = None,
            target: Any = None,
            name: Any = None,
            args: tuple = (),
            kwargs: dict | None = None,
            *,
            daemon: Any = None,
        ) -> None:
            if target is not None:
                wrapped = _TracedTarget(
                    target,
                    hook.prefixes,
                    tracked_classes=hook.tracked_classes,
                    taint_patterns=hook.taint_patterns,
                )
                _original_process_init(
                    proc_self,
                    group=group,
                    target=wrapped,
                    name=name,
                    args=args,
                    kwargs=kwargs if kwargs is not None else {},
                    daemon=daemon,
                )
            else:
                _original_process_init(
                    proc_self,
                    group=group,
                    target=target,
                    name=name,
                    args=args,
                    kwargs=kwargs if kwargs is not None else {},
                    daemon=daemon,
                )
            hook._children.append(proc_self)

        multiprocessing.process.BaseProcess.__init__ = _patched_init

    def uninstall(self) -> None:
        multiprocessing.process.BaseProcess.__init__ = _original_process_init

    def join_children(self, timeout: float = 30) -> None:
        for child in self._children:
            child.join(timeout=timeout)

    def child_trace_paths(self) -> list[str]:
        paths = []
        for child in self._children:
            if child.pid is not None:
                p = f"/tmp/{child.pid}.db"
                if os.path.exists(p):
                    paths.append(p)
        return paths


class _TracedTarget:
    def __init__(
        self,
        original_target: Any,
        prefixes: list[str],
        tracked_classes: list[str] | None = None,
        taint_patterns: list[str] | None = None,
    ) -> None:
        self.original_target = original_target
        self.prefixes = prefixes
        self.tracked_classes = tracked_classes
        self.taint_patterns = taint_patterns

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        import threading

        from tracer._tracer import (
            Database,
            OwnershipHook,
            PathFilter,
            install,
            install_thread,
            load_ast_data,
            uninstall,
        )
        from tracer.ast_index import AstIndex
        from tracer.ipc import patch_message_queue

        pid = os.getpid()
        output_file = f"/tmp/{pid}.db"

        path_filter = PathFilter(prefixes=self.prefixes, tracked_classes=self.tracked_classes)
        ast_index = AstIndex()
        ast_index.preprocess(path_filter)

        db = Database()

        from tracer.__main__ import TraceHook
        hook = TraceHook(db, path_filter)
        ownership = OwnershipHook(db, hook)

        load_ast_data(ast_index._func_to_id, ast_index._control_flow_lines)

        patch_message_queue(db)

        prefixes = list(path_filter._prefixes)
        install(hook, prefixes, db, ownership, path_filter, taint_patterns=self.taint_patterns)

        _original_run = threading.Thread.run
        def _patched_run(self_thread: threading.Thread) -> None:
            install_thread()
            _original_run(self_thread)
        threading.Thread.run = _patched_run  # type: ignore

        written = False

        def _write_trace() -> None:
            nonlocal written
            if written:
                return
            written = True
            uninstall()
            try:
                from tracer.__main__ import serialize
                serialize(db, output_file)
            except Exception:
                import traceback
                traceback.print_exc()

        atexit.register(_write_trace)

        try:
            return self.original_target(*args, **kwargs)
        finally:
            _write_trace()
