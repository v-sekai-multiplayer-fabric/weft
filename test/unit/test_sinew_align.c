/*
 * Mirrors sinew-mocap/solve's core/spec/AlignTest.lean exactly: a known
 * rotation (quaternion (0.5,0.5,0.5,0.5), a 120-degree turn) and a
 * spread of source vectors; align over (R*b, b) pairs must recover R,
 * checked at N=5 (full multi-pair path), N=2 (covariance+ns30 path),
 * and N=1 (rodrigues path) -- the same three cases the Lean test
 * checks, with the same 1e-4 tolerance on the N=5 case.
 */

#include "sinew_align.h"
#include <math.h>
#include <stdio.h>

/* Math.lean's quatToMat, transcribed the same way sinew_align.c
 * transcribes Align.lean -- row-major 3x3 output. */
static void quat_to_mat(double qw, double qx, double qy, double qz, double out_r[9])
{
    double n = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (n < 1e-9) {
        double id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        for (int i = 0; i < 9; i++) out_r[i] = id[i];
        return;
    }
    double w = qw / n, x = qx / n, y = qy / n, z = qz / n;
    out_r[0] = 1 - 2 * (y * y + z * z); out_r[1] = 2 * (x * y - z * w);     out_r[2] = 2 * (x * z + y * w);
    out_r[3] = 2 * (x * y + z * w);     out_r[4] = 1 - 2 * (x * x + z * z); out_r[5] = 2 * (y * z - x * w);
    out_r[6] = 2 * (x * z - y * w);     out_r[7] = 2 * (y * z + x * w);     out_r[8] = 1 - 2 * (x * x + y * y);
}

static void mat_apply(const double r[9], const double v[3], double out[3])
{
    out[0] = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
    out[1] = r[3] * v[0] + r[4] * v[1] + r[5] * v[2];
    out[2] = r[6] * v[0] + r[7] * v[1] + r[8] * v[2];
}

static double max_recovery_error(const double r_true[9], const double r_rec[9],
                                  const double *srcs, size_t count)
{
    double e = 0.0;
    for (size_t i = 0; i < count; i++) {
        double true_v[3], rec_v[3];
        mat_apply(r_true, &srcs[i * 3], true_v);
        mat_apply(r_rec, &srcs[i * 3], rec_v);
        double dx = true_v[0] - rec_v[0], dy = true_v[1] - rec_v[1], dz = true_v[2] - rec_v[2];
        double d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d > e) e = d;
    }
    return e;
}

int main(void)
{
    double r[9];
    quat_to_mat(0.5, 0.5, 0.5, 0.5, r);

    /* srcs : #[<1,0,0>, <0,1,0>, <0,0,1>, <1,1,0>, <0.3,-0.7,0.5>] */
    double srcs[5 * 3] = {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        1, 1, 0,
        0.3, -0.7, 0.5,
    };
    double targets[5 * 3];
    for (int i = 0; i < 5; i++) {
        mat_apply(r, &srcs[i * 3], &targets[i * 3]);
    }

    double r_rec5[9];
    sinew_align(targets, srcs, 5, r_rec5);
    double e5 = max_recovery_error(r, r_rec5, srcs, 5);
    printf("align (N=5): max recovery error = %.8f\n", e5);

    double r_rec2[9];
    sinew_align(targets, srcs, 2, r_rec2);
    double e2 = max_recovery_error(r, r_rec2, srcs, 2);
    printf("align (N=2): max recovery error = %.8f\n", e2);

    double one_src[3] = { 0, 1, 0 };
    double one_target[3];
    mat_apply(r, one_src, one_target);
    double r_rec1[9];
    sinew_align(one_target, one_src, 1, r_rec1);
    double e1 = max_recovery_error(r, r_rec1, one_src, 1);
    printf("rodrigues (N=1): error = %.8f\n", e1);

    /* AlignTest.lean's own pass criterion: e (the N=5 case) < 1e-4. */
    if (e5 < 1e-4 && e2 < 1e-4 && e1 < 1e-4) {
        printf("OK\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
