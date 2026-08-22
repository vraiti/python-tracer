//! Per-call replay and intra-call dataflow resolution.

use crate::ast::{Def, Expr, Stmt, StmtKind, Target};
use std::collections::{HashMap, VecDeque};
use std::rc::Rc;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Param,
    AttrRead,
    Literal,
    Composite,
    CallResult,
    Member,
    Ipc,
}

impl Kind {
    pub fn as_str(self) -> &'static str {
        match self {
            Kind::Param => "param",
            Kind::AttrRead => "attr_read",
            Kind::Literal => "literal",
            Kind::Composite => "composite",
            Kind::CallResult => "call_result",
            Kind::Member => "member",
            Kind::Ipc => "ipc",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TargetType {
    Arg,
    AttrWrite,
    Return,
    Ipc,
}

impl TargetType {
    pub fn as_str(self) -> &'static str {
        match self {
            TargetType::Arg => "arg",
            TargetType::AttrWrite => "attr_write",
            TargetType::Return => "return",
            TargetType::Ipc => "ipc",
        }
    }
}

pub struct Edge {
    pub pid: i64,
    pub source_call_id: i64,
    pub source_type: Kind,
    pub source_name: String,
    pub target_pid: i64,
    pub target_call_id: i64,
    pub target_type: TargetType,
    pub target_name: String,
    pub member_path: Option<String>,
}

pub struct CallInfo {
    pub pid: i64,
    pub call_id: i64,
    pub call_lineno: i64,
    pub obj_id: i64,
    pub control_flow: Option<Vec<u8>>,
    pub ref_: String,
}

pub struct AttrRead {
    pub caller_id: i64,
    pub read_call_lineno: i64,
}

/// Replay the branch bytes over the function's bytecode control-flow
/// graph (`Def::c`), yielding the executed statements in order: each time
/// execution enters a line that starts statements, those statements are
/// emitted. Consecutive re-entries of the same statement (a multi-line
/// statement whose bytecode revisits its first line) collapse to one.
///
/// Node kinds follow astdump.py: 0 linear, 1 conditional jump (byte 1 =
/// taken), 2 FOR_ITER (byte 1 = exhausted), 3 unconditional jump, 4 SEND
/// (await: continue at END_SEND), 5 GET_ANEXT (byte 0 per attempt; a
/// following 2 means the loop is exhausted), 6 return/raise.
pub fn replay_cfg<'a>(body: &'a [Stmt], cfg: &[(i64, u8, i64)], blob: &[u8]) -> Vec<&'a Stmt> {
    fn index<'a>(stmts: &'a [Stmt], by_line: &mut HashMap<i64, Vec<&'a Stmt>>) {
        for s in stmts {
            by_line.entry(s.l).or_default().push(s);
            match &s.kind {
                StmtKind::For { b, e, .. } | StmtKind::While { b, e } | StmtKind::If { b, e } => {
                    index(b, by_line);
                    index(e, by_line);
                }
                StmtKind::With { b } | StmtKind::Try { b } => index(b, by_line),
                _ => {}
            }
        }
    }
    let mut by_line: HashMap<i64, Vec<&'a Stmt>> = HashMap::new();
    index(body, &mut by_line);

    let mut out: Vec<&'a Stmt> = Vec::new();
    let mut pos = 0usize;
    let mut idx = 0usize;
    let mut last_line = -1i64;
    // Back edges taken without consuming any byte since the previous pass
    // (e.g. `while True:` whose exit is an exception) terminate the walk.
    let mut back_edge_pos: HashMap<usize, usize> = HashMap::new();
    let mut steps = 0usize;
    while idx < cfg.len() && steps < 4_000_000 {
        steps += 1;
        let (line, kind, target) = cfg[idx];
        if line > 0 && line != last_line {
            if let Some(stmts) = by_line.get(&line) {
                for &st in stmts {
                    if out.last().map_or(true, |&l| !std::ptr::eq(l, st)) {
                        out.push(st);
                    }
                }
            }
            last_line = line;
        }
        let jump = |taken: bool, idx: usize| if taken { target as usize } else { idx + 1 };
        idx = match kind {
            1 | 2 => match blob.get(pos) {
                Some(&b) => {
                    pos += 1;
                    jump(b == 1, idx)
                }
                None => break,
            },
            3 => {
                let t = target as usize;
                if t <= idx {
                    if back_edge_pos.get(&idx) == Some(&pos) {
                        break;
                    }
                    back_edge_pos.insert(idx, pos);
                }
                t
            }
            4 => target as usize,
            5 => {
                if blob.get(pos).is_none() {
                    break;
                }
                pos += 1;
                if blob.get(pos) == Some(&2) {
                    pos += 1;
                    target as usize
                } else {
                    idx + 1
                }
            }
            6 => break,
            _ => idx + 1,
        };
    }
    REPLAY_STEPS.with(|c| c.set(steps));
    out
}

