/- The Gyre: the room graph and the objective, modelled.

   Source: `zone-guest-gyre`, `docs/0003-the-gyre-mud-domain-and-mode-selector.md`, and
   the guest it describes, `mud/guest/mud_guest.cpp` in `zone-server-h2o`.

   The Gyre is a second MUD domain, not a second engine. Its smallest loop is two rooms,
   `decanting_floor` and `splicers_den`, with one exit each way, no items, and no NPCs.
   The loop is look, go, look, and the objective completes when a player has seen both
   rooms.

   The properties below are the ones a port must not break. The one that matters is
   `objective_needs_both`. A player who walks east and back has moved twice and seen two
   rooms, and a player who never leaves has seen one. An implementation that counts moves
   rather than rooms passes the first case and fails the second.

   No Mathlib. Proofs use native_decide. -/
namespace Weft.Gyre

/-- A room of the smallest Gyre loop. -/
inductive Room where
  | decantingFloor
  | splicersDen
  deriving Repr, DecidableEq

/-- The directions the loop uses. -/
inductive Dir where
  | east
  | west
  deriving Repr, DecidableEq

open Room Dir

/-- Every room, for the enumerations below. -/
def rooms : List Room := [decantingFloor, splicersDen]

def dirs : List Dir := [east, west]

/-- Where a player starts. -/
def start : Room := decantingFloor

/-- The exits. A direction that leaves the map gives none, and a move that gives none
    leaves the player where they were. -/
def exit (r : Room) (d : Dir) : Option Room :=
  match r, d with
  | decantingFloor, east => some splicersDen
  | splicersDen, west => some decantingFloor
  | decantingFloor, west => none
  | splicersDen, east => none

/-- A move is total. A player who walks into a wall stays in the room. -/
def move (r : Room) (d : Dir) : Room :=
  match exit r d with
  | some r' => r'
  | none => r

/-- The rooms a player has seen. The head is the newest, and a room is recorded once. -/
abbrev Seen := List Room

def see (seen : Seen) (r : Room) : Seen :=
  if seen.contains r then seen else r :: seen

/-- Walk a list of directions from a room, and record every room on the way. -/
def walk (r : Room) (seen : Seen) : List Dir → Room × Seen
  | [] => (r, seen)
  | d :: rest =>
      let r' := move r d
      walk r' (see seen r') rest

/-- The objective of the Gyre loop: see every room. -/
def objectiveComplete (seen : Seen) : Bool :=
  rooms.all (fun r => seen.contains r)

/- ## The properties a port must hold -/

/-- The map is the one the guest describes: one exit each way, and nothing else. -/
theorem exits_are_one_each_way :
    (exit decantingFloor east = some splicersDen)
    ∧ (exit splicersDen west = some decantingFloor)
    ∧ (exit decantingFloor west = none)
    ∧ (exit splicersDen east = none) := by native_decide

/-- A move never leaves the map, whatever a player types. -/
theorem move_is_total :
    (rooms.flatMap (fun r => dirs.map (fun d => move r d))).all (fun r => rooms.contains r)
      = true := by native_decide

/-- A wall does not move a player. -/
theorem wall_does_not_move :
    move decantingFloor west = decantingFloor ∧ move splicersDen east = splicersDen := by
  native_decide

/-- East and then west returns a player to the start. -/
theorem round_trip : (walk start (see [] start) [east, west]).1 = start := by native_decide

/-- Every room is reachable from the start, and one move is enough. -/
theorem every_room_reachable :
    rooms.all (fun r => r = start ∨ (dirs.any (fun d => move start d = r))) = true := by
  native_decide

/-- **The objective needs both rooms, and not two moves.** A player who walks east and
    back has moved twice and finished. A player who walks into the wall twice has also
    moved twice and has not. -/
theorem objective_needs_both :
    objectiveComplete (walk start (see [] start) [east, west]).2 = true
    ∧ objectiveComplete (walk start (see [] start) [west, west]).2 = false := by
  native_decide

/-- Standing still does not finish the objective. -/
theorem start_alone_is_not_enough :
    objectiveComplete (see [] start) = false := by native_decide

/-- Seeing a room again does not change what a player has seen, so the objective cannot
    be finished by walking the same room twice. -/
theorem see_is_idempotent :
    see (see [] start) start = see [] start := by native_decide

/-- The objective, once complete, stays complete. A player may keep walking. -/
theorem objective_is_stable :
    objectiveComplete (walk start (see [] start) [east, west, east, west]).2 = true := by
  native_decide

end Weft.Gyre
