/- Read-ahead for a page VFS over a network database.

   Source: rivet `engine/packages/depot-client/src/vfs.rs`, `PrefetchTracker::record`,
   and `engine/packages/depot-client/src/sqlite_page.rs`.

   A page miss is a network round trip, so a VFS over FoundationDB lives or dies on
   read-ahead. A fixed depth is a guess: too deep and a point read pays for pages it
   never wants, too shallow and a table scan pays a round trip for each page.

   rivet guesses nothing. It scores the access pattern, escalates when the score says
   the reader is scanning, and decays fast when it is not. The proofs below are the
   three behaviours that matter. No Mathlib; proofs use native_decide. -/
namespace Weft.Prefetch

/-- A page number. -/
abbrev Page := Nat

/-- Two pages count as consecutive within this gap. A B-tree scan skips pages, so an
    exact successor test would miss a real scan. -/
def gapTolerance : Nat := 8

/-- Escalate read-ahead at this score. -/
def escalateAt : Int := 6

/-- Use the full depth at this score. -/
def fullDepthAt : Int := escalateAt + 4

/-- The score never passes this, so one long scan cannot buy unlimited credit. -/
def scoreMax : Int := 12

/-- The tracker of one page class. -/
structure Tracker where
  score : Int := 0
  lastPage : Option Page := none
  scanTip : Option Page := none
  deriving Repr, DecidableEq

def forwardFrom (base : Option Page) (pg : Page) : Bool :=
  match base with
  | none => false
  | some b => b < pg && pg - b ≤ gapTolerance

/-- One page access.

    A forward page adds 2. A page that is neither forward nor a repeat subtracts: 1
    while a scan is established, and 4 otherwise. The rates are the design. Up 2 and
    down 4 means a scan must be twice as consistent as the noise to hold its credit,
    and the softer decay during a scan tolerates the interleaving that a real B-tree
    walk produces. -/
def record (t : Tracker) (pg : Page) : Tracker :=
  let forward := forwardFrom t.lastPage pg || forwardFrom t.scanTip pg
  let repeated := t.lastPage = some pg
  if forward then
    { score := min (t.score + 2) scoreMax, lastPage := some pg, scanTip := some pg }
  else if repeated then
    { t with lastPage := some pg }
  else if escalateAt ≤ t.score && t.scanTip ≠ none then
    { score := max (t.score - 1) 0, lastPage := some pg, scanTip := t.scanTip }
  else
    { score := max (t.score - 4) 0, lastPage := some pg, scanTip := some pg }

def runOn (t : Tracker) (pages : List Page) : Tracker := pages.foldl record t

def escalates (t : Tracker) : Bool := escalateAt ≤ t.score
def fullDepth (t : Tracker) : Bool := fullDepthAt ≤ t.score

def fresh : Tracker := {}

/- ## The three behaviours -/

/-- A table scan escalates, and it does so quickly. Three forward pages reach the
    threshold, so the cost of detection is three round trips and not a tuned number. -/
theorem scan_escalates : escalates (runOn fresh [10, 11, 12, 13]) = true := by native_decide

/-- A long scan reaches the full depth and stops there, because the score is capped. -/
theorem long_scan_reaches_full_depth :
    fullDepth (runOn fresh [1, 2, 3, 4, 5, 6, 7, 8]) = true := by native_decide

theorem score_is_capped : (runOn fresh (List.range 40)).score = scoreMax := by native_decide

/-- Random access never escalates, so a point-read workload pays for no read-ahead.
    This is the property a fixed depth cannot have. -/
theorem random_access_never_escalates :
    escalates (runOn fresh [500, 3, 981, 40, 77, 612, 8, 250]) = false := by native_decide

/-- A scan that ends decays back, so read-ahead stops when the reader stops scanning.

    It takes seven random pages, not one. That hysteresis is deliberate: a scan that is
    briefly interrupted keeps its read-ahead, and only a sustained change of pattern
    turns it off. -/
theorem scan_then_random_decays :
    escalates (runOn (runOn fresh [1, 2, 3, 4, 5, 6, 7, 8]) [900, 40, 700, 20, 850, 33, 640])
      = false := by native_decide

/-- Six random pages are not enough, which is the hysteresis stated as a bound. -/
theorem brief_interruption_keeps_read_ahead :
    escalates (runOn (runOn fresh [1, 2, 3, 4, 5, 6, 7, 8]) [900, 40, 700, 20, 850, 33])
      = true := by native_decide

/-- A gap inside the tolerance is still a scan. A B-tree leaf walk skips pages, and an
    exact successor test would miss it. -/
theorem gapped_scan_still_escalates :
    escalates (runOn fresh [10, 14, 20, 26]) = true := by native_decide

/-- A gap outside the tolerance is not a scan. -/
theorem wide_stride_does_not_escalate :
    escalates (runOn fresh [10, 30, 50, 70]) = false := by native_decide

/- ## Why one tracker is not enough

   A scan over rows with overflowing payloads reads a leaf page, then an overflow page,
   then the next leaf, and so on. The two classes live in different parts of the file,
   so one tracker sees large alternating jumps and never scores a scan. rivet keeps one
   tracker for each class. -/

/-- The interleaved access that a real scan produces: leaves 10..13 with overflow pages
    900.. between them. -/
def interleaved : List Page := [10, 900, 11, 901, 12, 902, 13, 903]

/-- **One tracker fails.** The alternation looks like random access, so a scan that is
    plainly a scan escalates nothing. -/
theorem one_tracker_misses_the_scan :
    escalates (runOn fresh interleaved) = false := by native_decide

/-- Split by class, the same access escalates on both streams. This is the whole reason
    `sqlite_page.classify` exists. -/
def leaves : List Page := [10, 11, 12, 13]
def overflows : List Page := [900, 901, 902, 903]

theorem split_trackers_find_the_scan :
    escalates (runOn fresh leaves) = true ∧ escalates (runOn fresh overflows) = true := by
  native_decide

end Weft.Prefetch
