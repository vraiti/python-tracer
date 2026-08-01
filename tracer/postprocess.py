from __future__ import annotations

import argparse
import ast
import os
import sqlite3
import sys
from dataclasses import dataclass, field
from typing import Any


_CF_TYPES = (ast.If, ast.While, ast.For, ast.AsyncFor)


@dataclass
class DataflowEdge:
    pid: int
    source_call_id: int
    source_type: str
    source_name: str
    target_pid: int
    target_call_id: int
    target_type: str
    target_name: str
    member_path: str | None = None


@dataclass
class CallInfo:
    pid: int
    call_id: int
    function_id: int
    caller_id: int
    call_lineno: int
    obj_id: int
    control_flow: bytes | None
    ref: str = ""


@dataclass
class AttrRead:
    pid: int
    call_id: int
    caller_id: int
    write_call_lineno: int
    read_call_lineno: int


# ---------------------------------------------------------------------------
# AST helpers
# ---------------------------------------------------------------------------

def _parse_file(filepath: str) -> ast.Module | None:
    try:
        with open(filepath) as f:
            source = f.read()
        return ast.parse(source, filepath)
    except (SyntaxError, OSError):
        return None


def _find_function_node(tree: ast.Module, qualname: str) -> ast.FunctionDef | ast.AsyncFunctionDef | None:
    parts = qualname.split(".")
    node: ast.AST = tree
    for part in parts:
        found = False
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                if child.name == part:
                    node = child
                    found = True
                    break
        if not found:
            return None
    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
        return node
    return None


# ---------------------------------------------------------------------------
# Execution order reconstruction
# ---------------------------------------------------------------------------

def reconstruct_executed_stmts(
    func_node: ast.FunctionDef | ast.AsyncFunctionDef,
    cf_blob: bytes | None,
) -> list[ast.stmt]:
    blob = cf_blob or b""
    bit_pos = [0]

    def consume() -> int:
        byte_idx = bit_pos[0] // 8
        if byte_idx >= len(blob):
            return 0
        val = (blob[byte_idx] >> (bit_pos[0] % 8)) & 1
        bit_pos[0] += 1
        return val

    executed: list[ast.stmt] = []

    def walk_body(stmts: list[ast.stmt]) -> None:
        for stmt in stmts:
            executed.append(stmt)

            if isinstance(stmt, (ast.For, ast.AsyncFor)):
                while bit_pos[0] // 8 < len(blob):
                    decision = consume()
                    if decision == 1:
                        walk_body(stmt.body)
                    else:
                        break
                else:
                    consume()
                if stmt.orelse:
                    walk_body(stmt.orelse)

            elif isinstance(stmt, ast.While):
                while bit_pos[0] // 8 < len(blob):
                    decision = consume()
                    if decision == 1:
                        walk_body(stmt.body)
                    else:
                        break
                else:
                    consume()
                if stmt.orelse:
                    walk_body(stmt.orelse)

            elif isinstance(stmt, ast.If):
                decision = consume()
                if decision == 1:
                    walk_body(stmt.body)
                elif stmt.orelse:
                    walk_body(stmt.orelse)

            elif isinstance(stmt, (ast.With, ast.AsyncWith)):
                walk_body(stmt.body)

            elif isinstance(stmt, ast.Try):
                walk_body(stmt.body)

            elif hasattr(ast, "TryStar") and isinstance(stmt, ast.TryStar):
                walk_body(stmt.body)

    walk_body(func_node.body)
    return executed


# ---------------------------------------------------------------------------
# Expression name extraction
# ---------------------------------------------------------------------------

def _expr_names(expr: ast.expr) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(expr):
        if isinstance(node, ast.Name):
            names.add(node.id)
    return names


def _attr_chain(expr: ast.expr) -> str | None:
    parts: list[str] = []
    node = expr
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        parts.reverse()
        return ".".join(parts)
    return None


# ---------------------------------------------------------------------------
# Source tracking: what produced each local variable
# ---------------------------------------------------------------------------

@dataclass
class ValueSource:
    kind: str  # 'param', 'attr_read', 'call_result', 'literal', 'composite'
    name: str
    call_id: int = 0
    lineno: int = 0
    member_path: str | None = None
    sources: list[ValueSource] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Intra-call dataflow resolution
# ---------------------------------------------------------------------------

