#pragma once

#include <stddef.h>

/*
 * Direct C transcription of sinew-mocap/solve's core/spec/Sinew/Align.lean
 * -- task #16, corrected: per direct instruction, sinew-mocap/solve's own
 * algorithm is Lean4-specified first (Lean -> Slang codegen is its actual
 * pipeline, core/spec/Sinew/SlangCodegen/), the same pattern as
 * lean-entity-packet and lean-rebac-core. The QP/mink framing this task
 * had earlier was wrong for this reason -- sinew-mocap/solve does NOT
 * use mink or QP at all. It has its own proven, simpler algorithm:
 * Kabsch-style rotation fitting (Rodrigues for one vector pair, a
 * Newton-Schulz-orthogonalized covariance for two or more, falling back
 * to a Jacobi-SVD Kabsch solve when the fast path produces an invalid
 * rotation). That algorithm -- not mink's QP machinery -- is what this
 * file ports, function-for-function against Align.lean:
 *
 *   det9, mul9, transpose9   -- row-major 3x3 helpers
 *   rodrigues                -- single-pair shortest-arc rotation
 *   ns30                     -- 30-iteration Newton-Schulz orthogonalization
 *   regularize                -- rank-deficient covariance bump
 *   valid9                   -- is this a proper near-orthonormal rotation?
 *   jacobi9                  -- cyclic Jacobi eigendecomposition (3x3 symmetric)
 *   kabsch                   -- SVD-based fallback rotation fit
 *   finishAlign, align       -- the top-level dispatcher AlignTest.lean tests
 *
 * Verified in test/unit/test_sinew_align.c against the exact known
 * rotation (quaternion (0.5,0.5,0.5,0.5), a 120-degree turn)
 * AlignTest.lean itself checks recovery of, at N=1 (rodrigues path),
 * N=2 (covariance+ns30 path), and N=5 (the full multi-pair path) -- the
 * same test oracle the Lean source uses, not an invented one.
 */

/* Row-major 3x3 matrices/vectors as flat arrays: m[0..2]=row0, m[3..5]=row1,
 * m[6..8]=row2, matching Align.lean's Array Float convention exactly. */

double sinew_det9(const double m[9]);
void sinew_mul9(const double a[9], const double b[9], double out[9]);
void sinew_transpose9(const double m[9], double out[9]);

/* Shortest-arc rotation taking b onto a (single vector pair), each a
 * 3-element {x,y,z} array. */
void sinew_rodrigues(const double a[3], const double b[3], double out_r[9]);

void sinew_ns30(const double h[9], double out_r[9]);
void sinew_regularize(const double h[9], double out_h[9]);
int sinew_valid9(const double r[9]); /* 1 = proper near-orthonormal rotation */

/* Cyclic Jacobi eigendecomposition of a symmetric 3x3; out_eigenvalues[3],
 * out_v[9] (eigenvectors as columns, row-major storage). */
void sinew_jacobi9(const double a_in[9], double out_eigenvalues[3], double out_v[9]);

void sinew_kabsch(const double h[9], double out_r[9]);

/* finishAlign: h is the accumulated covariance (sum of outer products);
 * n is the pair count; a0/b0/a1/b1 are the first two pairs (only a0/b0
 * used when n==1). */
void sinew_finish_align(const double h[9], int n,
                         const double a0[3], const double a1[3],
                         const double b0[3], const double b1[3],
                         double out_r[9]);

/* align: top-level entry point. targets/sources are `count` 3-vectors
 * each (targets[i] ~= R * sources[i]); writes the recovered rotation
 * into out_r. Matches Align.lean's `align (pairs : Array (V3 x V3))`
 * with pairs[i] = (targets[i], sources[i]). */
void sinew_align(const double *targets_xyz, const double *sources_xyz,
                  size_t count, double out_r[9]);
