from __future__ import annotations

import ast
import hashlib
import os
import pickle
import sqlite3
from typing import Any

from tracer._tracer import PathFilter

_CACHE_SCHEMA = """
CREATE TABLE IF NOT EXISTS ast_cache (
    ref TEXT PRIMARY KEY,
    file_hash TEXT NOT NULL,
    func_ast BLOB NOT NULL,
    cf_lines BLOB
)
"""


class AstProcessor:
    def __init__(self, cache_path: str = "traces/ast-cache.db") -> None:
        self._func_to_id: dict[str, int] = {}
        self._next_func_id = 0
        self._control_flow_lines: dict[str, set[int]] = {}
        self._file_asts: dict[str, ast.Module] = {}
        os.makedirs(os.path.dirname(cache_path), exist_ok=True)
        self._cache_conn = sqlite3.connect(cache_path)
        self._cache_conn.execute(_CACHE_SCHEMA)
        self._cache_conn.commit()

    def preprocess(self, path_filter: PathFilter) -> None:
        for prefix in path_filter._prefixes:
            self._walk_directory(prefix)
        self._cache_conn.commit()

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
        except OSError:
            return

        file_hash = hashlib.sha256(source.encode()).hexdigest()

        rows = self._cache_conn.execute(
            "SELECT ref, file_hash, func_ast, cf_lines FROM ast_cache WHERE ref LIKE ?",
            (filepath + ":%",),
        ).fetchall()

        if rows and all(r[1] == file_hash for r in rows):
            for ref, _, func_ast_blob, cf_lines_blob in rows:
                self.get_function_id(ref)
                if cf_lines_blob is not None:
                    self._control_flow_lines[ref] = pickle.loads(cf_lines_blob)
            self._file_asts[filepath] = pickle.loads(rows[0][2])
            return

        if rows:
            self._cache_conn.execute(
                "DELETE FROM ast_cache WHERE ref LIKE ?",
                (filepath + ":%",),
            )

        try:
            tree = ast.parse(source, filepath)
        except SyntaxError:
            return

        self._file_asts[filepath] = tree
        self._extract_functions(tree, filepath, "", file_hash)

    def _extract_functions(
        self, node: ast.AST, filepath: str, prefix: str, file_hash: str,
    ) -> None:
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                qualname = f"{prefix}{child.name}" if not prefix else f"{prefix}.{child.name}"
                ref = f"{filepath}:{qualname}"
                self.get_function_id(ref)
                cf_lines = _collect_control_flow_lines(child)
                if cf_lines:
                    self._control_flow_lines[ref] = cf_lines
                self._cache_conn.execute(
                    "INSERT OR REPLACE INTO ast_cache (ref, file_hash, func_ast, cf_lines) VALUES (?, ?, ?, ?)",
                    (ref, file_hash, pickle.dumps(child), pickle.dumps(cf_lines) if cf_lines else None),
                )
                self._extract_functions(child, filepath, qualname, file_hash)
            elif isinstance(child, ast.ClassDef):
                classname = f"{prefix}{child.name}" if not prefix else f"{prefix}.{child.name}"
                self._extract_functions(child, filepath, classname, file_hash)

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