thread_local! {
    /// Steps taken by the most recent `replay_cfg` (diagnostics).
    pub static REPLAY_STEPS: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

/// `reconstruct_executed_stmts`: replay the branch bytes over the body.
/// 0 = jump not taken (if-body runs / loop yields), 1 = taken.
pub fn reconstruct<'a>(body: &'a [Stmt], blob: &[u8]) -> Vec<&'a Stmt> {
    struct Walker<'a, 'b> {
        blob: &'b [u8],
        pos: usize,
        out: Vec<&'a Stmt>,
    }
    impl<'a, 'b> Walker<'a, 'b> {
        fn consume(&mut self) -> Option<u8> {
            let v = self.blob.get(self.pos).copied();
            if v.is_some() {
                self.pos += 1;
            }
            v
        }
        fn walk(&mut self, stmts: &'a [Stmt]) {
            for stmt in stmts {
                self.out.push(stmt);
                match &stmt.kind {
                    StmtKind::For { b, e, .. } | StmtKind::While { b, e } => {
                        while self.consume() == Some(0) {
                            self.walk(b);
                        }
                        if !e.is_empty() {
                            self.walk(e);
                        }
                    }
                    StmtKind::If { b, e } => {
                        let decision = self.consume();
                        if decision == Some(0) {
                            self.walk(b);
                        } else if !e.is_empty() {
                            self.walk(e);
                        }
                    }
                    StmtKind::With { b } | StmtKind::Try { b } => self.walk(b),
                    _ => {}
                }
            }
        }
    }
    let mut w = Walker { blob, pos: 0, out: Vec::new() };
    w.walk(body);
    w.out
}

struct ValueSource {
    kind: Kind,
    name: String,
    call_id: i64,
    member_path: Option<String>,
    sources: Vec<Rc<ValueSource>>,
}

fn literal(name: impl Into<String>) -> Rc<ValueSource> {
    Rc::new(ValueSource {
        kind: Kind::Literal,
        name: name.into(),
        call_id: 0,
        member_path: None,
        sources: Vec::new(),
    })
}

struct Resolver<'a> {
    call: &'a CallInfo,
    symbols: HashMap<String, Rc<ValueSource>>,
    ar_by_line: HashMap<i64, &'a AttrRead>,
    /// Traced children per call line, in call order; each call expression
    /// the replay reaches consumes the next one, so repeated executions of
    /// a line (loops) map onto successive children.
    child_queues: HashMap<i64, VecDeque<&'a CallInfo>>,
    /// The child most recently consumed on each line; the value a call
    /// expression produced.
    child_current: HashMap<i64, &'a CallInfo>,
    edges: Vec<Edge>,
}

