//! `counts` subcommand: how many times each function was invoked, across the
//! whole trace (function ids are already canonicalized by ref in trace.db,
//! so this is a plain group-by over `calls`).

use rusqlite::Connection;

pub fn run(db: &str) -> Result<(), String> {
    let conn = Connection::open(db).map_err(|e| e.to_string())?;
    let mut stmt = conn
        .prepare(
            "SELECT f.ref, COUNT(*) AS n \
             FROM calls c JOIN functions f ON f.function_id = c.function_id \
             GROUP BY f.ref ORDER BY n DESC",
        )
        .map_err(|e| e.to_string())?;
    let mut rows = stmt.query([]).map_err(|e| e.to_string())?;
    while let Some(r) = rows.next().map_err(|e| e.to_string())? {
        let ref_: String = r.get(0).map_err(|e| e.to_string())?;
        let n: i64 = r.get(1).map_err(|e| e.to_string())?;
        println!("{n}\t{ref_}");
    }
    Ok(())
}