def resolve_intra_call(
    call: CallInfo,
    func_node: ast.FunctionDef | ast.AsyncFunctionDef,
    child_calls: list[CallInfo],
    attr_reads: list[AttrRead],
) -> list[DataflowEdge]:
    executed = reconstruct_executed_stmts(func_node, call.control_flow)

    symbols: dict[str, ValueSource] = {}

    args = func_node.args
    param_names: list[str] = []
    for a in args.posonlyargs + args.args:
        param_names.append(a.arg)
    for a in args.kwonlyargs:
        param_names.append(a.arg)
    if args.vararg:
        param_names.append(args.vararg.arg)
    if args.kwarg:
        param_names.append(args.kwarg.arg)

    for pname in param_names:
        symbols[pname] = ValueSource(kind="param", name=pname, call_id=call.call_id)

    ar_by_line: dict[int, AttrRead] = {}
    for ar in attr_reads:
        ar_by_line[ar.read_call_lineno] = ar

    child_by_lineno: dict[int, CallInfo] = {}
    for ch in child_calls:
        child_by_lineno[ch.call_lineno] = ch

    edges: list[DataflowEdge] = []

    executed_lines: set[int] = set()
    for stmt in executed:
        executed_lines.add(stmt.lineno)

    def source_from_expr(expr: ast.expr) -> ValueSource:
        if isinstance(expr, ast.Name):
            if expr.id in symbols:
                return symbols[expr.id]
            return ValueSource(kind="literal", name=expr.id)

        if isinstance(expr, ast.Attribute):
            chain = _attr_chain(expr)
            if chain and expr.lineno in ar_by_line:
                ar = ar_by_line[expr.lineno]
                return ValueSource(
                    kind="attr_read",
                    name=chain,
                    call_id=ar.caller_id,
                    lineno=ar.write_call_lineno,
                    member_path=chain,
                )
            if chain:
                base = chain.split(".")[0]
                if base in symbols:
                    src = symbols[base]
                    return ValueSource(
                        kind=src.kind,
                        name=chain,
                        call_id=src.call_id,
                        member_path=chain,
                        sources=[src],
                    )
            return ValueSource(kind="literal", name=chain or "?")

        names = _expr_names(expr)
        sources = []
        for n in names:
            if n in symbols:
                sources.append(symbols[n])
        if len(sources) == 1:
            return sources[0]
        if sources:
            return ValueSource(kind="composite", name=",".join(sorted(names)), sources=sources)
        return ValueSource(kind="literal", name="<expr>")

    def emit_edge(src: ValueSource, target_call_id: int, target_type: str, target_name: str) -> None:
        if src.kind == "composite":
            for s in src.sources:
                emit_edge(s, target_call_id, target_type, target_name)
            return
        if src.kind == "literal":
            return
        edges.append(DataflowEdge(
            pid=call.pid,
            source_call_id=src.call_id,
            source_type=src.kind,
            source_name=src.name,
            target_pid=call.pid,
            target_call_id=target_call_id,
            target_type=target_type,
            target_name=target_name,
            member_path=src.member_path,
        ))

    for stmt in executed:
        if isinstance(stmt, ast.Assign):
            src = source_from_expr(stmt.value)
            for target in stmt.targets:
                if isinstance(target, ast.Name):
                    symbols[target.id] = src
                elif isinstance(target, ast.Attribute):
                    chain = _attr_chain(target)
                    if chain:
                        emit_edge(src, call.call_id, "attr_write", chain)

        elif isinstance(stmt, ast.AnnAssign) and stmt.value is not None:
            src = source_from_expr(stmt.value)
            if isinstance(stmt.target, ast.Name):
                symbols[stmt.target.id] = src

        elif isinstance(stmt, ast.AugAssign):
            if isinstance(stmt.target, ast.Name):
                old = symbols.get(stmt.target.id)
                new_src = source_from_expr(stmt.value)
                if old:
                    symbols[stmt.target.id] = ValueSource(
                        kind="composite",
                        name=stmt.target.id,
                        sources=[old, new_src],
                    )
                else:
                    symbols[stmt.target.id] = new_src

        elif isinstance(stmt, ast.Return) and stmt.value is not None:
            src = source_from_expr(stmt.value)
            emit_edge(src, call.call_id, "return", "return")

        elif isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            _process_call_expr(stmt.value, stmt.lineno, call, child_by_lineno, symbols, source_from_expr, emit_edge)

        elif isinstance(stmt, ast.Assign) and isinstance(stmt.value, ast.Call):
            _process_call_expr(stmt.value, stmt.lineno, call, child_by_lineno, symbols, source_from_expr, emit_edge)

        # Conditionals consume values from their test expression
        if isinstance(stmt, (ast.If, ast.While)):
            names = _expr_names(stmt.test)
            for n in names:
                if n in symbols:
                    pass  # consumed but not propagated

        if isinstance(stmt, (ast.For, ast.AsyncFor)):
            iter_src = source_from_expr(stmt.iter)
            if isinstance(stmt.target, ast.Name):
                symbols[stmt.target.id] = ValueSource(
                    kind=iter_src.kind,
                    name=stmt.target.id,
                    call_id=iter_src.call_id,
                    member_path=iter_src.member_path,
                    sources=[iter_src],
                )

    return edges


