//! Statement skeletons produced by `python -m d3g.astdump`.
//!
//! Parsing is delegated to the interpreter that ran the traced program, so
//! the statement structure replayed against the recorded control-flow bits
//! is exactly the one that interpreter executed. Only what the resolver
//! consumes crosses the pipe; the Python AST itself is discarded per file.

use serde::Deserialize;
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

#[derive(Deserialize)]
#[serde(tag = "k")]
pub enum Expr {
    #[serde(rename = "n")]
    Name { id: String },
    #[serde(rename = "a")]
    Attr { c: Option<String>, l: i64 },
    #[serde(rename = "c")]
    Call {
        l: i64,
        f: Option<String>,
        o: Option<String>,
        a: Vec<Expr>,
        kw: Vec<(Option<String>, Expr)>,
        nm: Vec<String>,
    },
    #[serde(rename = "x")]
    Other { nm: Vec<String> },
}

#[derive(Deserialize)]
#[serde(tag = "k")]
pub enum Target {
    #[serde(rename = "n")]
    Name { id: String },
    #[serde(rename = "a")]
    Attr { c: Option<String> },
    #[serde(rename = "x")]
    Other,
}

#[derive(Deserialize)]
#[serde(tag = "t")]
pub enum StmtKind {
    #[serde(rename = "for")]
    For {
        tg: Option<String>,
        it: Expr,
        b: Vec<Stmt>,
        e: Vec<Stmt>,
    },
    #[serde(rename = "while")]
    While { b: Vec<Stmt>, e: Vec<Stmt> },
    #[serde(rename = "if")]
    If { b: Vec<Stmt>, e: Vec<Stmt> },
    #[serde(rename = "with")]
    With { b: Vec<Stmt> },
    #[serde(rename = "try")]
    Try { b: Vec<Stmt> },
    #[serde(rename = "assign")]
    Assign { tg: Vec<Target>, v: Expr },
    #[serde(rename = "ann")]
    AnnAssign { tg: Option<String>, v: Option<Expr> },
    #[serde(rename = "aug")]
    AugAssign { tg: Option<String>, v: Expr },
    #[serde(rename = "ret")]
    Return { v: Option<Expr> },
    #[serde(rename = "expr")]
    ExprCall { v: Expr },
    #[serde(rename = "o")]
    Other,
}

#[derive(Deserialize)]
pub struct Stmt {
    pub l: i64,
    #[serde(flatten)]
    pub kind: StmtKind,
}

#[derive(Deserialize)]
pub struct Def {
    pub n: String,
    #[serde(rename = "fn")]
    pub is_fn: bool,
    #[serde(default)]
    pub p: Vec<String>,
    #[serde(default)]
    pub b: Vec<Stmt>,
    #[serde(default)]
    pub d: Vec<Def>,
    /// Bytecode control-flow graph: `[line, kind, target]` per node (see
    /// astdump.py); absent when the file did not compile.
    #[serde(default)]
    pub c: Option<Vec<(i64, u8, i64)>>,
}

#[derive(Deserialize)]
pub struct Module {
    pub d: Vec<Def>,
}

impl Module {
    /// `_find_function_node`: descend the qualname through directly nested
    /// definitions; the final node must be a function. The function's
    /// parameters and body are moved out so the module can be dropped while
    /// only the functions the trace references stay resident; nested
    /// definitions remain reachable through the emptied shell.
    pub fn take_function(&mut self, qualname: &str) -> Option<Def> {
        fn descend<'a>(defs: &'a mut [Def], parts: &[&str]) -> Option<&'a mut Def> {
            let (first, rest) = parts.split_first()?;
            let idx = defs.iter().position(|d| d.n == *first)?;
            let found = &mut defs[idx];
            if rest.is_empty() {
                Some(found)
            } else {
                descend(&mut found.d, rest)
            }
        }
        let parts: Vec<&str> = qualname.split('.').collect();
        let node = descend(&mut self.d, &parts)?;
        if !node.is_fn {
            return None;
        }
        Some(Def {
            n: node.n.clone(),
            is_fn: true,
            p: std::mem::take(&mut node.p),
            b: std::mem::take(&mut node.b),
            c: node.c.take(),
            d: Vec::new(),
        })
    }
}

/// Persistent `python -m d3g.astdump` child; one request line per file.
pub struct AstServer {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    line: String,
}

impl AstServer {
    pub fn spawn(python: &str) -> std::io::Result<Self> {
        let mut child = Command::new(python)
            .args(["-m", "d3g.astdump"])
            // Never trace the helper, whatever the caller's environment.
            .env_remove("PYTHON_TRACER_CONFIG")
            .env_remove("PYTHON_TRACER_OUTDIR")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()?;
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        Ok(Self { child, stdin, stdout, line: String::new() })
    }

    /// Parse `path`. `None` when the file is unreadable or does not parse.
    /// Nothing is cached here: the caller keeps only what it needs.
    pub fn module(&mut self, path: &str) -> Option<Module> {
        if path.contains('\n') {
            return None;
        }
        writeln!(self.stdin, "{path}").ok()?;
        self.stdin.flush().ok()?;
        self.line.clear();
        let n = self.stdout.read_line(&mut self.line).ok()?;
        if n == 0 {
            eprintln!("d3g-postprocess: AST helper exited");
            std::process::exit(1);
        }
        // Deeply nested bodies exceed serde_json's default depth limit.
        let mut de = serde_json::Deserializer::from_str(&self.line);
        de.disable_recursion_limit();
        match <Option<Module> as serde::Deserialize>::deserialize(&mut de) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("d3g-postprocess: bad AST reply for {path}: {e}");
                None
            }
        }
    }
}

impl Drop for AstServer {
    fn drop(&mut self) {
        // Closing stdin ends the helper's input loop.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
