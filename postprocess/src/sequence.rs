//! `sequence` subcommand: prints every traced call in the order it occurred.
//! Ordered by (pid, call_id): call_id is assigned from a monotonically
//! increasing per-process counter at call time (see hook.c's
//! `next_call_id`), so this is exact per-process order; across processes
//! there's no shared clock, so calls are simply grouped by pid.

use rusqlite::Connection;

pub fn run(db: &str) -> Result<(), String> {
    let conn = Connection::open(db).map_err(|e| e.to_string())?;
    let mut stmt = conn
        .prepare(
            "SELECT c.pid, c.call_id, c.caller_id, c.call_lineno, f.ref \
             FROM calls c JOIN functions f ON f.function_id = c.function_id \
             ORDER BY c.pid, c.call_id",
        )
        .map_err(|e| e.to_string())?;
    let mut rows = stmt.query([]).map_err(|e| e.to_string())?;
    while let Some(r) = rows.next().map_err(|e| e.to_string())? {
        let pid: i64 = r.get(0).map_err(|e| e.to_string())?;
        let call_id: i64 = r.get(1).map_err(|e| e.to_string())?;
        let caller_id: i64 = r.get(2).map_err(|e| e.to_string())?;
        let call_lineno: i64 = r.get(3).map_err(|e| e.to_string())?;
        let ref_: String = r.get(4).map_err(|e| e.to_string())?;
        let short: Vec<&str> = ref_.rsplit('/').take(2).collect();
        let short = short.into_iter().rev().collect::<Vec<_>>().join("/");
        println!("{pid}:{call_id} <- {pid}:{caller_id} {short}:{call_lineno}");
    }
    Ok(())
}
