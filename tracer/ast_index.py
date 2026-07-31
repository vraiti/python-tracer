from __future__ import annotations

import ast
import os
from typing import Any

from tracer._tracer import PathFilter


class AstIndex:
    def __init__(self) -> None:
        self._func_to_id: dict[str, int] = {}
        self._next_func_id = 0
        self._control_flow_lines: dict[str, set[int]] = {}
        self._file_asts: dict[str, ast.Module] = {}

    def preprocess(self, path_filter: PathFilter) -> None:
        for prefix in path_filter._prefixes:
            self._walk_directory(prefix)

    def _walk_directory(self, root: str) -> None:
        for dirpath, _, filenames in os.walk(root):
            for fname in filenames:
                if not fname.endswith(".py"):
                    continue
                filepath = os.path.join(dirpath, fname)
                self._process_file(filepath)

    def _process_file(self, filepath: str) -> None:
        try:
            with open(filepath) as f:
                source = f.read()
            tree = ast.parse(source, filepath)
        except (SyntaxError, OSError):
            return
        self._file_asts[filepath] = tree
        self._extract_functions(tree, filepath, "")

    def _extract_functions(
        self, node: ast.AST, filepath: str, prefix: str,
    ) -> None:
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                qualname = f"{prefix}{child.name}" if not prefix else f"{prefix}.{child.name}"
                ref = f"{filepath}:{qualname}"
                self.get_function_id(ref)
                cf_lines = _collect_control_flow_lines(child)
                if cf_lines:
                    self._control_flow_lines[ref] = cf_lines
                self._extract_functions(child, filepath, qualname)
            elif isinstance(child, ast.ClassDef):
                classname = f"{prefix}{child.name}" if not prefix else f"{prefix}.{child.name}"
                self._extract_functions(child, filepath, classname)

    def get_function_id(self, ref: str) -> int:
        fid = self._func_to_id.get(ref)
        if fid is not None:
            return fid
        fid = self._next_func_id
        self._func_to_id[ref] = fid
        self._next_func_id += 1
        return fid

    def get_control_flow_lines(self, ref: str) -> set[int] | None:
        return self._control_flow_lines.get(ref)

    def ref_from_code(self, code: Any) -> str:
        return f"{code.co_filename}:{code.co_qualname}"


_CF_TYPES = (ast.If, ast.While, ast.For, ast.AsyncFor)


def _collect_control_flow_lines(func_node: ast.AST) -> set[int]:
    lines: set[int] = set()
    for node in ast.walk(func_node):
        if isinstance(node, _CF_TYPES):
            lines.add(node.lineno)
    return lines
