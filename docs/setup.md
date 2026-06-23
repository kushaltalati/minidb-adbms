# Setup and demonstration guide

## Requirements

- A C++17 compiler — Apple Clang, LLVM Clang, or g++.
- GNU `make`.
- No external libraries. The test framework is built in.

## Build, test, benchmark

```bash
make            # build the interactive shell -> ./minidb
make test       # build + run all 59 unit/integration tests
make bench      # build + run the benchmark harness
make clean      # remove build artefacts and database files
```

`make test` prints a `[PASS]`/`[FAIL]` line per test and a final summary; it
exits non-zero if anything fails. You can run a subset by name substring:

```bash
./build/test_runner btree     # only B+ tree tests
./build/test_runner recovery  # only recovery tests
```

## Running the shell

```bash
./minidb mydata      # uses ./mydata as the storage directory (created if absent)
```

Type SQL ending in `;`. Dot-commands: `.help`, `.tables`, `.stats`, `.exit`.
The prompt becomes `minidb*>` while a transaction is open.

Supported SQL:

```sql
CREATE TABLE t (id INT PRIMARY KEY, name TEXT, n INT);
CREATE INDEX idx_n ON t (n);
INSERT INTO t VALUES (1,'a',10), (2,'b',20);
SELECT * FROM t WHERE n >= 10;
SELECT a.name, b.n FROM t a JOIN t b ON a.id = b.id;
DELETE FROM t WHERE id = 2;
BEGIN;  ...  COMMIT;   -- or ABORT
EXPLAIN SELECT * FROM t WHERE id = 1;
```

---

## Demonstration walkthroughs

These map directly to the features the project must demonstrate.

### A. Storage & buffer pool

```sql
CREATE TABLE big (id INT PRIMARY KEY, v TEXT);
-- insert a few hundred rows (script it or paste), then:
SELECT * FROM big WHERE id = 100;
```
Then run `.stats` to show buffer-pool hits/misses. Re-running the same lookup
increases hits (the page is cached). The benchmark (`make bench`, metric 3)
shows a 100% hit ratio for a hot row.

### B. Index utilisation during query execution

```sql
CREATE TABLE t (id INT PRIMARY KEY, cat INT, payload TEXT);
INSERT INTO t VALUES (1,3,'a'),(2,3,'b'),(3,1,'c'),(4,2,'d'),(5,3,'e');
CREATE INDEX idx_cat ON t (cat);
EXPLAIN SELECT * FROM t WHERE cat = 3;       -- IndexScan: equality is selective enough
EXPLAIN SELECT * FROM t WHERE cat > 1;       -- SeqScan:  a broad range, index not worth it
EXPLAIN SELECT * FROM t WHERE payload = 'b'; -- SeqScan:  no index on payload
```
This shows the optimizer genuinely *choosing* between an index scan and a table
scan on the **same** indexed column — it costs each path (full scan ≈ N vs
index scan ≈ selectivity × N × random-access penalty) and only uses the index
when it is cheaper. `EXPLAIN` prints both costs for the chosen index scan.

### C. JOIN execution and join-order choice

```sql
CREATE TABLE users (id INT PRIMARY KEY, name TEXT);
CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, item TEXT);
INSERT INTO users VALUES (1,'alice'),(2,'bob');
INSERT INTO orders VALUES (10,1,'book'),(11,1,'pen'),(12,2,'lamp');
EXPLAIN SELECT u.name, o.item FROM users u JOIN orders o ON u.id = o.uid;
SELECT u.name, o.item FROM users u JOIN orders o ON u.id = o.uid WHERE u.name='alice';
```
`EXPLAIN` shows which relation is the outer side and whether an index
nested-loop join was chosen.

### D. Transactions, isolation and commit/abort

```sql
BEGIN;
INSERT INTO users VALUES (3,'carol');
SELECT * FROM users;     -- carol is visible inside the transaction
ABORT;
SELECT * FROM users;     -- carol is gone (rolled back)
```

### E. Concurrent transactions, lock acquisition and deadlock

Concurrency uses real threads and is demonstrated by the lock-manager tests:

```bash
./build/test_runner lock
```

- `lock.shared_locks_are_compatible` — two readers share a row.
- `lock.exclusive_is_exclusive_and_blocks` — a writer blocks readers until it
  releases (strict 2PL).
- `lock.deadlock_is_detected` — two transactions each holding a lock then
  requesting the other's; the wait-for graph detects the cycle and exactly one
  transaction is aborted as the victim while the other proceeds.

These print which transaction was chosen as the deadlock victim.

### F. Crash and recovery

The engine has a `simulate_crash()` hook that discards in-memory pages without
flushing — exactly what a power loss does. The test
`engine.crash_recovery_preserves_committed` demonstrates the full cycle:

```bash
./build/test_runner crash_recovery
```

It commits two rows, starts (but never commits) a third, "crashes", reopens the
database, and verifies the two committed rows survive while the uncommitted one
is rolled back — i.e. the WAL + recovery preserved exactly the committed state.

The lower-level recovery tests (`./build/test_runner recovery`) additionally
show committed deletes surviving a crash and that recovery is idempotent if run
twice.

---

## Where data lives

Inside the storage directory you pass to `./minidb`:

- `catalog.meta` — table and index definitions (human-readable).
- `wal.log` — the write-ahead log.
- `table_<id>.db` — one heap file per table.

Delete the directory to start from a clean database.
