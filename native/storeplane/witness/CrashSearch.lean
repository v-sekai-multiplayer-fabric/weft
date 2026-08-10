import PlausibleWitnessDag

/-! # A witness search over crash points

`prove_crash` asks one question: does a crash at this point leave a torn database? A
sweep over a few wall clock delays answers it for the delays somebody picked, and says
nothing about the rest. That is the weak half of the evidence in weft issue 42.

This is the search that replaces the sweep. A crash point is a commit index, so the
space is finite, ordered, and repeatable. The search either finds a crash point that
tears the database, or it covers every crash point and finds none.

The ladder comes from `plausible-witness-dag`. It escalates only when it must:

* plausible samples the candidate window at each rung, so a broken store plane fails
  fast, wherever the fault sits in the range;
* the deterministic readback then walks the crash points in order, and it reports
  whether it covered the whole range or ran out of budget.

The difference between those two negatives is the point. `budgetHit` means the search
did not look everywhere. `provablyNone` means it did. A sweep can only ever report the
first, and issue 43 is about a soak that reported a count and discarded which one it was.

The oracle runs a real process against a real FoundationDB, so it is `IO` behind a pure
face. Every probe is memoized, which bounds the whole search at one process for each
crash point however many times plausible samples it.
-/

open PlausibleWitnessDag

namespace Weft.CrashSearch

/-- What to crash, and how far the crash points reach. -/
structure Config where
  prog   : String := "./prove_crash"
  db     : String := "crash_search.db"
  rows   : Nat := 50
  /-- Crash points run from 1 to this. `prove_crash` reports when one is out of range. -/
  points : Nat := 120
  deriving Inhabited

/-- The commit sizes the search covers.

A crash point alone is one dimension, and it is not the only way the store plane can
break. The size of a commit picks the path the code takes: a commit of one page and a
commit of many pages take different branches, and the pages of a large commit are
written before the head moves. So the search runs over the product of the two, and a
candidate names both.

The sizes are not a tuning knob. They run from the smallest commit a database can make
to one that dirties enough pages to need several index rows, so each one lands on a
different branch of the commit path. -/
def rowChoices : Array Nat := #[1, 8, 64, 400, 2000]

initialize configRef : IO.Ref Config ← IO.mkRef {}

/-- One answer for each crash point already probed. A probe costs a process, and
plausible samples the same candidate many times, so this is what keeps the search
affordable. -/
initialize cacheRef : IO.Ref (List (Nat × Bool)) ← IO.mkRef []

/-- What each witness printed, so the search can name the failure and not only count it. -/
initialize reasonRef : IO.Ref (List (Nat × String)) ← IO.mkRef []

/-- How many processes the search actually ran. -/
initialize probesRef : IO.Ref Nat ← IO.mkRef 0

/-- The highest crash point that was inside the writer's commit sequence. -/
initialize reachedRef : IO.Ref Nat ← IO.mkRef 0

/-- Split a candidate index into a commit size and a crash point. -/
def decode (cfg : Config) (i : Nat) : Nat × Nat :=
  let sizes := rowChoices.size
  let rows := (rowChoices[i % sizes]?).getD 1
  let point := (i / sizes) % cfg.points + 1
  (rows, point)

def describe (cfg : Config) (i : Nat) : String :=
  let (rows, point) := decode cfg i
  s!"a crash before commit {point} of a {rows} row writer"

/-- Run `prove_crash` for one candidate.

