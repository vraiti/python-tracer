#!/usr/bin/env python3
import glob
import os
import re
import sqlite3
import sys

dbs = []
for p in glob.glob("/tmp/*.db"):
    m = re.match(r"/tmp/(\d+)\.db$", p)
    if m:
        dbs.append((int(m.group(1)), p))

dbs.sort(reverse=True)
if len(dbs) < 2:
    print(f"Need at least 2 /tmp/{{pid}}.db files, found {len(dbs)}", file=sys.stderr)
    sys.exit(1)

parent_pid, parent_path = dbs[0]
child_pid, child_path = dbs[1]
print(f"Parent: {parent_path} (pid {parent_pid})")
print(f"Child:  {child_path} (pid {child_pid})")

out = "/tmp/test_merge.db"
if os.path.exists(out):
    os.remove(out)

conn = sqlite3.connect(out)
conn.execute("ATTACH DATABASE ? AS parent", (parent_path,))
for row in conn.execute("SELECT sql FROM parent.sqlite_master WHERE sql IS NOT NULL"):
    conn.execute(row[0])
for table in ("meta", "functions", "calls", "attr_reads", "objects", "members", "ipc"):
    try:
        conn.execute(f"INSERT INTO {table} SELECT * FROM parent.{table}")
    except sqlite3.Error as e:
        print(f"  copy parent.{table}: {e}")
conn.execute("DETACH DATABASE parent")
conn.commit()
print(f"Copied parent into {out}")

try:
    conn.execute("ATTACH DATABASE ? AS child", (child_path,))
    for table in ("meta", "functions", "calls", "attr_reads", "objects", "members", "ipc"):
        ignore = "OR IGNORE " if table in ("meta", "functions") else ""
        sql = f"INSERT {ignore}INTO {table} SELECT * FROM child.{table}"
        try:
            conn.execute(sql)
            n = conn.execute(f"SELECT changes()").fetchone()[0]
            print(f"  merged child.{table}: {n} rows")
        except sqlite3.Error as e:
            print(f"  merged child.{table}: FAILED: {e}")
    conn.execute("DETACH DATABASE child")
    conn.commit()
    print("Merge succeeded")
except sqlite3.Error as e:
    print(f"Merge failed: {e}")
finally:
    conn.close()
    os.remove(out)
