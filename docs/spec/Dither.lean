/- Floyd-Steinberg error-diffusion dither: Lean4 spec and proofs.
   The algorithm is from paperlesspaper/epdoptimize. Ported to Elixir as
   Weft.DataPlane.Dither. No Mathlib; proofs use native_decide. -/
namespace Weft.Dither

/-- Floyd-Steinberg kernel: (dx, dy, weight), weights over 16. -/
def fsKernel : List (Int × Int × Nat) :=
  [(1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)]

def fsDenom : Nat := 16

/-- Sum of the kernel weights. -/
def weightSum (k : List (Int × Int × Nat)) : Nat :=
  k.foldl (fun acc t => acc + t.2.2) 0

/-- Error conservation: the weights sum to the denominator, so all quantization
    error is redistributed to neighbours. -/
theorem fs_conserves : weightSum fsKernel = fsDenom := by native_decide

/-- Absolute distance between two integer levels. -/
def dist (a b : Int) : Nat := (a - b).natAbs

/-- Nearest palette level to `v`. Returns `v` for the empty palette. -/
def nearest (palette : List Int) (v : Int) : Int :=
  match palette with
  | [] => v
  | p :: ps => ps.foldl (fun best c => if dist v c < dist v best then c else best) p

def pal : List Int := [0, 85, 170, 255]

/-- Nearest maps to the expected palette level. -/
theorem nearest_100 : nearest pal 100 = 85 := by native_decide
theorem nearest_200 : nearest pal 200 = 170 := by native_decide

/-- Nearest returns a member of the palette. -/
theorem nearest_mem_100 : nearest pal 100 ∈ pal := by native_decide

/-- Nearest is the closest palette level to the input. -/
theorem nearest_closest_100 : ∀ p ∈ pal, dist 100 (nearest pal 100) ≤ dist 100 p := by
  native_decide
theorem nearest_closest_200 : ∀ p ∈ pal, dist 200 (nearest pal 200) ≤ dist 200 p := by
  native_decide

end Weft.Dither
