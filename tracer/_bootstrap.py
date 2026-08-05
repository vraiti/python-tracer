from __future__ import annotations

import os
import sys
import threading


def init() -> None:
    config_path = os.environ.get("PYTHON_TRACER_CONFIG")
    output_dir = os.environ.get("PYTHON_TRACER_OUTDIR")
    if not config_path or not output_dir:
        missing = []
        if not config_path:
            missing.append("PYTHON_TRACER_CONFIG")
        if not output_dir:
            missing.append("PYTHON_TRACER_OUTDIR")
        print(f"tracer: {', '.join(missing)} not set", file=sys.stderr)
        sys.exit(1)

    from tracer._tracer import Database, PathFilter, install, install_thread, uninstall

    import yaml
    with open(config_path) as f:
        cfg = yaml.safe_load(f) or {}

    os.makedirs(output_dir, exist_ok=True)

    modules = cfg.get("modules") or []
    tracked_classes = cfg.get("classes") or []
    taint_patterns = cfg.get("taint-functions") or None

    prefixes = None
    if modules:
        import importlib.util
        prefixes = []
        for mod_name in modules:
            spec = importlib.util.find_spec(mod_name)
            if spec is None:
                print(f"tracer: could not find module '{mod_name}'", file=sys.stderr)
                continue
            if spec.submodule_search_locations:
                prefixes.append(spec.submodule_search_locations[0])
            elif spec.origin:
                prefixes.append(os.path.dirname(os.path.abspath(spec.origin)))

    path_filter = PathFilter(prefixes=prefixes, tracked_classes=tracked_classes)
    db = Database()

    from tracer.__main__ import TraceHook
    hook = TraceHook(db, path_filter)

    prefix_list = list(path_filter._prefixes)
    install(hook, prefix_list, db, path_filter, taint_patterns=taint_patterns)

    from tracer.process_hook import ProcessHook
    proc_hook = ProcessHook(
        prefixes=prefix_list,
        tracked_classes=tracked_classes,
        taint_patterns=taint_patterns,
    )
    proc_hook.install()

    _original_run = threading.Thread.run
    def _patched_run(self: threading.Thread) -> None:
        install_thread()
        _original_run(self)
    threading.Thread.run = _patched_run  # type: ignore