impl<'a> Resolver<'a> {
    fn source_from_expr(&self, expr: &Expr) -> Rc<ValueSource> {
        match expr {
            Expr::Name { id } => match self.symbols.get(id) {
                Some(s) => s.clone(),
                None => literal(id.clone()),
            },
            Expr::Attr { c: chain, l } => {
                if let Some(chain) = chain {
                    if let Some(ar) = self.ar_by_line.get(l) {
                        return Rc::new(ValueSource {
                            kind: Kind::AttrRead,
                            name: chain.clone(),
                            call_id: ar.caller_id,
                            member_path: Some(chain.clone()),
                            sources: Vec::new(),
                        });
                    }
                    let base = chain.split('.').next().unwrap();
                    if let Some(src) = self.symbols.get(base) {
                        return Rc::new(ValueSource {
                            kind: src.kind,
                            name: chain.clone(),
                            call_id: src.call_id,
                            member_path: Some(chain.clone()),
                            sources: vec![src.clone()],
                        });
                    }
                    literal(chain.clone())
                } else {
                    literal("?")
                }
            }
            Expr::Call { l, .. } if self.child_current.contains_key(l) => {
                // The value is whatever the traced child call returned; its
                // provenance continues through that call's own `return` edges.
                Rc::new(ValueSource {
                    kind: Kind::CallResult,
                    name: "return".to_string(),
                    call_id: self.child_current[l].call_id,
                    member_path: None,
                    sources: Vec::new(),
                })
            }
            Expr::Call { nm, .. } | Expr::Other { nm } => {
                let sources: Vec<Rc<ValueSource>> =
                    nm.iter().filter_map(|n| self.symbols.get(n).cloned()).collect();
                match sources.len() {
                    1 => sources.into_iter().next().unwrap(),
                    0 => literal("<expr>"),
                    _ => Rc::new(ValueSource {
                        kind: Kind::Composite,
                        name: nm.join(","),
                        call_id: 0,
                        member_path: None,
                        sources,
                    }),
                }
            }
        }
    }

    fn emit_edge(&mut self, src: &Rc<ValueSource>, target_call_id: i64, target_type: TargetType, target_name: &str) {
        match src.kind {
            Kind::Composite => {
                for s in &src.sources {
                    self.emit_edge(s, target_call_id, target_type, target_name);
                }
            }
            Kind::Literal => {}
            _ => self.edges.push(Edge {
                pid: self.call.pid,
                source_call_id: src.call_id,
                source_type: src.kind,
                source_name: src.name.clone(),
                target_pid: self.call.pid,
                target_call_id,
                target_type,
                target_name: target_name.to_string(),
                member_path: src.member_path.clone(),
            }),
        }
    }

    /// Emit `arg` edges for every call nested anywhere in `expr`, each
    /// matched to a traced child by its own line.
    fn process_calls(&mut self, expr: &Expr) {
        match expr {
            Expr::Call { l, a, kw, .. } => {
                // Arguments are evaluated (and their calls made) before the
                // outer call, so consume children innermost first.
                for arg in a {
                    self.process_calls(arg);
                }
                for (_, value) in kw {
                    self.process_calls(value);
                }
                self.process_call_expr(expr, *l);
            }
            _ => {}
        }
    }

    /// Take the traced child on `lineno` that this call expression made.
    /// Several traced calls can share a line (attribute protocol, generator
    /// bookkeeping, the call itself), so prefer the first whose qualname
    /// matches the callee; otherwise the first that is not an attribute
    /// protocol method.
    fn take_child(&mut self, lineno: i64, callee: Option<&str>) -> Option<&'a CallInfo> {
        let q = self.child_queues.get_mut(&lineno)?;
        let pos = callee
            .and_then(|f| q.iter().position(|c| callee_matches(&c.ref_, f)))
            .or_else(|| q.iter().position(|c| !is_attr_protocol(&c.ref_)))?;
        let child = q.remove(pos)?;
        self.child_current.insert(lineno, child);
        Some(child)
    }

    fn process_call_expr(&mut self, expr: &Expr, lineno: i64) {
        let Expr::Call { f, o, a, kw, .. } = expr else { return };
        let Some(child) = self.take_child(lineno, f.as_deref()) else { return };
        let child_id = child.call_id;
        if let Some(obj) = o {
            if let Some(src) = self.symbols.get(obj).cloned() {
                self.emit_edge(&src, child_id, TargetType::Arg, "self");
            }
        }
        for (i, arg) in a.iter().enumerate() {
            let src = self.source_from_expr(arg);
            self.emit_edge(&src, child_id, TargetType::Arg, &format!("arg{i}"));
        }
        for (name, value) in kw {
            let src = self.source_from_expr(value);
            let name = name.as_deref().unwrap_or("**kwargs");
            self.emit_edge(&src, child_id, TargetType::Arg, name);
        }
    }
}