def _process_call_expr(
    call_expr: ast.Call,
    lineno: int,
    parent_call: CallInfo,
    child_by_lineno: dict[int, CallInfo],
    symbols: dict[str, ValueSource],
    source_from_expr: Any,
    emit_edge: Any,
) -> None:
    child = child_by_lineno.get(lineno)
    if child is None:
        return

    func_node_ast = call_expr.func
    if isinstance(func_node_ast, ast.Attribute) and isinstance(func_node_ast.value, ast.Name):
        obj_name = func_node_ast.value.id
        if obj_name in symbols:
            emit_edge(symbols[obj_name], child.call_id, "arg", "self")

    for i, arg in enumerate(call_expr.args):
        src = source_from_expr(arg)
        emit_edge(src, child.call_id, "arg", f"arg{i}")

    for kw in call_expr.keywords:
        src = source_from_expr(kw.value)
        name = kw.arg if kw.arg else "**kwargs"
        emit_edge(src, child.call_id, "arg", name)


# ---------------------------------------------------------------------------
# Cross-call graph merge
# ---------------------------------------------------------------------------

def merge_graphs(
    all_edges: list[DataflowEdge],
    calls: dict[tuple[int, int], CallInfo],
    members: dict[tuple[int, int], dict[str, int]],
    objects: dict[tuple[int, int], int],
) -> list[DataflowEdge]:
    merged = list(all_edges)

    obj_call_to_idx: dict[tuple[int, int], tuple[int, int]] = {}
    for (pid, idx), cid in objects.items():
        obj_call_to_idx[(pid, cid)] = (pid, idx)

    for edge in all_edges:
        if edge.source_type == "attr_read" and edge.member_path:
            parts = edge.member_path.split(".")
            if len(parts) >= 2:
                attr = parts[-1]
                src_call = calls.get((edge.pid, edge.source_call_id))
                if src_call and src_call.obj_id > 0:
                    obj_members = members.get((edge.pid, src_call.obj_id), {})
                    if attr in obj_members:
                        child_idx = obj_members[attr]
                        child_call_id = objects.get((edge.pid, child_idx), 0)
                        if child_call_id:
                            merged.append(DataflowEdge(
                                pid=edge.pid,
                                source_call_id=child_call_id,
                                source_type="member",
                                source_name=attr,
                                target_pid=edge.target_pid,
                                target_call_id=edge.target_call_id,
                                target_type=edge.target_type,
                                target_name=edge.target_name,
                                member_path=edge.member_path,
                            ))

    return merged


# ---------------------------------------------------------------------------
# Cross-process IPC edges
# ---------------------------------------------------------------------------

def resolve_ipc_edges(
    c: Any,
) -> list[DataflowEdge]:
    from collections import defaultdict

    channels: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for pid, name, call_id in c.execute("SELECT pid, name, obj_idx FROM ipc"):
        if call_id > 0:
            channels[name].append((pid, call_id))

    edges: list[DataflowEdge] = []
    for name, endpoints in channels.items():
        if len(endpoints) < 2:
            continue
        for i, (src_pid, src_cid) in enumerate(endpoints):
            for tgt_pid, tgt_cid in endpoints[i + 1:]:
                if src_pid == tgt_pid:
                    continue
                edges.append(DataflowEdge(
                    pid=src_pid,
                    source_call_id=src_cid,
                    source_type="ipc",
                    source_name=name,
                    target_pid=tgt_pid,
                    target_call_id=tgt_cid,
                    target_type="ipc",
                    target_name=name,
                ))
                edges.append(DataflowEdge(
                    pid=tgt_pid,
                    source_call_id=tgt_cid,
                    source_type="ipc",
                    source_name=name,
                    target_pid=src_pid,
                    target_call_id=src_cid,
                    target_type="ipc",
                    target_name=name,
                ))
    return edges