`prove_crash` exits 0 when the database it left is whole, 1 when it is torn or fails its
own integrity check, and 2 when the crash point is past the end of the commit sequence.
Only the middle one is a witness. -/
def probe (i : Nat) : IO Bool := do
  let cache ← cacheRef.get
  match cache.lookup i with
  | some hit => return hit
  | none =>
    let cfg ← configRef.get
    let (rows, point) := decode cfg i
    -- Each candidate gets its own database. A crash leaves a damaged database behind on
    -- purpose, so probes that shared one name would inherit the damage and the search
    -- would blame whichever candidate it happened to try next.
    let db := s!"{cfg.db}.{rows}.{point}"
    let out ← IO.Process.output
      { cmd := cfg.prog, args := #[db, toString rows, "at", toString point] }
    probesRef.modify (· + 1)

    let torn := out.exitCode == 1
    if out.exitCode != 2 then
      reachedRef.modify (max · point)
    if torn then
      reasonRef.modify ((i, out.stdout ++ out.stderr) :: ·)

    cacheRef.modify ((i, torn) :: ·)
    return torn

/-- The pure face of the oracle. The search below is a pure predicate over candidates,
and the answer comes from a process, so the effect is hidden here and nowhere else. -/
unsafe def tearsAtImpl (n : Nat) : Bool :=
  match unsafeIO (probe n) with
  | .ok torn => torn
  | .error _ => false

@[implemented_by tearsAtImpl]
opaque tearsAt (n : Nat) : Bool

/-- The size of the whole candidate space: every commit size at every crash point. -/
unsafe def spaceImpl (_ : Unit) : Nat :=
  match unsafeIO (do return (← configRef.get).points * rowChoices.size) with
  | .ok n => n
  | .error _ => 1

@[implemented_by spaceImpl]
opaque space (_ : Unit) : Nat

/-- The candidate predicate plausible samples.

A candidate index folds into the space, so plausible explores all of it however wide its
own window is. -/
def crashCandidate (_lvl : Level) (i : Nat) : Bool :=
  tearsAt (i % space ())

/-- Walk the candidate space in order and stop at the first candidate that tears. -/
def scan : Nat → Nat → Option Nat
  | _, 0 => none
  | start, budget + 1 => if tearsAt start then some start else scan (start + 1) budget

/-- The deterministic readback.

It covers crash points 1 to `steps`. When it finds nothing and `steps` did not reach the
end of the range, it says so, and the driver escalates instead of calling the range
clean. -/
def crashReadback (steps : Nat) : Readback (List String) :=
  let budget := min steps (space ())
  match scan 0 budget with
  | some i =>
      { value := [s!"candidate {i} left a torn database"]
      , found := true, witnessIdx := i, budgetHit := false }
  | none =>
      { value := [], found := false, witnessIdx := 0
      , budgetHit := budget < space () }

/-- The ladder. Each rung widens the deterministic walk, and the last rung covers the
whole crash point range so that a negative can be a real negative.

The rungs are fractions of the range rather than chosen numbers, so the ladder moves with
the range instead of being tuned to one. -/
def crashLadder (range : Nat) : Array Level := #[
  { idx := 0, walkSteps := range / 4 + 1, finBound := 256, numInst := 64  },
  { idx := 1, walkSteps := range / 2 + 1, finBound := 256, numInst := 128 },
  { idx := 2, walkSteps := range,         finBound := 256, numInst := 256 }]

def usage : String :=
  "usage: crash-search <prove_crash> [--db NAME] [--rows N] [--points N]\n\n" ++
  "Searches crash points for one that leaves a torn database.\n" ++
  "Exit: 0 no crash point tears the database, 1 one does, 2 the search ran out of budget."

def parse (args : List String) (cfg : Config) : Except String Config :=
  match args with
  | [] => .ok cfg
  | "--db" :: v :: rest => parse rest { cfg with db := v }
  | "--rows" :: v :: rest =>
      match v.toNat? with
      | some n => parse rest { cfg with rows := n }
      | none => .error s!"--rows wants a number, got {v}"
  | "--points" :: v :: rest =>
      match v.toNat? with
      | some n => parse rest { cfg with points := n }
      | none => .error s!"--points wants a number, got {v}"
  | other :: _ => .error s!"unknown argument {other}"

def main (argv : List String) : IO UInt32 := do
  match argv with
  | [] => IO.println usage; return 2
  | prog :: rest =>
    match parse rest { prog := prog } with
    | .error msg => IO.eprintln msg; return 2
    | .ok cfg =>
      configRef.set cfg
      let total := cfg.points * rowChoices.size
      IO.println s!"searching {total} candidates: crash points 1..{cfg.points} \
        at commit sizes {rowChoices.toList}"

      let (found, lvl, trace) ←
        resolve "store plane: a crash that tears the database"
          crashCandidate crashReadback (crashLadder total)

      let probes ← probesRef.get
      let reached ← reachedRef.get
      IO.println s!"resolved at L{lvl} after {probes} crashes, \
        crash points up to {reached} were in range"

      match trace.outcome with
      | .found idx =>
          IO.println s!"WITNESS: {describe cfg idx} tears the database"
          for line in found do IO.println s!"  {line}"
          let reasons ← reasonRef.get
          match reasons.lookup idx with
          | some text => IO.println "  the crash left:"; IO.println text
          | none => pure ()
          return 1
      | .provablyNone =>
          IO.println s!"none: no candidate of {total} leaves a torn database"
          return 0
      | .budgetHit =>
          IO.println "budget: the search did not cover the whole space, so this is not a pass"
          return 2

end Weft.CrashSearch

def main (args : List String) : IO UInt32 := Weft.CrashSearch.main args
