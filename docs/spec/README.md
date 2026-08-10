# Formal specs

This directory holds the formal specification of an algorithm. A spec is a Lean4 file.
It is the source of truth for the algorithm. The Elixir code is a port of it.

## Why a spec comes first

An algorithm on the hot path must be correct. A test shows correctness on the cases you
thought of. A proof shows correctness on every case. So weft writes the algorithm in
Lean4 first, proves the properties that matter, then ports it to Elixir.

## The rules

- Formalize the algorithm in Lean4 first. Then port it to Elixir.
- Proofs use `native_decide`. Do not use Mathlib.
- Every Elixir test mirrors a proof. A proof with no matching test is not done.

## How to read a spec

Start at the theorems. Each one names a property that must hold. The definitions above a
theorem are the algorithm it holds for.

## The specs

| Spec | Proves | Elixir port | Test |
| --- | --- | --- | --- |
| `Dither.lean` | Floyd-Steinberg error diffusion conserves the quantization error, and the kernel weights sum to the denominator | `Weft.DataPlane.Dither` | `test/weft/data_plane/dither_test.exs` |

The Floyd-Steinberg algorithm comes from
[paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize). The scope in
`Weft.DataPlane` uses it to color and dither the braille panels. See `docs/reference/tasks.md`.
