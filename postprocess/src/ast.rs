//! Statement skeletons produced by `python -m d3g.astdump`.
//!
//! Parsing is delegated to the interpreter that ran the traced program.
//! Only the functions the trace references are requested, and only what the
//! resolver consumes crosses the pipe, in the binary record format described
//! in `Lib/d3g/astdump.py`.

use std::collections::HashMap;
use std::io::{BufReader, Read, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

pub enum Expr {
    Name { id: String },
    Attr { c: Option<String>, l: i64 },
    Call {
        l: i64,
        f: Option<String>,
        o: Option<String>,
        a: Vec<Expr>,
        kw: Vec<(Option<String>, Expr)>,
        nm: Vec<String>,
    },
    Other { nm: Vec<String> },
}

pub enum Target {
    Name { id: String },
    Attr { c: Option<String> },
    Other,
}

pub enum StmtKind {
    For { tg: Option<String>, it: Expr, b: Vec<Stmt>, e: Vec<Stmt> },
    While { t: Expr, b: Vec<Stmt>, e: Vec<Stmt> },
    If { t: Expr, b: Vec<Stmt>, e: Vec<Stmt> },
    With { b: Vec<Stmt> },
    /// `h`: handler, else and finally bodies (reached only through the
    /// bytecode replay; the statement-skeleton fallback ignores them).
    Try { b: Vec<Stmt>, h: Vec<Stmt> },
    Assign { tg: Vec<Target>, v: Expr },
    AnnAssign { tg: Option<String>, v: Option<Expr> },
    AugAssign { tg: Option<String>, v: Expr },
    Return { v: Option<Expr> },
    ExprCall { v: Expr },
    Other,
}

pub struct Stmt {
    pub l: i64,
    pub kind: StmtKind,
}

pub struct Def {
    pub p: Vec<String>,
    pub b: Vec<Stmt>,
}

const NONE: u32 = 0xFFFF_FFFF;

struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
    strings: Vec<String>,
}

type R<T> = Result<T, String>;

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> R<&'a [u8]> {
        let end = self.pos + n;
        if end > self.buf.len() {
            return Err("truncated AST record".into());
        }
        let s = &self.buf[self.pos..end];
        self.pos = end;
        Ok(s)
    }
    fn u8(&mut self) -> R<u8> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> R<usize> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]) as usize)
    }
    fn u32(&mut self) -> R<u32> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn string(&mut self) -> R<String> {
        let i = self.u32()?;
        self.strings.get(i as usize).cloned().ok_or_else(|| "bad string index".to_string())
    }
    fn opt_string(&mut self) -> R<Option<String>> {
        let i = self.u32()?;
        if i == NONE {
            return Ok(None);
        }
        self.strings.get(i as usize).cloned().map(Some).ok_or_else(|| "bad string index".to_string())
    }
    fn names(&mut self) -> R<Vec<String>> {
        let n = self.u16()?;
        (0..n).map(|_| self.string()).collect()
    }
    fn expr(&mut self) -> R<Expr> {
        Ok(match self.u8()? {
            0 => Expr::Name { id: self.string()? },
            1 => Expr::Attr { c: self.opt_string()?, l: self.u32()? as i64 },
            2 => {
                let l = self.u32()? as i64;
                let f = self.opt_string()?;
                let o = self.opt_string()?;
                let na = self.u16()?;
                let a = (0..na).map(|_| self.expr()).collect::<R<_>>()?;
                let nk = self.u16()?;
                let mut kw = Vec::with_capacity(nk);
                for _ in 0..nk {
                    let name = self.opt_string()?;
                    kw.push((name, self.expr()?));
                }
                Expr::Call { l, f, o, a, kw, nm: self.names()? }
            }
            3 => Expr::Other { nm: self.names()? },
            t => return Err(format!("bad expr tag {t}")),
        })
    }
    fn opt_expr(&mut self) -> R<Option<Expr>> {
        Ok(if self.u8()? == 0 { None } else { Some(self.expr()?) })
    }
    fn target(&mut self) -> R<Target> {
        Ok(match self.u8()? {
            0 => Target::Name { id: self.string()? },
            1 => Target::Attr { c: self.opt_string()? },
            _ => Target::Other,
        })
    }
    fn body(&mut self) -> R<Vec<Stmt>> {
        let n = self.u32()? as usize;
        let mut out = Vec::with_capacity(n.min(1 << 16));
        for _ in 0..n {
            out.push(self.stmt()?);
        }
        Ok(out)
    }
    fn stmt(&mut self) -> R<Stmt> {
        let l = self.u32()? as i64;
        let kind = match self.u8()? {
            1 => StmtKind::For { tg: self.opt_string()?, it: self.expr()?, b: self.body()?, e: self.body()? },
            2 => StmtKind::While { t: self.expr()?, b: self.body()?, e: self.body()? },
            3 => StmtKind::If { t: self.expr()?, b: self.body()?, e: self.body()? },
            4 => StmtKind::With { b: self.body()? },
            5 => StmtKind::Try { b: self.body()?, h: self.body()? },
            6 => {
                let n = self.u16()?;
                let tg = (0..n).map(|_| self.target()).collect::<R<_>>()?;
                StmtKind::Assign { tg, v: self.expr()? }
            }
            7 => StmtKind::AnnAssign { tg: self.opt_string()?, v: self.opt_expr()? },
            8 => StmtKind::AugAssign { tg: self.opt_string()?, v: self.expr()? },
            9 => StmtKind::Return { v: self.opt_expr()? },
            10 => StmtKind::ExprCall { v: self.expr()? },
            _ => StmtKind::Other,
        };
        Ok(Stmt { l, kind })
    }
}

