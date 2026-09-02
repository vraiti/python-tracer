//! `reach` subcommand: reachability over dataflow_edges with a witness path.
//!
//! Loads the edge list into an in-memory CSR (both directions) and runs a
//! bidirectional BFS between two calls. Nodes are (pid, call_id) pairs.

use rusqlite::{params, Connection};
use std::collections::HashMap;
use std::time::Instant;

fn pack(pid: i64, call_id: i64) -> u64 {
    ((pid as u64) << 32) | (call_id as u64 & 0xFFFF_FFFF)
}

fn unpack(key: u64) -> (i64, i64) {
    ((key >> 32) as i64, (key & 0xFFFF_FFFF) as i64)
}

struct Csr {
    offsets: Vec<u32>,
    targets: Vec<u32>,
}

impl Csr {
    fn build(n_nodes: usize, edges: impl Iterator<Item = (u32, u32)> + Clone) -> Csr {
        let mut offsets = vec![0u32; n_nodes + 1];
        for (s, _) in edges.clone() {
            offsets[s as usize + 1] += 1;
        }
        for i in 0..n_nodes {
            offsets[i + 1] += offsets[i];
        }
        let mut cursor = offsets.clone();
        let mut targets = vec![0u32; offsets[n_nodes] as usize];
        for (s, t) in edges {
            targets[cursor[s as usize] as usize] = t;
            cursor[s as usize] += 1;
        }
        Csr { offsets, targets }
    }

    fn neighbors(&self, n: u32) -> &[u32] {
        &self.targets[self.offsets[n as usize] as usize..self.offsets[n as usize + 1] as usize]
    }
}

pub fn run(db: &str, src: (i64, i64), dst: (i64, i64)) -> Result<(), String> {
    let conn = Connection::open(db).map_err(|e| e.to_string())?;
    let t0 = Instant::now();

    // Load edges, compacting (pid, call_id) keys to dense u32 ids.
    let mut ids: HashMap<u64, u32> = HashMap::new();
    let mut keys: Vec<u64> = Vec::new();
    let intern = |k: u64, keys: &mut Vec<u64>, ids: &mut HashMap<u64, u32>| -> u32 {
        *ids.entry(k).or_insert_with(|| {
            keys.push(k);
            (keys.len() - 1) as u32
        })
    };
    let mut edges: Vec<(u32, u32)> = Vec::new();
    {
        let mut stmt = conn
            .prepare("SELECT pid, source_call_id, target_pid, target_call_id FROM dataflow_edges")
            .map_err(|e| e.to_string())?;
        let mut rows = stmt.query([]).map_err(|e| e.to_string())?;
        while let Some(r) = rows.next().map_err(|e| e.to_string())? {
            let s = pack(r.get_unwrap(0), r.get_unwrap(1));
            let t = pack(r.get_unwrap(2), r.get_unwrap(3));
            edges.push((
                intern(s, &mut keys, &mut ids),
                intern(t, &mut keys, &mut ids),
            ));
        }
    }
    let n = keys.len();
    let fwd = Csr::build(n, edges.iter().copied());
    let bwd = Csr::build(n, edges.iter().map(|&(s, t)| (t, s)));
    drop(edges);
    eprintln!("loaded {} edges, {} nodes in {:.1}s", fwd.targets.len(), n, t0.elapsed().as_secs_f64());

    let (Some(&src_id), Some(&dst_id)) = (ids.get(&pack(src.0, src.1)), ids.get(&pack(dst.0, dst.1))) else {
        return Err("source or destination call has no dataflow edges".into());
    };
    drop(ids);

    // Bidirectional BFS, expanding the smaller frontier. parent = own id marks a root.
    let t1 = Instant::now();
    const UNSEEN: u32 = u32::MAX;
    let mut parent_f = vec![UNSEEN; n];
    let mut parent_b = vec![UNSEEN; n];
    parent_f[src_id as usize] = src_id;
    parent_b[dst_id as usize] = dst_id;
    let mut qf = vec![src_id];
    let mut qb = vec![dst_id];
    let mut meet: Option<u32> = None;
    'search: while !qf.is_empty() && !qb.is_empty() {
        let forward = qf.len() <= qb.len();
        let (q, seen, other, adj) = if forward {
            (&mut qf, &mut parent_f, &parent_b, &fwd)
        } else {
            (&mut qb, &mut parent_b, &parent_f, &bwd)
        };
        let mut next = Vec::new();
        for &node in q.iter() {
            for &nb in adj.neighbors(node) {
                if seen[nb as usize] == UNSEEN {
                    seen[nb as usize] = node;
                    if other[nb as usize] != UNSEEN {
                        meet = Some(nb);
                        break 'search;
                    }
                    next.push(nb);
                }
            }
        }
        *q = next;
    }

    let Some(meet) = meet.or_else(|| (src_id == dst_id).then_some(src_id)) else {
        println!("goal NOT reached ({:.2}s)", t1.elapsed().as_secs_f64());
        return Ok(());
    };
    let mut path: Vec<u32> = Vec::new();
    let mut node = meet;
    while parent_f[node as usize] != node {
        path.push(node);
        node = parent_f[node as usize];
    }
    path.push(node);
    path.reverse();
    node = meet;
    while parent_b[node as usize] != node {
        node = parent_b[node as usize];
        path.push(node);
    }
    println!("goal REACHED, {} hops ({:.2}s)", path.len() - 1, t1.elapsed().as_secs_f64());

    let mut name = conn
        .prepare(
            "SELECT f.ref FROM calls c JOIN functions f ON f.function_id = c.function_id \
             WHERE c.pid = ?1 AND c.call_id = ?2",
        )
        .map_err(|e| e.to_string())?;
    for &id in &path {
        let (pid, cid) = unpack(keys[id as usize]);
        let ref_: String = name
            .query_row(params![pid, cid], |r| r.get(0))
            .unwrap_or_else(|_| "?".into());
        let short: Vec<&str> = ref_.rsplit('/').take(2).collect();
        println!("  {pid}:{cid} {}", short.into_iter().rev().collect::<Vec<_>>().join("/"));
    }
    Ok(())
}
