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
mod reach;
mod resolve;

use ast::{AstServer, Def};
use indexmap::IndexMap;
use resolve::{AttrRead, CallInfo, Edge, Kind, TargetType};
use rusqlite::{params, Connection, Statement};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Instant;

type Key = (i64, i64);

fn usage() -> ! {
    eprintln!("usage: d3g-postprocess TRACE_SUBDIR\n\n\
               TRACE_SUBDIR is a subdirectory of $PYTHON_D3G_OUTDIR (its per-process\n\
               {{pid}}.db files are read in place; the dependency graph and\n\
               blob-free copies of the input tables are written to trace.db).\n\
               The interpreter used to parse source files is $VIRTUAL_ENV/bin/python3\n\
               ($VIRTUAL_ENV must be set).\n\n\
               d3g-postprocess reach TRACE_DB SRC_PID:SRC_CALL DST_PID:DST_CALL\n\
               Reports whether a dataflow path connects the two calls, with a\n\
               shortest witness path.");
    std::process::exit(2);
}

fn parse_call(s: &str) -> Option<(i64, i64)> {
    let (pid, cid) = s.split_once(':')?;
    Some((pid.parse().ok()?, cid.parse().ok()?))
}

fn main() {
    let mut args = std::env::args().skip(1);
    if std::env::args().nth(1).as_deref() == Some("reach") {
        let a: Vec<String> = std::env::args().skip(2).collect();
        let (Some(db), Some(src), Some(dst)) =
            (a.first(), a.get(1).and_then(|s| parse_call(s)), a.get(2).and_then(|s| parse_call(s)))
        else {
            usage()
        };
        if let Err(e) = reach::run(db, src, dst) {
            eprintln!("Error: {e}");
            std::process::exit(1);
        }
        return;
    }

    let mut subdir: Option<String> = None;
    while let Some(a) = args.next() {
        match a.as_str() {
            "-h" | "--help" => usage(),
            _ if subdir.is_none() => subdir = Some(a),
            _ => usage(),
        }
    }
    let Some(subdir) = subdir else { usage() };

    let venv = std::env::var("VIRTUAL_ENV").unwrap_or_else(|_| {
        eprintln!("Error: VIRTUAL_ENV not set");
        std::process::exit(1);
    });
    let python = format!("{venv}/bin/python3");

    let outdir = std::env::var("PYTHON_D3G_OUTDIR").unwrap_or_else(|_| {
        eprintln!("Error: PYTHON_D3G_OUTDIR not set");
        std::process::exit(1);
    });
    let target = PathBuf::from(outdir).join(subdir);

    // Read, then mask: the python subprocess spawned below (ast.rs's
    // AstServer) inherits this process's environment, and a d3g-instrumented
    // python3 would otherwise trace itself while merely parsing source for
    // us, polluting (or recursively depending on) the trace we're
    // postprocessing.
    std::env::remove_var("PYTHON_D3G_CONFIG");
    std::env::remove_var("PYTHON_D3G_OUTDIR");

    // D3G_REPLAY=skeleton selects the statement-skeleton replay (the
    // original algorithm) instead of the bytecode CFG walk; for comparison.
    let skeleton_replay = std::env::var("D3G_REPLAY").map(|v| v == "skeleton").unwrap_or(false);

    if let Err(e) = postprocess(&target, &python, skeleton_replay) {
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
                    let mut found = self.asts.functions(path, &quals).unwrap_or_default();
                    for q in quals {
                        let def = found.remove(&q);
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

/// Everything a consumer of trace.db needs except the control_flow blobs,
/// which only resolution reads (directly from the per-process files).
const COPY_SCHEMA: &str = "
    DROP TABLE IF EXISTS meta; CREATE TABLE meta (pid INTEGER);
    DROP TABLE IF EXISTS machine; CREATE TABLE machine (machine_id TEXT NOT NULL);
    DROP TABLE IF EXISTS functions;
    CREATE TABLE functions (function_id INTEGER PRIMARY KEY, ref TEXT NOT NULL, cfg BLOB);
    DROP TABLE IF EXISTS calls;
    CREATE TABLE calls (
        pid INTEGER NOT NULL, call_id INTEGER NOT NULL, function_id INTEGER NOT NULL,
        caller_id INTEGER NOT NULL, call_lineno INTEGER NOT NULL, obj_id INTEGER NOT NULL,
        control_flow BLOB, func_idx INTEGER NOT NULL DEFAULT 0,
        created_id INTEGER NOT NULL DEFAULT 0, created_lineno INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (pid, call_id));
    DROP TABLE IF EXISTS attr_reads;
    CREATE TABLE attr_reads (
        pid INTEGER NOT NULL, call_id INTEGER NOT NULL, caller_id INTEGER NOT NULL,
        write_call_lineno INTEGER NOT NULL, read_call_lineno INTEGER NOT NULL);
    DROP TABLE IF EXISTS call_args;
    CREATE TABLE call_args (
        pid INTEGER NOT NULL, call_id INTEGER NOT NULL, name TEXT NOT NULL,
        obj_idx INTEGER NOT NULL);
    DROP TABLE IF EXISTS objects;
    CREATE TABLE objects (
        pid INTEGER NOT NULL, obj_idx INTEGER NOT NULL, call_id INTEGER NOT NULL,
        PRIMARY KEY (pid, obj_idx));
    DROP TABLE IF EXISTS members;
    CREATE TABLE members (
        pid INTEGER NOT NULL, obj_idx INTEGER NOT NULL, attr TEXT NOT NULL,
        child_idx INTEGER NOT NULL);
    DROP TABLE IF EXISTS ipc;
    CREATE TABLE ipc (pid INTEGER NOT NULL, name TEXT NOT NULL, call_id INTEGER NOT NULL);
    DROP TABLE IF EXISTS io_objects;
    CREATE TABLE io_objects (
        pid INTEGER NOT NULL, io_object_id INTEGER NOT NULL, name TEXT NOT NULL,
        offset INTEGER NOT NULL, PRIMARY KEY (pid, io_object_id));
    DROP TABLE IF EXISTS io_ops;
    CREATE TABLE io_ops (
        pid INTEGER NOT NULL, io_object_id INTEGER NOT NULL, call_id INTEGER NOT NULL,
        offset INTEGER NOT NULL, length INTEGER NOT NULL, op_type INTEGER NOT NULL);";

fn postprocess(target: &Path, python: &str, skeleton_replay: bool) -> Result<(), String> {
    let t0 = Instant::now();
    let is_dir = target.is_dir();
    let (out_path, input_paths) = if is_dir {
        let mut ins: Vec<PathBuf> = std::fs::read_dir(target)
            .map_err(|e| format!("{}: {e}", target.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| {
                p.extension().map_or(false, |x| x == "db")
                    && p.file_name().map_or(false, |n| n != "trace.db")
            })
            .collect();
        ins.sort();
        if ins.is_empty() {
            return Err(format!("no per-process trace databases in {}", target.display()));
        }
        (target.join("trace.db"), ins)
    } else {
        (target.to_path_buf(), vec![target.to_path_buf()])
    };
    let conn = Connection::open(&out_path).map_err(|e| e.to_string())?;
    conn.execute_batch("PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY; PRAGMA cache_size=-65536;")
        .map_err(|e| e.to_string())?;

    // Per-process inputs are read in place; the output database receives the
    // dependency graph plus blob-free copies of every input table. A single
    // database target is both input and output, as before.
    let owned_inputs: Vec<Connection> = if is_dir {
        conn.execute_batch(COPY_SCHEMA).map_err(|e| e.to_string())?;
        let mut v = Vec::new();
        for p in &input_paths {
            let ic = Connection::open(p).map_err(|e| format!("{}: {e}", p.display()))?;
            ic.execute_batch("PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY;")
                .map_err(|e| e.to_string())?;
            check_machine_id(&ic)?;
            v.push(ic);
        }
        v
    } else {
        check_machine_id(&conn)?;
        Vec::new()
    };
    let input_conn = |i: usize| -> &Connection { if is_dir { &owned_inputs[i] } else { &conn } };

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
    // Function ids are interned per process; canonicalize by ref across
    // inputs. fmaps[i] rewrites input i's ids; a single-file target keeps
    // its ids (empty map, identity fallback).
    let mut func_map: HashMap<i64, String> = HashMap::new();
    // Bytecode CFGs recorded by the tracer (see Modules/_tracer/cfg.h);
    // decoded once per function here.
    let mut cfg_map: HashMap<i64, Vec<(i64, u8, i64)>> = HashMap::new();
    let mut fmaps: Vec<HashMap<i64, i64>> = Vec::new();
    {
        let mut ref_to_id: HashMap<String, i64> = HashMap::new();
        let mut canon: Vec<(i64, String, Option<Vec<u8>>)> = Vec::new();
        for i in 0..input_paths.len() {
            let mut fmap = HashMap::new();
            for (fid, ref_, cfg) in
                query(input_conn(i), "SELECT function_id, ref, cfg FROM functions", |r| {
                    Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?, r.get::<_, Option<Vec<u8>>>(2)?))
                })?
            {
                let id = if is_dir {
                    *ref_to_id.entry(ref_.clone()).or_insert_with(|| {
                        canon.push((canon.len() as i64 + 1, ref_.clone(), None));
                        canon.len() as i64
                    })
                } else {
                    fid
                };
                func_map.entry(id).or_insert(ref_);
                if let Some(blob) = cfg {
                    if !cfg_map.contains_key(&id) {
                        cfg_map.insert(id, decode_cfg(&blob));
                        if is_dir {
                            canon[(id - 1) as usize].2 = Some(blob);
                        }
                    }
                }
                fmap.insert(fid, id);
            }
            fmaps.push(fmap);
        }
        if is_dir {
            let mut ins = conn
                .prepare("INSERT INTO functions VALUES (?1, ?2, ?3)")
                .map_err(|e| e.to_string())?;
            for (id, ref_, cfg) in &canon {
                ins.execute(params![id, ref_, cfg]).map_err(|e| e.to_string())?;
            }
        }
    }

    // ---- blob-free copies of the input tables (directory target) -------
    if is_dir {
        for (i, p) in input_paths.iter().enumerate() {
            conn.execute("ATTACH DATABASE ?1 AS child", params![p.to_string_lossy()])
                .map_err(|e| e.to_string())?;
            conn.execute_batch("CREATE TEMP TABLE fmap (old INTEGER PRIMARY KEY, new INTEGER NOT NULL)")
                .map_err(|e| e.to_string())?;
            {
                let mut ins = conn.prepare("INSERT INTO fmap VALUES (?1, ?2)").map_err(|e| e.to_string())?;
                for (old, new) in &fmaps[i] {
                    ins.execute(params![old, new]).map_err(|e| e.to_string())?;
                }
            }
            let child_has = |col: &str| -> bool {
                conn.prepare(&format!("SELECT {col} FROM child.calls LIMIT 1")).is_ok()
            };
            let extra = if child_has("created_id") {
                "c.func_idx, c.created_id, c.created_lineno"
            } else if child_has("func_idx") {
                "c.func_idx, 0, 0"
            } else {
                "0, 0, 0"
            };
            conn.execute_batch(&format!(
                "INSERT INTO calls
                 SELECT c.pid, c.call_id, COALESCE(m.new, c.function_id), c.caller_id,
                        c.call_lineno, c.obj_id, NULL, {extra}
                 FROM child.calls c LEFT JOIN fmap m ON m.old = c.function_id;
                 INSERT INTO meta SELECT * FROM child.meta;
                 INSERT OR IGNORE INTO machine SELECT * FROM child.machine;
                 INSERT INTO attr_reads SELECT * FROM child.attr_reads;
                 INSERT INTO objects SELECT * FROM child.objects;
                 INSERT INTO members SELECT * FROM child.members;
                 INSERT INTO ipc SELECT * FROM child.ipc;
                 INSERT INTO io_objects SELECT * FROM child.io_objects;
                 INSERT INTO io_ops SELECT * FROM child.io_ops;"
            ))
            .map_err(|e| e.to_string())?;
            if conn.prepare("SELECT 1 FROM child.call_args LIMIT 1").is_ok() {
                conn.execute("INSERT INTO call_args SELECT * FROM child.call_args", [])
                    .map_err(|e| e.to_string())?;
            }
            conn.execute_batch("DROP TABLE fmap; DETACH DATABASE child;")
                .map_err(|e| e.to_string())?;
        }
        eprintln!("assembled {} input(s) into {} in {:.1}s", input_paths.len(), out_path.display(), t0.elapsed().as_secs_f64());
    }

    let mut objects: IndexMap<Key, i64> = IndexMap::new();
    let mut members: IndexMap<Key, IndexMap<String, i64>> = IndexMap::new();
    for i in 0..input_paths.len() {
        let ic = input_conn(i);
        for (key, cid) in query(ic, "SELECT pid, obj_idx, call_id FROM objects", |r| {
            Ok(((r.get(0)?, r.get(1)?), r.get(2)?))
        })? {
            objects.insert(key, cid);
        }
        for (pid, obj_idx, attr, child_idx) in
            query(ic, "SELECT pid, obj_idx, attr, child_idx FROM members", |r| {
                Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?, r.get::<_, String>(2)?, r.get::<_, i64>(3)?))
            })?
        {
            members.entry((pid, obj_idx)).or_default().insert(attr, child_idx);
        }
    }
    let init_calls: HashSet<Key> = objects.iter().map(|(&(pid, _), &cid)| (pid, cid)).collect();

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

    // attr_write edges whose source is an object's __init__ call, for
    // default_owner: (pid, source_call_id, target_call_id, target_obj_id, attr).
    let mut owner_writes: HashMap<Key, Vec<(i64, i64, String)>> = HashMap::new();

    let (mut n_resolved, mut n_skipped, mut n_calls) = (0usize, 0usize, 0usize);

    // Per-process trace files are fully independent (each their own db, own
    // rowid ordering, own attr_reads lockstep cursor) once the shared
    // read-only tables above (func_map/cfg_map/members/objects/init_calls)
    // are built -- process them on one thread per input, each with its own
    // connection and its own AstServer subprocess (re-parsing a shared
    // source file per thread trades a little duplicated AST work for not
    // having to share a FuncCache/AstServer pipe across threads). Only the
    // final write into `w`/`owner_writes`/the counters happens back on this
    // thread, so `conn`'s single write path is untouched. Skipped entirely
    // for a single input (including the non-directory case, where the lone
    // "input" is `conn` itself, already mid-transaction).
    let per_input: Vec<Result<InputResult, String>> = if input_paths.len() > 1 {
        thread::scope(|scope| {
            let handles: Vec<_> = (0..input_paths.len())
                .map(|i| {
                    let path = &input_paths[i];
                    let fmap = &fmaps[i];
                    let func_map = &func_map;
                    let cfg_map = &cfg_map;
                    let members = &members;
                    let objects = &objects;
                    let init_calls = &init_calls;
                    scope.spawn(move || {
                        let ic = Connection::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
                        ic.execute_batch("PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY;")
                            .map_err(|e| e.to_string())?;
                        process_input(
                            &ic, fmap, func_map, cfg_map, members, objects, init_calls, python,
                            skeleton_replay,
                        )
                    })
                })
                .collect();
            handles.into_iter().map(|h| h.join().unwrap()).collect()
        })
    } else {
        vec![process_input(
            input_conn(0), &fmaps[0], &func_map, &cfg_map, &members, &objects, &init_calls, python,
            skeleton_replay,
        )]
    };

    for result in per_input {
        let r = result?;
        n_resolved += r.n_resolved;
        n_skipped += r.n_skipped;
        n_calls += r.n_calls;
        if r.ar_consumed != r.n_attr_reads {
            return Err(format!(
                "attr_reads not aligned with calls ({} of {} consumed); \
                 the trace was not written by the D3G writer in call order",
                r.ar_consumed, r.n_attr_reads
            ));
        }
        for e in r.edges {
            w.edge(&e).map_err(|e| e.to_string())?;
        }
        for (pid, call_id, order, lineno) in r.lines {
            w.line
                .execute(params![pid, call_id, order, lineno])
                .map_err(|e| e.to_string())?;
            w.n_lines += 1;
        }
        for (k, mut v) in r.owner_writes {
            owner_writes.entry(k).or_default().append(&mut v);
        }
        eprintln!("  {} calls, {} edges, {} lines, {:.0}s", n_calls, w.n_edges, w.n_lines, t0.elapsed().as_secs_f64());
    }

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
        out_path.display(),
        owner_map.len(),
        t0.elapsed().as_secs_f64()
    );
    Ok(())
}

