//! Offline merge of per-process `{pid}.db` files into `trace.db`.
//!
//! Repeatable: inputs are left in place and `trace.db` is regenerated on
//! every run. Rows are already keyed by pid, so tables are unioned as-is,
//! except `functions`: ids are interned per process, so each child's
//! functions are re-keyed by ref and its calls rewritten.

use rusqlite::{params, Connection};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub const MERGED_NAME: &str = "trace.db";

pub fn merge_traces(output_dir: &Path) -> Result<PathBuf, String> {
    let merged_path = output_dir.join(MERGED_NAME);
    let mut inputs: Vec<PathBuf> = std::fs::read_dir(output_dir)
        .map_err(|e| format!("{}: {e}", output_dir.display()))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| {
            p.extension().map_or(false, |x| x == "db")
                && p.file_name().map_or(false, |n| n != MERGED_NAME)
        })
        .collect();
    inputs.sort();
    if inputs.is_empty() {
        return Err(format!("no per-process trace databases in {}", output_dir.display()));
    }

    // Build in a file that no later run could mistake for an input.
    let tmp_path = output_dir.join(format!(".merge-{}.merging", std::process::id()));
    std::fs::copy(&inputs[0], &tmp_path).map_err(|e| format!("copy {}: {e}", inputs[0].display()))?;

    let result = (|| -> rusqlite::Result<()> {
        let mut conn = Connection::open(&tmp_path)?;
        conn.execute_batch("PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY;")?;
        for db_path in &inputs[1..] {
            if let Err(e) = merge_one(&mut conn, db_path) {
                eprintln!("Failed to merge {}: {e}", db_path.display());
                let _ = conn.execute_batch("ROLLBACK");
                let _ = conn.execute_batch("DETACH DATABASE child");
            }
        }
        Ok(())
    })();
    if let Err(e) = result {
        let _ = std::fs::remove_file(&tmp_path);
        return Err(e.to_string());
    }
    std::fs::rename(&tmp_path, &merged_path).map_err(|e| e.to_string())?;
    eprintln!("Merged {} trace(s) into {}", inputs.len(), merged_path.display());
    Ok(merged_path)
}

fn merge_one(conn: &mut Connection, db_path: &Path) -> rusqlite::Result<()> {
    conn.execute("ATTACH DATABASE ?1 AS child", params![db_path.to_string_lossy()])?;
    let tx = conn.unchecked_transaction()?;
    {
        let mut fmap: HashMap<i64, i64> = HashMap::new();
        let mut child_funcs = tx.prepare("SELECT function_id, ref FROM child.functions")?;
        let mut lookup = tx.prepare("SELECT function_id FROM functions WHERE ref = ?1")?;
        let mut insert_fn = tx.prepare("INSERT INTO functions (ref) VALUES (?1)")?;
        let rows: Vec<(i64, String)> = child_funcs
            .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?
            .collect::<Result<_, _>>()?;
        for (fid, ref_) in rows {
            let existing: Option<i64> = lookup.query_row(params![ref_], |r| r.get(0)).ok();
            let id = match existing {
                Some(id) => id,
                None => {
                    insert_fn.execute(params![ref_])?;
                    tx.last_insert_rowid()
                }
            };
            fmap.insert(fid, id);
        }

        tx.execute("INSERT OR IGNORE INTO meta SELECT * FROM child.meta", [])?;
        let mut calls = tx.prepare(
            "SELECT pid, call_id, function_id, caller_id, call_lineno, obj_id, control_flow FROM child.calls",
        )?;
        let mut insert_call = tx.prepare("INSERT INTO calls VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)")?;
        let mut rows = calls.query([])?;
        while let Some(r) = rows.next()? {
            let fid: i64 = r.get(2)?;
            insert_call.execute(params![
                r.get::<_, i64>(0)?,
                r.get::<_, i64>(1)?,
                fmap.get(&fid).copied().unwrap_or(fid),
                r.get::<_, i64>(3)?,
                r.get::<_, i64>(4)?,
                r.get::<_, i64>(5)?,
                r.get::<_, Option<Vec<u8>>>(6)?,
            ])?;
        }
        for table in ["attr_reads", "objects", "members", "ipc", "io_objects", "io_ops"] {
            tx.execute(&format!("INSERT INTO {table} SELECT * FROM child.{table}"), [])?;
        }
    }
    tx.commit()?;
    conn.execute("DETACH DATABASE child", [])?;
    Ok(())
}
