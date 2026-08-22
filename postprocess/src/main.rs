//! d3g-postprocess: merge per-process D3G traces and build the dependency
//! graph. Port of the former `d3g/postprocess.py`; AST parsing is delegated
//! to the interpreter given by `--python` (see `d3g.astdump`).
//!
//! Memory is bounded by the object tables, not by the number of calls: the
//! `calls` table is streamed once (joined to each call's children through
//! an index, with `attr_reads` consumed in lockstep), and `executed_lines`
//! and `dataflow_edges` rows are written as they are produced. Only the
//! skeletons of the functions the trace references, the `objects`/`members`
//! tables and the attr_write edges that can decide `default_owner` stay
//! resident.

mod ast;
mod merge;
mod resolve;

use ast::{AstServer, Def};
use indexmap::IndexMap;
use resolve::{AttrRead, CallInfo, Edge, Kind, TargetType};
use rusqlite::{params, Connection, Statement};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::time::Instant;

type Key = (i64, i64);

fn usage() -> ! {
    eprintln!("usage: d3g-postprocess [--python PYTHON] TARGET\n\n\
               TARGET is a trace output directory (its per-process {{pid}}.db files are\n\
               merged into trace.db, which is then postprocessed) or a single trace\n\
               database. PYTHON is the interpreter used to parse source files\n\
               (default: python3).");
    std::process::exit(2);
}

fn main() {
    let mut python = String::from("python3");
    let mut target: Option<PathBuf> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--python" => python = args.next().unwrap_or_else(|| usage()),
            "-h" | "--help" => usage(),
            _ if target.is_none() => target = Some(PathBuf::from(a)),
            _ => usage(),
        }
    }
    let Some(target) = target else { usage() };
    // D3G_REPLAY=skeleton selects the statement-skeleton replay (the
    // original algorithm) instead of the bytecode CFG walk; for comparison.
    let skeleton_replay = std::env::var("D3G_REPLAY").map(|v| v == "skeleton").unwrap_or(false);

    let db_path = if target.is_dir() {
        match merge::merge_traces(&target) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }
    } else {
        target
    };
    if let Err(e) = postprocess(&db_path, &python, skeleton_replay) {
        eprintln!("Error: {e}");
        std::process::exit(1);
    }
}

fn check_machine_id(conn: &Connection) -> Result<(), String> {
    let trace_id: Option<String> = conn
        .query_row("SELECT machine_id FROM machine", [], |r| r.get(0))
        .map_err(|_| "trace has no machine table. Re-collect the trace.".to_string())?;
    let trace_id = match trace_id {
        Some(id) if !id.is_empty() => id,
        _ => return Err("trace has no machine_id. Re-collect the trace.".into()),
    };
    let Ok(local) = std::fs::read_to_string("/etc/machine-id") else { return Ok(()) };
    let local = local.trim();
    if local != trace_id {
        return Err(format!(
            "trace was collected on machine {trace_id}, but this machine is {local}. \
             Postprocess must run on the same machine as the trace."
        ));
    }
    Ok(())
}

/// Function skeletons for every ref in `functions`, fetched lazily per file
/// and retained per ref; the rest of each parsed module is dropped.
struct FuncCache {
    asts: AstServer,
    by_file: HashMap<String, Vec<String>>, // file -> qualnames referenced
    funcs: HashMap<String, Option<Def>>,   // ref -> skeleton (None: not found)
}

impl FuncCache {
    fn new(asts: AstServer, refs: impl Iterator<Item = String>) -> Self {
        let mut by_file: HashMap<String, Vec<String>> = HashMap::new();
        for r in refs {
            if let Some((path, qual)) = r.rsplit_once(':') {
                by_file.entry(path.to_string()).or_default().push(qual.to_string());
            }
        }
        Self { asts, by_file, funcs: HashMap::new() }
    }

