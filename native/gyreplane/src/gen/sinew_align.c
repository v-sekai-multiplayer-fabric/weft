/*
 * Direct C transcription of sinew-mocap/solve's Align.lean. See
 * sinew_align.h for provenance and scope.
 */

#include "sinew_align.h"

#include <math.h>
#include <string.h>

#define SINEW_EPS 1e-8

static double v3_dot(const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v3_cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static double v3_norm(const double a[3])
{
    return sqrt(v3_dot(a, a));
}

static void v3_scale(const double a[3], double s, double out[3])
{
    out[0] = a[0] * s;
    out[1] = a[1] * s;
    out[2] = a[2] * s;
}

double sinew_det9(const double m[9])
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

void sinew_mul9(const double a[9], const double b[9], double out[9])
{
    double c[9];
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            c[r * 3 + col] = a[r * 3] * b[col] + a[r * 3 + 1] * b[3 + col] +
                              a[r * 3 + 2] * b[6 + col];
        }
    }
    memcpy(out, c, sizeof(c));
}

void sinew_transpose9(const double m[9], double out[9])
{
    double o[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            o[i * 3 + j] = m[j * 3 + i];
        }
    }
    memcpy(out, o, sizeof(o));
}

void sinew_rodrigues(const double a[3], const double b[3], double out_r[9])
{
    double an = v3_norm(a), bn = v3_norm(b);
    double au[3], bu[3];
    v3_scale(a, 1.0 / fmax(an, SINEW_EPS), au);
    v3_scale(b, 1.0 / fmax(bn, SINEW_EPS), bu);
    double d = v3_dot(au, bu);
    if (d < -1.0) d = -1.0;
    if (d > 1.0) d = 1.0;

    if (d < -1.0 + 1e-6) {
        double w[3] = { fabs(bu[0]) > 0.6 ? 0.0 : 1.0,
                         fabs(bu[0]) > 0.6 ? 1.0 : 0.0, 0.0 };
        double x0[3];
        v3_cross(bu, w, x0);
        double x[3];
        v3_scale(x0, 1.0 / v3_norm(x0), x);
        double aa[3] = { x[0], x[1], x[2] };
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                out_r[r * 3 + c] = 2.0 * aa[r] * aa[c] - (r == c ? 1.0 : 0.0);
            }
        }
        return;
    }

    double v[3];
    v3_cross(bu, au, v);
    double k[9] = { 0.0, -v[2], v[1], v[2], 0.0, -v[0], -v[1], v[0], 0.0 };
    double kk[9];
    sinew_mul9(k, k, kk);
    double f = 1.0 / (1.0 + d);
    for (int i = 0; i < 9; i++) {
        out_r[i] = (i % 4 == 0 ? 1.0 : 0.0) + k[i] + f * kk[i];
    }
}

void sinew_ns30(const double h[9], double out_r[9])
{
    double mrs = 0.0;
    for (int r = 0; r < 3; r++) {
        double row_abs_sum = fabs(h[r * 3]) + fabs(h[r * 3 + 1]) + fabs(h[r * 3 + 2]);
        mrs = fmax(mrs, row_abs_sum);
    }
    double r[9];
    for (int i = 0; i < 9; i++) {
        r[i] = h[i] / (mrs + SINEW_EPS);
    }
    for (int iter = 0; iter < 30; iter++) {
        double rtr[9];
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                rtr[a * 3 + b] = r[a] * r[b] + r[3 + a] * r[3 + b] + r[6 + a] * r[6 + b];
            }
        }
        double term[9];
        for (int k = 0; k < 9; k++) {
            term[k] = (k % 4 == 0 ? 3.0 : 0.0) - rtr[k];
        }
        double rn[9];
        sinew_mul9(r, term, rn);
        for (int k = 0; k < 9; k++) {
            r[k] = rn[k] * 0.5;
        }
    }
    if (sinew_det9(r) < 0) {
        r[2] = -r[2];
        r[5] = -r[5];
        r[8] = -r[8];
    }
    memcpy(out_r, r, sizeof(r));
}

void sinew_regularize(const double h[9], double out_h[9])
{
    double scale = 0.0;
    for (int r = 0; r < 3; r++) {
        double row_abs_sum = fabs(h[r * 3]) + fabs(h[r * 3 + 1]) + fabs(h[r * 3 + 2]);
        scale = fmax(scale, row_abs_sum);
    }
    if (scale < SINEW_EPS) scale = SINEW_EPS;
    double vol = fabs(sinew_det9(h)) / (scale * scale * scale);
    double rw = fmax(0.0, fmin(1.0, (1e-6 - vol) / 1e-6));
    double add = 0.05 * rw * scale;
    memcpy(out_h, h, 9 * sizeof(double));
    out_h[0] += add;
    out_h[4] += add;
    out_h[8] += add;
}

int sinew_valid9(const double r[9])
{
    double d = sinew_det9(r);
    if (!(d > 0.0 && fabs(d - 1.0) <= 1e-2)) {
        return 0;
    }
    double e = 0.0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double v = r[i] * r[j] + r[3 + i] * r[3 + j] + r[6 + i] * r[6 + j] -
                       (i == j ? 1.0 : 0.0);
            e = fmax(e, fabs(v));
        }
    }
    return e <= 1e-2;
}