# ---------------------------------------------------------------------------
# Main postprocessor
# ---------------------------------------------------------------------------

def _check_machine_id(c: Any) -> None:
    try:
        row = c.execute("SELECT machine_id FROM machine").fetchone()
    except Exception:
        return
    if row is None:
        return
    trace_id = row[0]
    if not trace_id:
        return
    local_id = ""
    try:
        with open("/etc/machine-id") as f:
            local_id = f.read().strip()
    except OSError:
        return
    if local_id != trace_id:
        print(
            f"Error: trace was collected on machine {trace_id}, "
            f"but this machine is {local_id}. "
            f"Postprocess must run on the same machine as the trace.",
            file=sys.stderr,
        )
        sys.exit(1)


def postprocess(db_path: str) -> None:
    conn = sqlite3.connect(db_path)
    c = conn.cursor()
    _check_machine_id(c)

    func_map: dict[int, str] = {}
    for fid, ref in c.execute("SELECT function_id, ref FROM functions"):
        func_map[fid] = ref

    calls: dict[tuple[int, int], CallInfo] = {}
    for row in c.execute("SELECT pid, call_id, function_id, caller_id, call_lineno, obj_id, control_flow FROM calls"):
        ci = CallInfo(
            pid=row[0],
            call_id=row[1],
            function_id=row[2],
            caller_id=row[3],
            call_lineno=row[4],
            obj_id=row[5],
            control_flow=row[6],
            ref=func_map.get(row[2], ""),
        )
        calls[(ci.pid, ci.call_id)] = ci

    attr_reads_by_call: dict[tuple[int, int], list[AttrRead]] = {}
    for row in c.execute("SELECT pid, call_id, caller_id, write_call_lineno, read_call_lineno FROM attr_reads"):
        ar = AttrRead(pid=row[0], call_id=row[1], caller_id=row[2], write_call_lineno=row[3], read_call_lineno=row[4])
        attr_reads_by_call.setdefault((ar.pid, ar.call_id), []).append(ar)

    objects: dict[tuple[int, int], int] = {}
    for row in c.execute("SELECT pid, obj_idx, call_id FROM objects"):
        objects[(row[0], row[1])] = row[2]

    members: dict[tuple[int, int], dict[str, int]] = {}
    for row in c.execute("SELECT pid, obj_idx, attr, child_idx FROM members"):
        members.setdefault((row[0], row[1]), {})[row[2]] = row[3]

    children_by_caller: dict[tuple[int, int], list[CallInfo]] = {}
    for ci in calls.values():
        children_by_caller.setdefault((ci.pid, ci.caller_id), []).append(ci)

    file_asts: dict[str, ast.Module] = {}
    func_nodes: dict[str, ast.FunctionDef | ast.AsyncFunctionDef | None] = {}

    def get_func_node(ref: str) -> ast.FunctionDef | ast.AsyncFunctionDef | None:
        if ref in func_nodes:
            return func_nodes[ref]
        if ":" not in ref:
            func_nodes[ref] = None
            return None
        filepath, qualname = ref.rsplit(":", 1)
        if filepath not in file_asts:
            tree = _parse_file(filepath)
            if tree is None:
                file_asts[filepath] = ast.Module(body=[], type_ignores=[])
            else:
                file_asts[filepath] = tree
        tree = file_asts[filepath]
        node = _find_function_node(tree, qualname)
        func_nodes[ref] = node
        return node

    all_edges: list[DataflowEdge] = []
    all_executed: list[tuple[int, int, int, int]] = []

    n_resolved = 0
    n_skipped = 0
    for ci in calls.values():
        func_node = get_func_node(ci.ref)
        if func_node is None:
            n_skipped += 1
            continue

        executed = reconstruct_executed_stmts(func_node, ci.control_flow)
        for order, stmt in enumerate(executed):
            all_executed.append((ci.pid, ci.call_id, order, stmt.lineno))

        child_calls = children_by_caller.get((ci.pid, ci.call_id), [])
        call_attr_reads = attr_reads_by_call.get((ci.pid, ci.call_id), [])

        edges = resolve_intra_call(ci, func_node, child_calls, call_attr_reads)
        all_edges.extend(edges)
        n_resolved += 1

    merged = merge_graphs(all_edges, calls, members, objects)
    ipc_edges = resolve_ipc_edges(c)
    merged.extend(ipc_edges)

    # --- default_owner: map each object to its first parent ---

    child_to_parents: dict[tuple[int, int], list[tuple[int, str]]] = {}
    for (pid, parent_idx), attrs in members.items():
        for attr, child_idx in attrs.items():
            child_to_parents.setdefault((pid, child_idx), []).append((parent_idx, attr))

    obj_init_call: dict[tuple[int, int], int] = {}
    for (pid, obj_idx), call_id in objects.items():
        obj_init_call[(pid, obj_idx)] = call_id

    attr_write_edges: list[DataflowEdge] = [
        e for e in merged if e.target_type == "attr_write"
    ]

    default_owners: list[tuple[int, int, int, str]] = []
    for (pid, child_idx), parents in child_to_parents.items():
        if len(parents) == 1:
            owner_idx, attr = parents[0]
            default_owners.append((pid, child_idx, owner_idx, attr))
            continue

        child_init_cid = obj_init_call.get((pid, child_idx))
        if child_init_cid is None:
            continue

        parent_set = {p for p, _ in parents}
        best: tuple[int, int, str] | None = None
        for e in attr_write_edges:
            if e.pid != pid:
                continue
            if e.source_call_id != child_init_cid:
                continue
            target_call = calls.get((e.pid, e.target_call_id))
            if target_call is None:
                continue
            if target_call.obj_id in parent_set:
                if best is None or e.target_call_id < best[0]:
                    attr_name = e.target_name.split(".")[-1] if "." in e.target_name else e.target_name
                    best = (e.target_call_id, target_call.obj_id, attr_name)

        if best:
            default_owners.append((pid, child_idx, best[1], best[2]))
        else:
            owner_idx, attr = parents[0]
            default_owners.append((pid, child_idx, owner_idx, attr))

    c.executescript("""
        CREATE TABLE IF NOT EXISTS dataflow_edges (
            pid INTEGER NOT NULL,
            source_call_id INTEGER NOT NULL,
            source_type TEXT NOT NULL,
            source_name TEXT NOT NULL,
            target_pid INTEGER NOT NULL,
            target_call_id INTEGER NOT NULL,
            target_type TEXT NOT NULL,
            target_name TEXT NOT NULL,
            member_path TEXT
        );
        CREATE TABLE IF NOT EXISTS executed_lines (
            pid INTEGER NOT NULL,
            call_id INTEGER NOT NULL,
            line_order INTEGER NOT NULL,
            lineno INTEGER NOT NULL
        );
        DROP TABLE IF EXISTS default_owner;
        CREATE TABLE default_owner (
            pid INTEGER NOT NULL,
            obj_idx INTEGER NOT NULL,
            owner_idx INTEGER NOT NULL,
            attr TEXT NOT NULL,
            PRIMARY KEY (pid, obj_idx)
        );
    """)

    c.executemany(
        "INSERT INTO dataflow_edges VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [
            (e.pid, e.source_call_id, e.source_type, e.source_name,
             e.target_pid, e.target_call_id, e.target_type, e.target_name, e.member_path)
            for e in merged
        ],
    )

    c.executemany(
        "INSERT INTO executed_lines VALUES (?, ?, ?, ?)",
        all_executed,
    )

    seen_owners: set[tuple[int, int]] = set()
    deduped_owners = []
    for row in default_owners:
        key = (row[0], row[1])
        if key not in seen_owners:
            seen_owners.add(key)
            deduped_owners.append(row)

    c.executemany(
        "INSERT INTO default_owner VALUES (?, ?, ?, ?)",
        deduped_owners,
    )

    conn.commit()
    conn.close()

    print(
        f"Postprocessed {db_path}: {n_resolved} calls resolved, {n_skipped} skipped, "
        f"{len(merged)} dataflow edges, {len(all_executed)} executed lines, "
        f"{len(deduped_owners)} default owners",
        file=sys.stderr,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="python-tracer postprocessor")
    parser.add_argument("db", help="path to trace SQLite database")
    args = parser.parse_args()
    postprocess(args.db)


if __name__ == "__main__":
    main()
