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

/- ## Tunable biases

   rivet's constants are the knobs, and each one trades read cost against write cost.
   weft keeps the knobs and changes the values, because a session game writes far more
   than it reads. See `../essays/topology.md`. -/

/-- `MAX_SHARD_VERSIONS_PER_SHARD`. It caps how many shard versions a read walks, so
    it caps read amplification. A higher value delays compaction and lowers write
    amplification. -/
def maxShardVersions : Nat := 32

/-- `MAX_COMMIT_DIRTY_PAGES`. rivet caps a commit at 320 pages so one commit always
    fits in one compaction batch. -/
def maxCommitDirtyPages : Nat := 320

/-- `CMP_FDB_BATCH_MAX_KEYS`. -/
def batchMaxKeys : Nat := 500

/-- Keys that one commit writes: one DELTA chunk and one PIDX row for each page, plus
    a commit row, a VTX row, and the head. -/
def keysForCommit (dirtyPages : Nat) : Nat := 2 * dirtyPages + 3

/-- rivet's own reasoning, checked: 320 dirty pages must fit under the 500 key cap
    with room for the per-commit rows.

    It does not fit. `keysForCommit 320 = 643`, which is above 500. So the cap holds
    only when a DELTA chunk carries more than one page, which is what the `chunk`
    field in `DELTA/{txid}/{chunk}` is for. The check is kept because it names the
    assumption instead of hiding it. -/
theorem commit_keys_need_chunking : keysForCommit maxCommitDirtyPages > batchMaxKeys := by
  native_decide

/-- With pages packed into chunks, a commit fits. 320 pages of 4 kB is 1.25 MB, and a
    chunk holds many pages, so the key count is the chunk count plus the rows. -/
def keysForChunkedCommit (dirtyPages pagesForEachChunk : Nat) : Nat :=
  dirtyPages / pagesForEachChunk + dirtyPages + 3

theorem chunked_commit_fits :
    keysForChunkedCommit maxCommitDirtyPages 16 ≤ batchMaxKeys := by native_decide

end Weft.Store