struct InputResult {
    edges: Vec<Edge>,
    lines: Vec<(i64, i64, i64, i64)>,
    owner_writes: HashMap<Key, Vec<(i64, i64, String)>>,
    n_resolved: usize,
    n_skipped: usize,
    n_calls: usize,
    ar_consumed: i64,
    n_attr_reads: i64,
}

fn process_input(
    ic: &Connection,
    fmap: &HashMap<i64, i64>,
    func_map: &HashMap<i64, String>,
    cfg_map: &HashMap<i64, Vec<(i64, u8, i64)>>,
    members: &IndexMap<Key, IndexMap<String, i64>>,
    objects: &IndexMap<Key, i64>,
    init_calls: &HashSet<Key>,
    python: &str,
    skeleton_replay: bool,
) -> Result<InputResult, String> {
    let n_attr_reads: i64 = ic
        .query_row("SELECT count(*) FROM attr_reads", [], |r| r.get::<_, i64>(0))
        .map_err(|e| e.to_string())?;

    let asts = AstServer::spawn(python).map_err(|e| format!("cannot start {python}: {e}"))?;
    let mut funcs = FuncCache::new(asts, func_map.values().cloned());

    let mut edges: Vec<Edge> = Vec::new();
    let mut lines: Vec<(i64, i64, i64, i64)> = Vec::new();
    let mut owner_writes: HashMap<Key, Vec<(i64, i64, String)>> = HashMap::new();
    let no_args: HashMap<String, i64> = HashMap::new();
    let mut call_args: HashMap<Key, HashMap<String, i64>> = HashMap::new();
    let (mut n_resolved, mut n_skipped, mut n_calls) = (0usize, 0usize, 0usize);
    let mut ar_consumed: i64 = 0;
    {
    let mut obj_of_call = ic
        .prepare("SELECT obj_id FROM calls WHERE pid = ?1 AND call_id = ?2")
        .map_err(|e| e.to_string())?;

    // Callable identity (traces without it resolve as before, minus
    // identity matching of variable callees).
    let has_identity = ic.prepare("SELECT func_idx FROM calls LIMIT 1").is_ok();
    // Generator/coroutine bodies group under their creating call (where the
    // call expression naming their arguments lives), not the resuming one.
    let has_creation = ic.prepare("SELECT created_id FROM calls LIMIT 1").is_ok();
    if has_creation {
        // A virtual generated column keeps the parent join sargable; a bare
        // expression index is not reliably chosen by the planner.
        if ic.prepare("SELECT resolve_parent FROM calls LIMIT 1").is_err() {
            ic.execute_batch(
                "ALTER TABLE calls ADD COLUMN resolve_parent INTEGER \
                 GENERATED ALWAYS AS (ifnull(nullif(created_id, 0), caller_id)) VIRTUAL;",
            )
            .map_err(|e| e.to_string())?;
        }
        ic.execute_batch(
            "CREATE INDEX IF NOT EXISTS calls_by_parent ON calls(pid, resolve_parent);",
        )
        .map_err(|e| e.to_string())?;
    }
    if has_identity {
        let mut stmt = ic
            .prepare("SELECT pid, call_id, name, obj_idx FROM call_args")
            .map_err(|e| e.to_string())?;
        let mut rows = stmt.query([]).map_err(|e| e.to_string())?;
        while let Some(r) = rows.next().map_err(|e| e.to_string())? {
            let key: Key = (r.get_unwrap(0), r.get_unwrap(1));
            call_args
                .entry(key)
                .or_default()
                .insert(r.get_unwrap(2), r.get_unwrap(3));
        }
    }

    // Each call joined to its children (index order = child rowid order, so
    // the last child per line wins as in the original dict build).
    let mut calls_stmt = ic
        .prepare(
            if has_creation {
                "SELECT p.pid, p.call_id, p.function_id, p.obj_id, p.control_flow, c.call_id, \
                        CASE WHEN c.created_id <> 0 AND c.created_lineno <> 0 \
                             THEN c.created_lineno ELSE c.call_lineno END, \
                        c.function_id, c.func_idx \
                 FROM calls p LEFT JOIN calls c \
                   ON c.pid = p.pid AND c.resolve_parent = p.call_id \
                 ORDER BY p.rowid"
            } else if has_identity {
                "SELECT p.pid, p.call_id, p.function_id, p.obj_id, p.control_flow, c.call_id, c.call_lineno, c.function_id, c.func_idx \
                 FROM calls p LEFT JOIN calls c ON c.pid = p.pid AND c.caller_id = p.call_id \
                 ORDER BY p.rowid"
            } else {
                "SELECT p.pid, p.call_id, p.function_id, p.obj_id, p.control_flow, c.call_id, c.call_lineno, c.function_id, 0 \
                 FROM calls p LEFT JOIN calls c ON c.pid = p.pid AND c.caller_id = p.call_id \
                 ORDER BY p.rowid"
            },
        )
        .map_err(|e| e.to_string())?;
    let mut calls_rows = calls_stmt.query([]).map_err(|e| e.to_string())?;

    // attr_reads rows are emitted with their call, so rowid order follows
    // calls rowid order; consume them in lockstep.
    let mut ar_stmt = ic
        .prepare("SELECT pid, call_id, caller_id, read_call_lineno FROM attr_reads ORDER BY rowid")
        .map_err(|e| e.to_string())?;
    let mut ar_rows = ar_stmt.query([]).map_err(|e| e.to_string())?;
    let mut ar_pending: Option<(Key, AttrRead)> = None;

    let mut current: Option<(CallInfo, Vec<CallInfo>)> = None;

    let mut process = |ci: CallInfo,
                       children: Vec<CallInfo>,
                       funcs: &mut FuncCache,
                       edges_out: &mut Vec<Edge>,
                       lines_out: &mut Vec<(i64, i64, i64, i64)>,
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
        let executed = match cfg_map.get(&ci.function_id) {
            Some(cfg) if !skeleton_replay => {
                if blob.len() >= 100_000 {
                    eprintln!("  large control-flow record: {} bytes, cfg {} nodes: {}", blob.len(), cfg.len(), ci.ref_);
                }
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
            lines_out.push((ci.pid, ci.call_id, order as i64, stmt.l));
        }
        let child_refs: Vec<&CallInfo> = children.iter().collect();
        let arg_funcs = call_args.get(&(ci.pid, ci.call_id)).unwrap_or(&no_args);
        let edges = resolve::resolve_intra_call(&ci, func, &executed, &child_refs, &reads, arg_funcs);
        for e in edges {
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
                                    edges_out.push(Edge {
                                        pid: e.pid,
                                        source_call_id: child_call_id,
                                        source_type: Kind::Member,
                                        source_name: attr.to_string(),
                                        target_pid: e.target_pid,
                                        target_call_id: e.target_call_id,
                                        target_type: e.target_type,
                                        target_name: e.target_name.clone(),
                                        member_path: Some(mp.clone()),
                                    });
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
            edges_out.push(e);
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
                process(ci, children, &mut funcs, &mut edges, &mut lines, &mut ar_rows, &mut ar_pending, &mut obj_of_call)?;
            }
            let raw_fid: i64 = r.get(2).map_err(|e| e.to_string())?;
            let function_id = fmap.get(&raw_fid).copied().unwrap_or(raw_fid);
            let cf: Option<Vec<u8>> = r.get(4).map_err(|e| e.to_string())?;
            current = Some((
                CallInfo {
                    pid,
                    call_id,
                    function_id,
                    call_lineno: 0,
                    obj_id: r.get(3).map_err(|e| e.to_string())?,
                    control_flow: cf.filter(|b| !b.is_empty()),
                    ref_: func_map.get(&function_id).cloned().unwrap_or_default(),
                    func_idx: 0,
                },
                Vec::new(),
            ));
        }
        let child_id: Option<i64> = r.get(5).map_err(|e| e.to_string())?;
        if let Some(child_id) = child_id {
            let (_, children) = current.as_mut().unwrap();
            let raw_fid: i64 = r.get(7).map_err(|e| e.to_string())?;
            let fid = fmap.get(&raw_fid).copied().unwrap_or(raw_fid);
            children.push(CallInfo {
                pid,
                call_id: child_id,
                function_id: fid,
                call_lineno: r.get(6).map_err(|e| e.to_string())?,
                obj_id: 0,
                control_flow: None,
                ref_: func_map.get(&fid).cloned().unwrap_or_default(),
                func_idx: r.get(8).map_err(|e| e.to_string())?,
            });
        }
        if n_calls % 1_000_000 == 0 && n_calls > 0 && !same {
            eprintln!("  {n_calls} calls, {} edges, {} lines so far (this input)", edges.len(), lines.len());
        }
    }
    if let Some((ci, children)) = current.take() {
        process(ci, children, &mut funcs, &mut edges, &mut lines, &mut ar_rows, &mut ar_pending, &mut obj_of_call)?;
    }
    drop(calls_rows);
    drop(ar_rows);
    } // per-input streaming loop
    drop(funcs);

    Ok(InputResult { edges, lines, owner_writes, n_resolved, n_skipped, n_calls, ar_consumed, n_attr_reads })
}

/// Decode a tracer CFG blob: 9-byte records {u32 line, u8 kind, u32 target}.
fn decode_cfg(blob: &[u8]) -> Vec<(i64, u8, i64)> {
    blob.chunks_exact(9)
        .map(|c| {
            let line = u32::from_le_bytes([c[0], c[1], c[2], c[3]]) as i64;
            let target = u32::from_le_bytes([c[5], c[6], c[7], c[8]]);
            (line, c[4], if target == 0xFFFF_FFFF { -1 } else { target as i64 })
        })
        .collect()
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