void sinew_jacobi9(const double a_in[9], double out_eigenvalues[3], double out_v[9])
{
    double a[9];
    memcpy(a, a_in, sizeof(a));
    double v[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const int pp[3] = { 0, 0, 1 };
    const int qq[3] = { 1, 2, 2 };

    for (int iter = 0; iter < 50; iter++) {
        if (fabs(a[1]) + fabs(a[2]) + fabs(a[5]) < 1e-20) {
            break;
        }
        for (int k = 0; k < 3; k++) {
            int p = pp[k], q = qq[k];
            double apq = a[p * 3 + q];
            if (fabs(apq) < 1e-20) {
                continue;
            }
            double phi = 0.5 * atan2(2.0 * apq, a[q * 3 + q] - a[p * 3 + p]);
            double c = cos(phi), s = sin(phi);
            for (int i = 0; i < 3; i++) {
                double aip = a[i * 3 + p], aiq = a[i * 3 + q];
                a[i * 3 + p] = c * aip - s * aiq;
                a[i * 3 + q] = s * aip + c * aiq;
            }
            for (int i = 0; i < 3; i++) {
                double api = a[p * 3 + i], aqi = a[q * 3 + i];
                a[p * 3 + i] = c * api - s * aqi;
                a[q * 3 + i] = s * api + c * aqi;
            }
            for (int i = 0; i < 3; i++) {
                double vip = v[i * 3 + p], viq = v[i * 3 + q];
                v[i * 3 + p] = c * vip - s * viq;
                v[i * 3 + q] = s * vip + c * viq;
            }
        }
    }
    out_eigenvalues[0] = a[0];
    out_eigenvalues[1] = a[4];
    out_eigenvalues[2] = a[8];
    memcpy(out_v, v, sizeof(v));
}

void sinew_kabsch(const double h[9], double out_r[9])
{
    double s[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            s[i * 3 + j] = h[i] * h[j] + h[3 + i] * h[3 + j] + h[6 + i] * h[6 + j];
        }
    }
    double w[3], vc[9];
    sinew_jacobi9(s, w, vc);

    int ord[3] = { 0, 1, 2 };
    for (int a = 0; a < 3; a++) {
        for (int b = a + 1; b < 3; b++) {
            if (w[ord[b]] > w[ord[a]]) {
                int t = ord[a];
                ord[a] = ord[b];
                ord[b] = t;
            }
        }
    }

    double v[9] = { 0 };
    double sig[3];
    for (int c = 0; c < 3; c++) {
        sig[c] = sqrt(fmax(w[ord[c]], 0.0));
        for (int r = 0; r < 3; r++) {
            v[r * 3 + c] = vc[r * 3 + ord[c]];
        }
    }

    double u[9] = { 0 };
    for (int c = 0; c < 3; c++) {
        double hvx = h[0] * v[c] + h[1] * v[3 + c] + h[2] * v[6 + c];
        double hvy = h[3] * v[c] + h[4] * v[3 + c] + h[5] * v[6 + c];
        double hvz = h[6] * v[c] + h[7] * v[3 + c] + h[8] * v[6 + c];
        double sc = sig[c] > 1e-12 ? 1.0 / sig[c] : 0.0;
        u[c] = hvx * sc;
        u[3 + c] = hvy * sc;
        u[6 + c] = hvz * sc;
    }
    for (int c = 0; c < 3; c++) {
        if (sig[c] <= 1e-12) {
            int a = (c + 1) % 3, b = (c + 2) % 3;
            double ua[3] = { u[a], u[3 + a], u[6 + a] };
            double ub[3] = { u[b], u[3 + b], u[6 + b] };
            double uc[3];
            v3_cross(ua, ub, uc);
            u[c] = uc[0];
            u[3 + c] = uc[1];
            u[6 + c] = uc[2];
        }
    }

    double vt[9];
    sinew_transpose9(v, vt);
    double uvt[9];
    sinew_mul9(u, vt, uvt);
    double sgn = sinew_det9(uvt) < 0 ? -1.0 : 1.0;
    double ud[9];
    memcpy(ud, u, sizeof(u));
    ud[2] *= sgn;
    ud[5] *= sgn;
    ud[8] *= sgn;
    sinew_mul9(ud, vt, out_r);
}

void sinew_finish_align(const double h_in[9], int n,
                         const double a0[3], const double a1[3],
                         const double b0[3], const double b1[3],
                         double out_r[9])
{
    if (n == 1) {
        sinew_rodrigues(a0, b0, out_r);
        return;
    }

    double h[9];
    memcpy(h, h_in, sizeof(h));

    double ns[3], nd[3];
    v3_cross(a0, a1, ns);
    v3_cross(b0, b1, nd);
    double ln = v3_norm(ns), ld = v3_norm(nd);
    if (ln > 1e-9 && ld > 1e-9) {
        double vs[3], vd[3];
        v3_scale(ns, v3_norm(a0) / (ln + SINEW_EPS), vs);
        v3_scale(nd, v3_norm(b0) / (ld + SINEW_EPS), vd);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                h[i * 3 + j] += vs[i] * vd[j];
            }
        }
    }

    double h_reg[9];
    sinew_regularize(h, h_reg);
    double r[9];
    sinew_ns30(h_reg, r);
    if (!sinew_valid9(r)) {
        sinew_kabsch(h_reg, out_r);
        return;
    }
    memcpy(out_r, r, sizeof(r));
}

void sinew_align(const double *targets_xyz, const double *sources_xyz,
                  size_t count, double out_r[9])
{
    double h[9] = { 0 };
    for (size_t p = 0; p < count; p++) {
        const double *a = &targets_xyz[p * 3];
        const double *b = &sources_xyz[p * 3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                h[i * 3 + j] += a[i] * b[j];
            }
        }
    }

    static const double zero[3] = { 0, 0, 0 };
    const double *a0 = count > 0 ? &targets_xyz[0] : zero;
    const double *b0 = count > 0 ? &sources_xyz[0] : zero;
    const double *a1 = count > 1 ? &targets_xyz[3] : zero;
    const double *b1 = count > 1 ? &sources_xyz[3] : zero;

    sinew_finish_align(h, (int)count, a0, a1, b0, b1, out_r);
}
