# Design decisions and trade-offs

This is the "why" behind MiniDB. For the viva, each of these is a decision we
can defend, including what we gave up.

## Language and build: C++17 + a plain Makefile

C++ gives us direct control over memory layout (important for an on-disk page
format) without a runtime getting in the way. We use a hand-written `Makefile`
rather than CMake because the toolchain only ships `make`, and a short Makefile
is easy to read during the demo.

One bug taught us a lesson worth recording: the Makefile **must** track header
dependencies (`-MMD -MP`). Without it, editing a struct in a header left stale
`.o` files compiled against the *old* layout, which linked into silent memory
corruption. The fix (auto-generated `.d` files) is now in the Makefile.

## Storage: slotted pages, append-style heap

We use slotted pages because they are the standard way to store
variable-length records while keeping a stable record id (`RID`). Deletes
tombstone the slot rather than compacting, which keeps RIDs stable — and stable
RIDs matter because the B+ tree stores them.

**Trade-off:** space freed by a delete in an earlier page is not reused for new
inserts (inserts append to the last page or a new one). A production system
tracks free space per page (PostgreSQL's Free Space Map). We left this as noted
future work; it does not affect correctness.

## Indexes are in-memory and rebuilt at startup

The B+ tree lives in memory and is rebuilt by scanning the heap when the database
opens. This is the most consequential simplification, and it buys a lot:

- **Recovery only has to worry about the heap.** Because indexes are derived
  from the heap, after we recover the heap we just rebuild the trees. We never
  log index page splits/merges, which is the single most complex part of real
  recovery (ARIES on B-tree structure modifications).
- **The index is always consistent with the data** after recovery, by
  construction.

**Trade-offs:** (1) memory grows with the data, and (2) startup pays a full scan
per table. For the data sizes this project targets these are fine, and the win
in simplicity and correctness is large. A production engine would persist index
pages through the buffer pool and log structure modifications.

## B+ tree with real rebalancing

We implemented the full B+ tree — split on insert, and borrow/merge on delete —
rather than the common shortcut of "lazy delete" (remove the key, never
rebalance). It is more code, but it is the honest data structure and it
demonstrates understanding of the hard part (deletion). We gained confidence in
it by testing against a `std::map` oracle over 4,000 randomised operations
(`tests/test_btree.cpp`).

## Recovery: "repeat history" ARIES, simplified

We follow ARIES' shape — analysis, redo, undo — with two deliberate
simplifications:

- **Redo replays the whole log** (not just from a checkpoint). Simpler to reason
  about; the cost is redoing more on restart. The page-LSN guard makes redo
  idempotent so it is always safe.
- **No compensation log records (CLRs).** On recovery we redo everything, then
  undo losers. Because undo is idempotent (undo-insert = tombstone, undo-delete =
  re-insert the logged before-image), we do not need CLRs for correctness in our
  single-pass model. A real system writes CLRs so that a crash *during* recovery
  does not redo work; we do not handle a crash mid-recovery.

This is enough to satisfy the core guarantee: committed transactions survive a
crash and uncommitted ones are rolled back (`tests/test_recovery.cpp`,
`tests/test_engine.cpp::crash_recovery_preserves_committed`).

## Concurrency control: strict 2PL + deadlock detection

We chose locking (strict two-phase locking) over multiversion concurrency
control because it directly demonstrates the lab's required concepts — lock
acquisition, lock conflicts, and deadlocks — and is simpler to reason about.
Strict 2PL (release all locks only at commit/abort) gives serializability *and*
recoverability (no cascading aborts).

For deadlocks we use **detection** (a wait-for graph with cycle detection) rather
than prevention (e.g. wait-die). Detection lets transactions proceed optimistically
and only intervenes when a real cycle forms; we abort the transaction that closes
the cycle.

**Trade-offs:** row-level locking with one global latch on the lock table is
simple but not built for high concurrency; and locking gives lower read
concurrency than MVCC. Both are acceptable for a teaching engine and keep the
code explainable.

## Types: just INT and TEXT, no NULLs

Two column types and no NULLs keep tuple serialisation trivial and let us focus
on the engine internals rather than a type system. Adding more types is a
localised change in `record/`.

## Optimizer: simple but real cost model

The optimizer uses textbook selectivity estimates (equality on a unique column ≈
1 row, equality on a non-unique column ≈ 10%, a range ≈ 33%) and exact row counts
(the primary index size). It makes two real decisions — index vs table scan, and
join order/algorithm — which are exactly the ones the brief asks for. The
index-vs-scan choice is genuinely cost-based: a full scan costs ≈ N while an
index scan costs ≈ selectivity × N × 4 (a random-access penalty per matched RID),
and the index is used only when it is cheaper. So a point lookup on a large table
uses the index, but a broad range (sel ≈ 1/3, where 1/3 × N × 4 > N) or a lookup
on a tiny table correctly falls back to a `SeqScan`. We did not implement
histograms (which would let a *selective* range use the index) or
dynamic-programming join enumeration; join ordering is a greedy "smallest
relation first" heuristic. `EXPLAIN` exposes the costs and choices so they can be
inspected and discussed.

## Catalog persistence: a plain text file

The catalog is a small human-readable text file. During the viva you can open it
and see exactly what the database knows. A binary format would be marginally
faster to load and is unnecessary at this scale.