fn qualname_parts(ref_: &str) -> impl Iterator<Item = &str> {
    ref_.rsplit(':').next().unwrap_or("").split('.')
}

/// `ref_` names a function the call expression `callee(...)` could have
/// entered directly: the function itself, or a class's constructor.
fn callee_matches(ref_: &str, callee: &str) -> bool {
    let parts: Vec<&str> = qualname_parts(ref_).collect();
    let n = parts.len();
    match parts.last() {
        Some(&last) if last == callee => true,
        Some(&"__init__") | Some(&"__new__") | Some(&"__call__") => n >= 2 && parts[n - 2] == callee,
        _ => false,
    }
}

fn is_attr_protocol(ref_: &str) -> bool {
    matches!(
        qualname_parts(ref_).last(),
        Some("__getattr__") | Some("__getattribute__") | Some("__get__") | Some("__set__")
    )
}

/// `resolve_intra_call`. `executed` is the replayed statement list.
pub fn resolve_intra_call<'a>(
    call: &'a CallInfo,
    func: &Def,
    executed: &[&Stmt],
    child_calls: &[&'a CallInfo],
    attr_reads: &'a [AttrRead],
) -> Vec<Edge> {
    let mut r = Resolver {
        call,
        symbols: HashMap::new(),
        ar_by_line: HashMap::new(),
        child_queues: HashMap::new(),
        child_current: HashMap::new(),
        edges: Vec::new(),
    };
    for p in &func.p {
        r.symbols.insert(
            p.clone(),
            Rc::new(ValueSource {
                kind: Kind::Param,
                name: p.clone(),
                call_id: call.call_id,
                member_path: None,
                sources: Vec::new(),
            }),
        );
    }
    // Later entries win, as with dict assignment in the original.
    for ar in attr_reads {
        r.ar_by_line.insert(ar.read_call_lineno, ar);
    }
    for ch in child_calls {
        r.child_queues.entry(ch.call_lineno).or_default().push_back(ch);
    }

    for stmt in executed {
        match &stmt.kind {
            StmtKind::Assign { tg, v } => {
                r.process_calls(v);
                let src = r.source_from_expr(v);
                for t in tg {
                    match t {
                        Target::Name { id } => {
                            r.symbols.insert(id.clone(), src.clone());
                        }
                        Target::Attr { c: Some(chain) } => {
                            let chain = chain.clone();
                            r.emit_edge(&src, call.call_id, TargetType::AttrWrite, &chain);
                        }
                        _ => {}
                    }
                }
            }
            StmtKind::AnnAssign { tg, v: Some(v) } => {
                r.process_calls(v);
                let src = r.source_from_expr(v);
                if let Some(id) = tg {
                    r.symbols.insert(id.clone(), src);
                }
            }
            StmtKind::AugAssign { tg: Some(id), v } => {
                r.process_calls(v);
                let old = r.symbols.get(id).cloned();
                let new_src = r.source_from_expr(v);
                let merged = match old {
                    Some(old) => Rc::new(ValueSource {
                        kind: Kind::Composite,
                        name: id.clone(),
                        call_id: 0,
                        member_path: None,
                        sources: vec![old, new_src],
                    }),
                    None => new_src,
                };
                r.symbols.insert(id.clone(), merged);
            }
            StmtKind::Return { v: Some(v) } => {
                r.process_calls(v);
                let src = r.source_from_expr(v);
                r.emit_edge(&src, call.call_id, TargetType::Return, "return");
            }
            StmtKind::ExprCall { v } => r.process_calls(v),
            _ => {}
        }

        if let StmtKind::For { tg: Some(id), it, .. } = &stmt.kind {
            // `for x in f(a)`: arguments flow into the iterator call and the
            // loop target derives from what it produced.
            r.process_calls(it);
            let iter_src = r.source_from_expr(it);
            r.symbols.insert(
                id.clone(),
                Rc::new(ValueSource {
                    kind: iter_src.kind,
                    name: id.clone(),
                    call_id: iter_src.call_id,
                    member_path: iter_src.member_path.clone(),
                    sources: vec![iter_src],
                }),
            );
        }
    }
    r.edges
}