    fn get(&mut self, ref_: &str) -> Option<&Def> {
        if !self.funcs.contains_key(ref_) {
            match ref_.rsplit_once(':') {
                None => {
                    self.funcs.insert(ref_.to_string(), None);
                }
                Some((path, _)) => {
                    let quals = self.by_file.remove(path).unwrap_or_default();
                    let mut module = self.asts.module(path);
                    for q in quals {
                        let def = module.as_mut().and_then(|m| m.take_function(&q));
                        self.funcs.insert(format!("{path}:{q}"), def);
                    }
                    // A ref outside the functions table still gets an entry.
                    self.funcs.entry(ref_.to_string()).or_insert(None);
                }
            }
        }
        self.funcs.get(ref_).unwrap().as_ref()
    }
}

struct Writer<'c> {
    edge: Statement<'c>,
    line: Statement<'c>,
    n_edges: usize,
    n_lines: usize,
}

impl<'c> Writer<'c> {
    fn edge(&mut self, e: &Edge) -> rusqlite::Result<()> {
        self.n_edges += 1;
        self.edge
            .execute(params![
                e.pid,
                e.source_call_id,
                e.source_type.as_str(),
                e.source_name,
                e.target_pid,
                e.target_call_id,
                e.target_type.as_str(),
                e.target_name,
                e.member_path
            ])
            .map(|_| ())
    }
}