/// Decode one reply: `None` when the file was unavailable, else the
/// requested functions by qualname.
fn decode(buf: &[u8]) -> R<Option<HashMap<String, Def>>> {
    let mut r = Reader { buf, pos: 0, strings: Vec::new() };
    if r.u8()? == 0 {
        return Ok(None);
    }
    let ns = r.u32()? as usize;
    r.strings.reserve(ns);
    for _ in 0..ns {
        let n = r.u16()?;
        let bytes = r.take(n)?;
        r.strings.push(String::from_utf8_lossy(bytes).into_owned());
    }
    let nf = r.u32()? as usize;
    let mut out = HashMap::with_capacity(nf);
    for _ in 0..nf {
        let q = r.string()?;
        let np = r.u16()?;
        let p = (0..np).map(|_| r.string()).collect::<R<_>>()?;
        let b = r.body()?;
        out.insert(q, Def { p, b });
    }
    Ok(Some(out))
}

/// Persistent `python -m d3g.astdump` child; one request line per file.
pub struct AstServer {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    buf: Vec<u8>,
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
        Ok(Self { child, stdin, stdout, buf: Vec::new() })
    }

    /// Fetch `qualnames` from `path`. `None` when the file is unreadable
    /// or does not parse; functions not found are absent from the map.
    pub fn functions(&mut self, path: &str, qualnames: &[String]) -> Option<HashMap<String, Def>> {
        if path.contains(['\n', '\t']) || qualnames.iter().any(|q| q.contains(['\n', '\t'])) {
            return None;
        }
        let mut req = String::from(path);
        for q in qualnames {
            req.push('\t');
            req.push_str(q);
        }
        req.push('\n');
        self.stdin.write_all(req.as_bytes()).ok()?;
        self.stdin.flush().ok()?;
        let mut len = [0u8; 4];
        if self.stdout.read_exact(&mut len).is_err() {
            eprintln!("d3g-postprocess: AST helper exited");
            std::process::exit(1);
        }
        let n = u32::from_le_bytes(len) as usize;
        self.buf.resize(n, 0);
        if self.stdout.read_exact(&mut self.buf).is_err() {
            eprintln!("d3g-postprocess: AST helper exited");
            std::process::exit(1);
        }
        match decode(&self.buf) {
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
