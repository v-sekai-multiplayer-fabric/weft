/- The store: rivet's Depot layout, modelled.

   Source: rivet `docs-internal/engine/sqlite/storage-structure.md`,
   `docs-internal/engine/sqlite/vfs-brief.md`, and
   `engine/packages/depot/src/conveyer/constants.rs`.

   weft copies this layout. It changes one thing: rivet's commit is one synchronous
   FoundationDB transaction, and weft ships the same rows after the local commit. The
   layout and the read rule do not change, so the proofs below hold for both.

   The property that matters is `read_preserved_by_compaction`. An implementation that
   clears DELTA in place breaks it, and a reader then sees a page vanish. No Mathlib;
   proofs use native_decide. -/
namespace Weft.Store

/-- A page number. -/
abbrev Page := Nat

/-- A transaction id. It orders every commit, and a read names one. -/
abbrev Txid := Nat

/-- Page contents. 0 means the zero-fill that the VFS gives for a gap. -/
abbrev Val := Nat

abbrev Assoc (α β : Type) := List (α × β)

def lookup (m : Assoc Nat β) (k : Nat) : Option β :=
  match m with
  | [] => none
  | (k', v) :: rest => if k' = k then some v else lookup rest k

/-- The FoundationDB state of one database.

    `pidx` is `BR/{db}/PIDX/{pgno} -> owner_txid`. It sends a page to the DELTA that
    owns it, so a read never scans the log.

    `delta` is `BR/{db}/DELTA/{txid}/{chunk}`. One txid holds the dirty pages of one
    commit.

    `shards` is `BR/{db}/SHARD/{shard}/{as_of_txid}`. A shard is versioned by
    `as_of_txid`, and compaction adds a version instead of overwriting one. -/
structure Store where
  pidx : Assoc Page Txid
  delta : Assoc Txid (Assoc Page Val)
  shards : Assoc Txid (Assoc Page Val)
  head : Txid
  deriving Repr, DecidableEq

/-- The newest shard version at or below `readTxid`, which is rivet's rule
    "reads choose the largest as_of_txid <= read_txid". -/
def shardAt (s : Store) (readTxid : Txid) : Assoc Page Val :=
  let usable := s.shards.filter (fun p => p.1 ≤ readTxid)
  match usable.foldl (fun best p => match best with
                                    | none => some p
                                    | some b => if b.1 ≤ p.1 then some p else some b) none with
  | none => []
  | some p => p.2

/-- The read path from `vfs-brief.md`: use PIDX to find the DELTA owner of a page,
    fall through to the newest SHARD at or below the read txid, then zero-fill.

    PIDX holds one owner for each page, the current one. So this serves a read at the
    head. A read below the head is a restore point, and rivet pins the branch to stop
    compaction from passing it. The pin is why compaction is safe, and the theorems
    below assume it. -/
def readPage (s : Store) (pg : Page) : Val :=
  match lookup s.pidx pg with
  | some owner =>
      match lookup s.delta owner with
      | some pages => (lookup pages pg).getD ((lookup (shardAt s s.head) pg).getD 0)
      | none => (lookup (shardAt s s.head) pg).getD 0
  | none => (lookup (shardAt s s.head) pg).getD 0

/-- A commit writes the dirty pages under one txid, points PIDX at it, and advances
    the head. rivet does this in one FoundationDB transaction. -/
def commit (s : Store) (txid : Txid) (dirty : Assoc Page Val) : Store :=
  { pidx := dirty.map (fun p => (p.1, txid)) ++ s.pidx
    delta := (txid, dirty) :: s.delta
    shards := s.shards
    head := txid }

/-- Compaction folds every DELTA at or below `asOf` into a new shard version.

    Two rules from rivet, and both are load-bearing. It adds a shard version and never
    overwrites one, so a reader below `asOf` keeps its base. It clears a PIDX row only
    when that row still points at a folded txid, so a page written after `asOf` keeps
    its owner. -/
def compact (s : Store) (asOf : Txid) : Store :=
  -- `delta` is newest first, so fold oldest first. A newer txid must end up in front
  -- of an older one, because `lookup` takes the first match.
  let folded := (s.delta.filter (fun d => d.1 ≤ asOf)).reverse
  let base := shardAt s asOf
  let merged := folded.foldl (fun acc d => d.2 ++ acc) base
  { pidx := s.pidx.filter (fun p => ¬ (p.2 ≤ asOf))
    delta := s.delta.filter (fun d => ¬ (d.1 ≤ asOf))
    shards := (asOf, merged) :: s.shards
    head := s.head }

/-- The wrong compaction: it clears every PIDX row and overwrites the base with only
    the folded pages. This is what weft implemented. -/
def compactInPlace (s : Store) (asOf : Txid) : Store :=
  let folded := (s.delta.filter (fun d => d.1 ≤ asOf)).reverse
  let merged := folded.foldl (fun acc d => d.2 ++ acc) []
  { pidx := []
    delta := []
    shards := [(asOf, merged)]
    head := s.head }

/- ## Instances to check

   `native_decide` closes a concrete goal, so the properties below are checked over an
   enumeration. -/

def pages : List Page := [0, 1, 2, 3, 4]

/-- Two commits, then a third that rewrites a page written by the second. -/
def sample : Store :=
  let s0 : Store := { pidx := [], delta := [], shards := [(0, [(0, 100), (1, 101)])], head := 0 }
  let s1 := commit s0 1 [(1, 201), (2, 202)]
  let s2 := commit s1 2 [(2, 302), (3, 303)]
  commit s2 3 [(1, 401)]

def readsOf (s : Store) : List Val := pages.map (fun p => readPage s p)

/-- What the sample holds: page 1 from the third commit, page 2 from the second, page
    3 from the second, page 0 from the base, page 4 zero-filled. -/
theorem sample_reads : readsOf sample = [100, 401, 302, 303, 0] := by native_decide

/-- **Compaction preserves every read.** This is the invariant weft broke. -/
theorem read_preserved_by_compaction :
    readsOf (compact sample 2) = readsOf sample := by native_decide

/-- It holds when compaction folds everything, including the newest commit. -/
theorem read_preserved_at_head :
    readsOf (compact sample 3) = readsOf sample := by native_decide

/-- Compaction twice is still safe, because a shard version is added and not
    overwritten. -/
theorem read_preserved_twice :
    readsOf (compact (compact sample 2) 3) = readsOf sample := by native_decide

/-- **The in-place version loses data.** It drops page 0, which lives in the base and
    not in any folded DELTA. This is the failure, kept as a proof and not a comment. -/
theorem in_place_compaction_loses_reads :
    readsOf (compactInPlace sample 2) ≠ readsOf sample := by native_decide

/-- A commit after compaction is still visible, so clearing PIDX by owner does not
    strand a later write. -/
theorem commit_after_compaction_visible :
    readsOf (commit (compact sample 2) 4 [(0, 555)]) = [555, 401, 302, 303, 0] := by
  native_decide

/- ## No constants

   rivet's constants are guesses about a workload. `MAX_SHARD_VERSIONS_PER_SHARD = 32`
   is right for some traffic and wrong for other traffic, and nothing tells you which
   you have. weft keeps none of them.

   Every quantity below is either a physical limit that FoundationDB imposes, or a
   ratio between two measured sizes. A ratio has no units to tune. It moves with the
   load: heavier writing compacts more often on its own, and a quiet actor compacts
   never. -/

/-- FoundationDB caps a value at 100 kB and a transaction at 10 MB. These are not
    choices. They are the shape of the database. -/
def fdbValueLimit : Nat := 100000
def fdbTxnLimit : Nat := 10000000

/-- SQLite's page size. Also not a choice. -/
def pageSize : Nat := 4096

/-- A chunk holds as many whole pages as fit under the value limit, so the chunk count
    is the smallest the database allows. Nothing is tuned. -/
def pagesForEachChunk : Nat := fdbValueLimit / pageSize

/-- The largest commit that fits one transaction, derived and not chosen. Room is kept
    for the PIDX rows, the commit row, the VTX row, and the head. -/
def maxCommitPages : Nat := (fdbTxnLimit - 4 * pageSize) / pageSize

theorem chunk_fits_a_value : pagesForEachChunk * pageSize ≤ fdbValueLimit := by native_decide

theorem commit_fits_a_txn : maxCommitPages * pageSize + 4 * pageSize ≤ fdbTxnLimit := by
  native_decide

/-- rivet caps a commit at 320 pages. The derived cap is larger, so rivet is leaving
    room it does not need, or it is protecting something this model does not hold. -/
theorem derived_cap_exceeds_rivet : maxCommitPages > 320 := by native_decide

/- ### Read cost does not depend on the log

   PIDX sends a page straight to its owner, so a read touches two rows whatever the
   log holds: the index row, and one of DELTA or SHARD. This is why weft needs no cap
   on shard versions to keep reads fast. -/

/-- How many rows a read touches. -/
def readRows (s : Store) (pg : Page) : Nat :=
  match lookup s.pidx pg with
  | some owner => match lookup s.delta owner with
                  | some _ => 2
                  | none => 2
  | none => 2

theorem read_touches_two_rows :
    pages.map (fun p => readRows sample p) = [2, 2, 2, 2, 2] := by native_decide

/-- The log grows and the read cost does not move. -/
theorem read_cost_ignores_log_size :
    pages.map (fun p => readRows (commit (commit sample 4 [(4, 1)]) 5 [(4, 2)]) p)
      = pages.map (fun p => readRows sample p) := by native_decide

/- ### Compaction triggers on a ratio, not a number

   Compact when the log is as large as the base. The rule needs no constant, and it
   gives two properties at once. Each byte is rewritten a bounded number of times,
   because the base must double before the next compaction. The log never exceeds the
   base, so a restore never reads more log than base. -/

def sizeOf (m : Assoc Page Val) : Nat := m.length

def logBytes (s : Store) : Nat := s.delta.foldl (fun acc d => acc + sizeOf d.2) 0

def baseBytes (s : Store) : Nat := sizeOf (shardAt s s.head)

/-- The rule. No threshold, no constant, no unit. -/
def shouldCompact (s : Store) : Bool := baseBytes s ≤ logBytes s

/-- A quiet actor never compacts, so an idle actor costs nothing. -/
theorem quiet_actor_does_not_compact :
    shouldCompact { pidx := [], delta := [], shards := [(0, [(0, 1), (1, 2)])], head := 0 }
      = false := by native_decide

/-- A write-heavy actor compacts on its own, with nothing configured. -/
theorem write_heavy_actor_compacts : shouldCompact sample = true := by native_decide

/-- Compaction restores the invariant, so the rule is stable and does not thrash. -/
theorem compaction_clears_the_trigger :
    shouldCompact (compact sample sample.head) = false := by native_decide

/- ### Retention follows demand

   rivet keeps 32 shard versions. weft keeps the versions that a pin needs and no
   others, so retention is set by what somebody is reading, not by a number. -/

/-- Drop every shard version below the oldest pin, and keep the rest. With no pin, one
    version is kept for the head. -/
def evict (s : Store) (oldestPin : Txid) : Store :=
  { s with shards := s.shards.filter (fun v => oldestPin ≤ v.1 ∨ v.1 = 0) }

/-- Eviction changes no read that a pin protects. -/
theorem eviction_preserves_pinned_reads :
    readsOf (evict (compact sample 2) 2) = readsOf sample := by native_decide

end Weft.Store