fn postprocess(db_path: &Path, python: &str, skeleton_replay: bool) -> Result<(), String> {
    let t0 = Instant::now();
    let conn = Connection::open(db_path).map_err(|e| e.to_string())?;
    conn.execute_batch("PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY; PRAGMA cache_size=-65536;")
        .map_err(|e| e.to_string())?;
    check_machine_id(&conn)?;

    // ---- schema and indexes --------------------------------------------
    conn.execute_batch(
        "CREATE INDEX IF NOT EXISTS calls_by_caller ON calls (pid, caller_id);
        DROP TABLE IF EXISTS dataflow_edges;
        CREATE TABLE dataflow_edges (
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
        DROP TABLE IF EXISTS executed_lines;
        CREATE TABLE executed_lines (
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
        );",
    )
    .map_err(|e| e.to_string())?;
    eprintln!("indexed in {:.1}s", t0.elapsed().as_secs_f64());

    // ---- small tables --------------------------------------------------
    let func_map: HashMap<i64, String> = query(&conn, "SELECT function_id, ref FROM functions", |r| {
        Ok((r.get(0)?, r.get(1)?))
    })?
    .into_iter()
    .collect();

    let objects: IndexMap<Key, i64> = query(&conn, "SELECT pid, obj_idx, call_id FROM objects", |r| {
        Ok(((r.get(0)?, r.get(1)?), r.get(2)?))
    })?
    .into_iter()
    .collect();
    let init_calls: HashSet<Key> = objects.iter().map(|(&(pid, _), &cid)| (pid, cid)).collect();

    let mut members: IndexMap<Key, IndexMap<String, i64>> = IndexMap::new();
    for (pid, obj_idx, attr, child_idx) in
        query(&conn, "SELECT pid, obj_idx, attr, child_idx FROM members", |r| {
            Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?, r.get::<_, String>(2)?, r.get::<_, i64>(3)?))
        })?
    {
        members.entry((pid, obj_idx)).or_default().insert(attr, child_idx);
    }
    let n_attr_reads: i64 = conn
        .query_row("SELECT count(*) FROM attr_reads", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;

    let asts = AstServer::spawn(python).map_err(|e| format!("cannot start {python}: {e}"))?;
    let mut funcs = FuncCache::new(asts, func_map.values().cloned());

    // ---- streaming pass over calls -------------------------------------
    let tx = conn.unchecked_transaction().map_err(|e| e.to_string())?;
    let mut w = Writer {
        edge: conn
            .prepare("INSERT INTO dataflow_edges VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)")
            .map_err(|e| e.to_string())?,
        line: conn
            .prepare("INSERT INTO executed_lines VALUES (?1, ?2, ?3, ?4)")
            .map_err(|e| e.to_string())?,
        n_edges: 0,
        n_lines: 0,
    };
    let mut obj_of_call = conn
        .prepare("SELECT obj_id FROM calls WHERE pid = ?1 AND call_id = ?2")
        .map_err(|e| e.to_string())?;

    // Each call joined to its children (index order = child rowid order, so
    // the last child per line wins as in the original dict build).
    let mut calls_stmt = conn
        .prepare(
            "SELECT p.pid, p.call_id, p.function_id, p.obj_id, p.control_flow, c.call_id, c.call_lineno, c.function_id \
             FROM calls p LEFT JOIN calls c ON c.pid = p.pid AND c.caller_id = p.call_id \
             ORDER BY p.rowid",
        )
        .map_err(|e| e.to_string())?;
    let mut calls_rows = calls_stmt.query([]).map_err(|e| e.to_string())?;

    // attr_reads rows are emitted with their call, so rowid order follows
    // calls rowid order; consume them in lockstep.
    let mut ar_stmt = conn
        .prepare("SELECT pid, call_id, caller_id, read_call_lineno FROM attr_reads ORDER BY rowid")
        .map_err(|e| e.to_string())?;
    let mut ar_rows = ar_stmt.query([]).map_err(|e| e.to_string())?;
    let mut ar_pending: Option<(Key, AttrRead)> = None;
    let mut ar_consumed: i64 = 0;

    // attr_write edges whose source is an object's __init__ call, for
    // default_owner: (pid, source_call_id, target_call_id, target_obj_id, attr).
    let mut owner_writes: HashMap<Key, Vec<(i64, i64, String)>> = HashMap::new();

    let (mut n_resolved, mut n_skipped, mut n_calls) = (0usize, 0usize, 0usize);
    let mut current: Option<(CallInfo, Vec<CallInfo>)> = None;

    let mut process = |ci: CallInfo,
                       children: Vec<CallInfo>,
                       funcs: &mut FuncCache,
                       w: &mut Writer,
                       ar_rows: &mut rusqlite::Rows,
                       ar_pending: &mut Option<(Key, AttrRead)>,
                       obj_of_call: &mut Statement|
     -> Result<(), String> {
        // Collect this call's attr_reads from the lockstep cursor.
        let key = (ci.pid, ci.call_id);
        let mut reads: Vec<AttrRead> = Vec::new();
        loop {
            if ar_pending.is_none() {
                match ar_rows.next().map_err(|e| e.to_string())? {
                    Some(r) => {
                        let k: Key = (r.get(0).map_err(|e| e.to_string())?, r.get(1).map_err(|e| e.to_string())?);
                        let ar = AttrRead {
                            caller_id: r.get(2).map_err(|e| e.to_string())?,
                            read_call_lineno: r.get(3).map_err(|e| e.to_string())?,
                        };
                        *ar_pending = Some((k, ar));
                    }
                    None => break,
                }
            }
            if ar_pending.as_ref().unwrap().0 == key {
                reads.push(ar_pending.take().unwrap().1);
                ar_consumed += 1;
            } else {
                break;
            }
        }

        let Some(func) = funcs.get(&ci.ref_) else {
            n_skipped += 1;
            return Ok(());
        };
        let blob = ci.control_flow.as_deref().unwrap_or(&[]);
        let executed = match &func.c {
            Some(cfg) if !skeleton_replay => {
                let ex = resolve::replay_cfg(&func.b, cfg, blob);
                let steps = resolve::REPLAY_STEPS.with(|c| c.get());
                if steps >= 200_000 {
                    eprintln!("  slow replay: {} steps, blob {} bytes, {} stmts, cfg {} nodes: {}", steps, blob.len(), ex.len(), cfg.len(), ci.ref_);
                }
                ex
            }
            _ => resolve::reconstruct(&func.b, blob),
        };
        for (order, stmt) in executed.iter().enumerate() {
            w.line
                .execute(params![ci.pid, ci.call_id, order as i64, stmt.l])
                .map_err(|e| e.to_string())?;
            w.n_lines += 1;
        }
        let child_refs: Vec<&CallInfo> = children.iter().collect();
        let edges = resolve::resolve_intra_call(&ci, func, &executed, &child_refs, &reads);
        for e in &edges {
            w.edge(e).map_err(|e| e.to_string())?;
            // merge_graphs: attr_read edges through tracked members.
            if e.source_type == Kind::AttrRead {
                if let Some(mp) = &e.member_path {
                    if let Some((_, attr)) = mp.rsplit_once('.') {
                        let src_obj: i64 = obj_of_call
                            .query_row(params![e.pid, e.source_call_id], |r| r.get(0))
                            .unwrap_or(0);
                        if src_obj > 0 {
                            if let Some(&child_idx) = members.get(&(e.pid, src_obj)).and_then(|m| m.get(attr)) {
                                let child_call_id = objects.get(&(e.pid, child_idx)).copied().unwrap_or(0);
                                if child_call_id != 0 {
                                    w.edge(&Edge {
                                        pid: e.pid,
                                        source_call_id: child_call_id,
                                        source_type: Kind::Member,
                                        source_name: attr.to_string(),
                                        target_pid: e.target_pid,
                                        target_call_id: e.target_call_id,
                                        target_type: e.target_type,
                                        target_name: e.target_name.clone(),
                                        member_path: Some(mp.clone()),
                                    })
                                    .map_err(|e| e.to_string())?;
                                }
                            }
                        }
                    }
                }
            }
            if e.target_type == TargetType::AttrWrite && init_calls.contains(&(e.pid, e.source_call_id)) {
                let attr = e.target_name.rsplit('.').next().unwrap().to_string();
                owner_writes
                    .entry((e.pid, e.source_call_id))
                    .or_default()
                    .push((e.target_call_id, ci.obj_id, attr));
            }
        }
        n_resolved += 1;
        Ok(())
    };

    loop {
        let row = calls_rows.next().map_err(|e| e.to_string())?;
        let Some(r) = row else { break };
        let pid: i64 = r.get(0).map_err(|e| e.to_string())?;
        let call_id: i64 = r.get(1).map_err(|e| e.to_string())?;
        let same = matches!(&current, Some((c, _)) if c.pid == pid && c.call_id == call_id);
        if !same {
            n_calls += 1;
            if let Some((ci, children)) = current.take() {
                process(ci, children, &mut funcs, &mut w, &mut ar_rows, &mut ar_pending, &mut obj_of_call)?;
            }
            let function_id: i64 = r.get(2).map_err(|e| e.to_string())?;
            let cf: Option<Vec<u8>> = r.get(4).map_err(|e| e.to_string())?;
            current = Some((
                CallInfo {
                    pid,
                    call_id,
                    call_lineno: 0,
                    obj_id: r.get(3).map_err(|e| e.to_string())?,
                    control_flow: cf.filter(|b| !b.is_empty()),
                    ref_: func_map.get(&function_id).cloned().unwrap_or_default(),
                },
                Vec::new(),
            ));
        }
        let child_id: Option<i64> = r.get(5).map_err(|e| e.to_string())?;
        if let Some(child_id) = child_id {
            let (_, children) = current.as_mut().unwrap();
            children.push(CallInfo {
                pid,
                call_id: child_id,
                call_lineno: r.get(6).map_err(|e| e.to_string())?,
                obj_id: 0,
                control_flow: None,
                ref_: {
                    let fid: i64 = r.get(7).map_err(|e| e.to_string())?;
                    func_map.get(&fid).cloned().unwrap_or_default()
                },
            });
        }
        if n_calls % 1_000_000 == 0 && n_calls > 0 && !same {
            eprintln!("  {n_calls} calls, {} edges, {} lines, {:.0}s", w.n_edges, w.n_lines, t0.elapsed().as_secs_f64());
        }
    }
    if let Some((ci, children)) = current.take() {
        process(ci, children, &mut funcs, &mut w, &mut ar_rows, &mut ar_pending, &mut obj_of_call)?;
    }
    drop(calls_rows);
    drop(ar_rows);
    if ar_consumed != n_attr_reads {
        return Err(format!(
            "attr_reads not aligned with calls ({ar_consumed} of {n_attr_reads} consumed); \
             the trace was not written by the D3G writer in call order"
        ));
    }
    drop(funcs);
    eprintln!(
        "resolved {n_resolved} calls ({n_skipped} skipped): {} edges, {} executed lines in {:.1}s",
        w.n_edges,
        w.n_lines,
        t0.elapsed().as_secs_f64()
    );

    // ---- cross-process IPC edges --------------------------------------
    let mut channels: IndexMap<String, Vec<Key>> = IndexMap::new();
    for (pid, name, call_id) in query(&conn, "SELECT pid, name, call_id FROM ipc", |r| {
        Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?, r.get::<_, i64>(2)?))
    })? {
        if call_id > 0 {
            channels.entry(name).or_default().push((pid, call_id));
        }
    }
    for (name, endpoints) in &channels {
        if endpoints.len() < 2 {
            continue;
        }
        for (i, &(src_pid, src_cid)) in endpoints.iter().enumerate() {
            for &(tgt_pid, tgt_cid) in &endpoints[i + 1..] {
                if src_pid == tgt_pid {
                    continue;
                }
                for (a, b) in [((src_pid, src_cid), (tgt_pid, tgt_cid)), ((tgt_pid, tgt_cid), (src_pid, src_cid))] {
                    w.edge(&Edge {
                        pid: a.0,
                        source_call_id: a.1,
                        source_type: Kind::Ipc,
                        source_name: name.clone(),
                        target_pid: b.0,
                        target_call_id: b.1,
                        target_type: TargetType::Ipc,
                        target_name: name.clone(),
                        member_path: None,
                    })
                    .map_err(|e| e.to_string())?;
                }
            }
        }
    }
    // ---- cross-process io edges ---------------------------------------
    // A write to a shared io object (shm segment, mmap'd file) in one
    // process and an overlapping read of the same object in another are
    // paired into a write->read edge. Objects are matched by backing name,
    // ranges are absolute (object offset + op offset).
    struct IoOp {
        pid: i64,
        call_id: i64,
        start: i64,
        end: i64,
    }
    let mut io_writes: IndexMap<String, Vec<IoOp>> = IndexMap::new();
    let mut io_reads: IndexMap<String, Vec<IoOp>> = IndexMap::new();
    for (pid, call_id, start, length, op_type, name) in query(
        &conn,
        "SELECT p.pid, p.call_id, o.offset + p.offset, p.length, p.op_type, o.name \
         FROM io_ops p JOIN io_objects o ON o.pid = p.pid AND o.io_object_id = p.io_object_id \
         ORDER BY p.rowid",
        |r| {
            Ok((
                r.get::<_, i64>(0)?,
                r.get::<_, i64>(1)?,
                r.get::<_, i64>(2)?,
                r.get::<_, i64>(3)?,
                r.get::<_, i64>(4)?,
                r.get::<_, String>(5)?,
            ))
        },
    )? {
        if call_id <= 0 || length <= 0 || name.starts_with("ANONYMOUS_") {
            continue;
        }
        let op = IoOp { pid, call_id, start, end: start + length };
        let map = if op_type == 1 { &mut io_writes } else { &mut io_reads };
        map.entry(name).or_default().push(op);
    }
    let mut n_io_edges = 0usize;
    for (name, writes) in &io_writes {
        let Some(reads) = io_reads.get_mut(name) else { continue };
        if !reads.iter().any(|r| writes.iter().any(|w| w.pid != r.pid)) {
            continue;
        }
        reads.sort_by_key(|r| r.start);
        let mut seen: HashSet<(Key, Key)> = HashSet::new();
        let label = format!("io:{name}");
        for wr in writes {
            for rd in reads.iter() {
                if rd.start >= wr.end {
                    break;
                }
                if rd.end <= wr.start || rd.pid == wr.pid {
                    continue;
                }
                if !seen.insert(((wr.pid, wr.call_id), (rd.pid, rd.call_id))) {
                    continue;
                }
                w.edge(&Edge {
                    pid: wr.pid,
                    source_call_id: wr.call_id,
                    source_type: Kind::Ipc,
                    source_name: label.clone(),
                    target_pid: rd.pid,
                    target_call_id: rd.call_id,
                    target_type: TargetType::Ipc,
                    target_name: label.clone(),
                    member_path: None,
                })
                .map_err(|e| e.to_string())?;
                n_io_edges += 1;
            }
        }
    }
    eprintln!("paired {n_io_edges} cross-process io edges");

    let n_edges = w.n_edges;
    let n_lines = w.n_lines;
    drop(w);
    drop(obj_of_call);

    // ---- default_owner: map each object to its first parent -------------
    let mut child_to_parents: IndexMap<Key, Vec<(i64, &str)>> = IndexMap::new();
    for (&(pid, parent_idx), attrs) in &members {
        for (attr, &child_idx) in attrs {
            child_to_parents.entry((pid, child_idx)).or_default().push((parent_idx, attr.as_str()));
        }
    }

    let mut owner_map: IndexMap<Key, (i64, i64, i64, String)> = IndexMap::new();
    for (&(pid, child_idx), parents) in &child_to_parents {
        let row = if parents.len() == 1 {
            let (owner_idx, attr) = parents[0];
            (pid, child_idx, owner_idx, attr.to_string())
        } else {
            let Some(&child_init_cid) = objects.get(&(pid, child_idx)) else { continue };
            let parent_set: HashSet<i64> = parents.iter().map(|p| p.0).collect();
            let mut best: Option<&(i64, i64, String)> = None;
            if let Some(writes) = owner_writes.get(&(pid, child_init_cid)) {
                for wr in writes {
                    if parent_set.contains(&wr.1) && best.map_or(true, |b| wr.0 < b.0) {
                        best = Some(wr);
                    }
                }
            }
            match best {
                Some((_, owner, attr)) => (pid, child_idx, *owner, attr.clone()),
                None => {
                    let (owner_idx, attr) = parents[0];
                    (pid, child_idx, owner_idx, attr.to_string())
                }
            }
        };
        owner_map.entry((pid, child_idx)).or_insert(row);
    }
    drop(owner_writes);

    // ---- break cycles: order by creation time, remove last -> first edge --
    let mut n_broken = 0usize;
    let keys: Vec<Key> = owner_map.keys().copied().collect();
    let mut visited_global: HashSet<Key> = HashSet::new();
    for key in keys {
        if visited_global.contains(&key) {
            continue;
        }
        let mut path: Vec<Key> = Vec::new();
        let mut path_set: HashSet<Key> = HashSet::new();
        let mut node = key;
        let mut broke = false;
        while !path_set.contains(&node) && owner_map.contains_key(&node) {
            if visited_global.contains(&node) {
                broke = true;
                break;
            }
            path.push(node);
            path_set.insert(node);
            let row = &owner_map[&node];
            node = (row.0, row.2);
        }
        if !broke && path_set.contains(&node) {
            let start = path.iter().position(|&k| k == node).unwrap();
            let mut cycle: Vec<Key> = path[start..].to_vec();
            cycle.sort_by_key(|k| objects.get(k).copied().unwrap_or(0));
            let last = *cycle.last().unwrap();
            owner_map.shift_remove(&last);
            n_broken += 1;
        }
        visited_global.extend(path_set);
    }

    {
        let mut ins = conn
            .prepare("INSERT INTO default_owner VALUES (?1, ?2, ?3, ?4)")
            .map_err(|e| e.to_string())?;
        for row in owner_map.values() {
            ins.execute(params![row.0, row.1, row.2, row.3]).map_err(|e| e.to_string())?;
        }
    }
    tx.commit().map_err(|e| e.to_string())?;

    eprintln!(
        "Postprocessed {}: {n_resolved} calls resolved, {n_skipped} skipped, {n_edges} dataflow edges, \
         {n_lines} executed lines, {} default owners, {n_broken} cycles broken ({:.1}s)",
        db_path.display(),
        owner_map.len(),
        t0.elapsed().as_secs_f64()
    );
    Ok(())
}

fn query<T>(
    conn: &Connection,
    sql: &str,
    f: impl FnMut(&rusqlite::Row<'_>) -> rusqlite::Result<T>,
) -> Result<Vec<T>, String> {
    let mut stmt = conn.prepare(sql).map_err(|e| format!("{sql}: {e}"))?;
    let rows = stmt.query_map([], f).map_err(|e| format!("{sql}: {e}"))?;
    rows.collect::<Result<Vec<T>, _>>().map_err(|e| format!("{sql}: {e}"))
}
