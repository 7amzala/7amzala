// =====================================================
// Physics model of a 4.20 m surf rod on a tripod
//
// The rod is a tapered carbon tube, cut into 5 cm pieces (beam
// finite elements). It rests in the butt cup and on the tripod head,
// and bends in two planes:
//   - in the line's plane (fish, waves, wind on the line)
//   - sideways (wind on the rod)
// Every millisecond it works out how each point of the rod moves,
// and gives what an accelerometer fixed on top of the rod would read.
//
// Simplified: small bending (linear), the line is a force at the tip
// (braid: tugs arrive at the tip almost as they are), no twisting.
// =====================================================

#pragma once
#include <cmath>
#include <vector>
#include <random>

struct RodConfig
{
  double length = 4.20;      // m
  double headPos = 1.25;     // tripod head, m from the butt
  double angleDeg = 65;      // rod above horizontal
  double lineDownDeg = 15;   // line below horizontal, from the tip to the sea
  double E = 130e9;          // carbon, Pa
  double rho = 1550;         // kg/m3
  double damping = 0.025;    // 2.5 % carbon + line; air adds more in wind (set per run)
  double sensorMass = 0.008; // kg (sensor in its case)
};

// Band matrix, half-bandwidth B (beam elements couple only neighbours)
struct Band
{
  int n = 0;
  static const int B = 3;
  std::vector<double> d;   // n x (2B+1)
  void init(int n_) { n = n_; d.assign(n * (2 * B + 1), 0.0); }
  double& at(int r, int c) { return d[r * (2 * B + 1) + (c - r + B)]; }
  double get(int r, int c) const { int k = c - r; if (k < -B || k > B) return 0; return d[r * (2 * B + 1) + (k + B)]; }
  void mul(const std::vector<double>& x, std::vector<double>& y) const
  {
    for (int r = 0; r < n; r++) {
      double s = 0;
      int c0 = r - B < 0 ? 0 : r - B, c1 = r + B >= n ? n - 1 : r + B;
      for (int c = c0; c <= c1; c++) s += get(r, c) * x[c];
      y[r] = s;
    }
  }
};

// Banded Cholesky (matrix is symmetric positive definite)
struct BandChol
{
  int n = 0; static const int B = 3;
  std::vector<double> L;   // n x (B+1): L(r, r-k) for k=0..B
  double& l(int r, int c) { return L[r * (B + 1) + (r - c)]; }
  void factor(const Band& A)
  {
    n = A.n; L.assign(n * (B + 1), 0.0);
    for (int r = 0; r < n; r++) {
      int c0 = r - B < 0 ? 0 : r - B;
      for (int c = c0; c <= r; c++) {
        double s = A.get(r, c);
        int k0 = (r - B > c - B ? r - B : c - B); if (k0 < 0) k0 = 0;
        for (int k = k0; k < c; k++) s -= l(r, k) * l(c, k);
        if (c == r) l(r, r) = std::sqrt(s);
        else l(r, c) = s / l(c, c);
      }
    }
  }
  void solve(std::vector<double>& x)
  {
    for (int r = 0; r < n; r++) {
      double s = x[r]; int k0 = r - B < 0 ? 0 : r - B;
      for (int k = k0; k < r; k++) s -= l(r, k) * x[k];
      x[r] = s / l(r, r);
    }
    for (int r = n - 1; r >= 0; r--) {
      double s = x[r]; int k1 = r + B >= n ? n - 1 : r + B;
      for (int k = r + 1; k <= k1; k++) s -= l(k, r) * x[k];
      x[r] = s / l(r, r);
    }
  }
};

class RodModel
{
public:
  RodConfig cfg;
  int nEl = 84, nNode = 85, nDof = 170;
  double h = 0.05;
  std::vector<double> od, id, mpl, EI;    // per node: outer/inner diameter, mass per length, EI
  double totalMass = 0, f1 = 0;
  int sensorNode = 80, headNode = 25;
  double dt = 0.001;

  // state, two planes: 0 = line plane, 1 = sideways
  std::vector<double> u[2], v[2], a[2];
  Band K, M, C;
  BandChol Keff;
  double alpha0 = 0;
  int fixedDof[2] = { 0, 0 };
  // Newmark with a little numerical damping of very fast wiggles (gamma > 1/2)
  const double gam = 0.6, bet = 0.3025;

  double outerD(double s)   // m, s from the butt
  {
    // typical 4.20 m 3-piece surf rod: 25 mm butt, 3.0 mm tip
    const double S[4] = { 0, 1.4, 2.8, 4.2 }, D[4] = { 25e-3, 18e-3, 12e-3, 3.0e-3 };
    for (int i = 0; i < 3; i++) if (s <= S[i + 1]) return D[i] + (D[i + 1] - D[i]) * (s - S[i]) / (S[i + 1] - S[i]);
    return D[3];
  }

  void build(double sensorFromTip)
  {
    nEl = (int)std::round(cfg.length / 0.05); nNode = nEl + 1; nDof = 2 * nNode; h = cfg.length / nEl;
    headNode = (int)std::round(cfg.headPos / h);
    sensorNode = nNode - 1 - (int)std::round(sensorFromTip / h);
    if (sensorNode < headNode + 1) sensorNode = headNode + 1;
    od.resize(nNode); id.resize(nNode); mpl.resize(nNode); EI.resize(nNode);
    totalMass = 0;
    for (int i = 0; i < nNode; i++) {
      double s = i * h, D = outerD(s);
      double wall = std::max(0.6e-3, 0.09 * D);
      double Di = D <= 3.2e-3 ? 0 : std::max(0.0, D - 2 * wall);
      od[i] = D; id[i] = Di;
      mpl[i] = cfg.rho * M_PI / 4 * (D * D - Di * Di);
      EI[i] = cfg.E * M_PI / 64 * (std::pow(D, 4) - std::pow(Di, 4));
    }
    K.init(nDof); M.init(nDof); C.init(nDof);
    for (int e = 0; e < nEl; e++) {
      double ei = 0.5 * (EI[e] + EI[e + 1]), m = 0.5 * (mpl[e] + mpl[e + 1]);
      totalMass += m * h;
      double k[4][4] = { { 12, 6 * h, -12, 6 * h }, { 6 * h, 4 * h * h, -6 * h, 2 * h * h }, { -12, -6 * h, 12, -6 * h }, { 6 * h, 2 * h * h, -6 * h, 4 * h * h } };
      double mm[4][4] = { { 156, 22 * h, 54, -13 * h }, { 22 * h, 4 * h * h, 13 * h, -3 * h * h }, { 54, 13 * h, 156, -22 * h }, { -13 * h, -3 * h * h, -22 * h, 4 * h * h } };
      int dof[4] = { 2 * e, 2 * e + 1, 2 * e + 2, 2 * e + 3 };
      for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        K.at(dof[r], dof[c]) += ei / (h * h * h) * k[r][c];
        M.at(dof[r], dof[c]) += m * h / 420 * mm[r][c];
      }
    }
    // guides (8, heavy near the butt, tiny near the tip) + tip ring
    const double gPos[9] = { 1.6, 2.15, 2.6, 3.0, 3.35, 3.65, 3.9, 4.08, 4.2 };
    const double gMass[9] = { 9e-3, 6e-3, 4e-3, 3e-3, 2e-3, 1.5e-3, 1.2e-3, 1e-3, 0.8e-3 };
    for (int i = 0; i < 9; i++) { int n = std::min(nNode - 1, (int)std::round(gPos[i] / h)); M.at(2 * n, 2 * n) += gMass[i]; totalMass += gMass[i]; }
    // reel, between the supports
    M.at(2 * 12, 2 * 12) += 0.45;
    // sensor
    M.at(2 * sensorNode, 2 * sensorNode) += cfg.sensorMass;
    // supports: butt cup (node 0) and tripod head hold the rod (no sideways movement there)
    fixedDof[0] = 0; fixedDof[1] = 2 * headNode;
    for (int f : fixedDof) {
      for (int k = -Band::B; k <= Band::B; k++) {
        int c = f + k; if (c < 0 || c >= nDof) continue;
        K.at(f, c) = 0; K.at(c, f) = 0; M.at(f, c) = 0; M.at(c, f) = 0;
      }
      K.at(f, f) = 1; M.at(f, f) = 1e-6;
    }

    f1 = firstFrequency();
    // Rayleigh damping: cfg.damping at f1 and at 25 Hz
    double w1 = 2 * M_PI * f1, w2 = 2 * M_PI * 25, z = cfg.damping;
    double am = 2 * z * w1 * w2 / (w1 + w2), bk = 2 * z / (w1 + w2);
    for (size_t i = 0; i < C.d.size(); i++) C.d[i] = am * M.d[i] + bk * K.d[i];

    Band A; A.init(nDof);
    for (size_t i = 0; i < A.d.size(); i++) A.d[i] = K.d[i] + gam / (bet * dt) * C.d[i] + 1.0 / (bet * dt * dt) * M.d[i];
    Keff.factor(A);

    alpha0 = cfg.angleDeg * M_PI / 180;
    for (int p = 0; p < 2; p++) { u[p].assign(nDof, 0); v[p].assign(nDof, 0); a[p].assign(nDof, 0); }
  }

  double firstFrequency()
  {
    BandChol kc; kc.factor(K);
    std::vector<double> x(nDof, 1.0), y(nDof), z(nDof);
    double lam = 0;
    for (int it = 0; it < 80; it++) {
      M.mul(x, y); z = y; kc.solve(z);
      std::vector<double> kz(nDof), mz(nDof); K.mul(z, kz); M.mul(z, mz);
      double num = 0, den = 0; for (int i = 0; i < nDof; i++) { num += z[i] * kz[i]; den += z[i] * mz[i]; }
      lam = num / den;
      double nrm = 0; for (double q : z) nrm += q * q; nrm = std::sqrt(nrm);
      for (int i = 0; i < nDof; i++) x[i] = z[i] / nrm;
    }
    return std::sqrt(lam) / (2 * M_PI);
  }

  // Forces for this step. Fills F[plane]
  // lineT: line tension (N). windU: wind speed (m/s) on the rod.
  // windDir: 0 = blowing across (sideways), 1 = along the line's plane
  // buffet[p][k]: local wind flutter for plane p, rod segment k (6 segments, not all in step)
  void forces(double lineT, double windU, double windDir, const double buffet[2][6], std::vector<double> F[2])
  {
    for (int p = 0; p < 2; p++) F[p].assign(nDof, 0.0);
    // gravity, perpendicular part (in the line's plane)
    double gPerp = -9.81 * std::cos(alpha0);
    for (int i = 0; i < nNode; i++) F[0][2 * i] += mpl[i] * h * gPerp;
    // line at the tip: perpendicular part of the tension
    double lineDir = -cfg.lineDownDeg * M_PI / 180;
    double comp = std::cos(lineDir) * -std::sin(alpha0) + std::sin(lineDir) * std::cos(alpha0);
    F[0][2 * (nNode - 1)] += lineT * comp;
    // wind drag on the rod
    for (int i = headNode; i < nNode; i++) {
      double q = 0.5 * 1.2 * 1.1 * od[i] * windU * std::fabs(windU) * h;
      int k = (i - headNode) * 6 / (nNode - headNode);
      F[1][2 * i] += q * std::cos(windDir) * (1 + buffet[1][k]);
      F[0][2 * i] += q * std::sin(windDir) * std::sin(alpha0) * (1 + buffet[0][k]);
    }
  }

  void staticStart(double lineT, double windU, double windDir)
  {
    double bf[2][6] = {};
    std::vector<double> F[2]; forces(lineT, windU, windDir, bf, F);
    BandChol kc; kc.factor(K);
    for (int p = 0; p < 2; p++) { for (int f : fixedDof) F[p][f] = 0; u[p] = F[p]; kc.solve(u[p]); v[p].assign(nDof, 0); a[p].assign(nDof, 0); }
  }

  void step(std::vector<double> F[2])
  {
    std::vector<double> t1(nDof), t2(nDof), rhs(nDof), m1(nDof), c1(nDof);
    const double b1 = 1.0 / (bet * dt * dt), b2 = 1.0 / (bet * dt), b3 = 1.0 / (2 * bet) - 1;
    const double c1a = gam / (bet * dt), c1b = gam / bet - 1, c1c = dt * (gam / (2 * bet) - 1);
    for (int p = 0; p < 2; p++) {
      for (int f : fixedDof) F[p][f] = 0;
      for (int i = 0; i < nDof; i++) { t1[i] = b1 * u[p][i] + b2 * v[p][i] + b3 * a[p][i]; t2[i] = c1a * u[p][i] + c1b * v[p][i] + c1c * a[p][i]; }
      M.mul(t1, m1); C.mul(t2, c1);
      for (int i = 0; i < nDof; i++) rhs[i] = F[p][i] + m1[i] + c1[i];
      Keff.solve(rhs);
      for (int i = 0; i < nDof; i++) {
        double an = b1 * (rhs[i] - u[p][i]) - b2 * v[p][i] - b3 * a[p][i];
        v[p][i] += dt * ((1 - gam) * a[p][i] + gam * an);
        a[p][i] = an; u[p][i] = rhs[i];
      }
    }
  }

  // What the accelerometer reads (in g), sensor axes: x along the rod,
  // z up from the rod (in the line's plane), y sideways
  void sensor(int node, double out[3])
  {
    double th = u[0][2 * node + 1], ph = u[1][2 * node + 1];
    double al = alpha0 + th;
    out[0] = std::sin(al);
    out[2] = std::cos(al) + a[0][2 * node] / 9.81;
    out[1] = a[1][2 * node] / 9.81 - ph * std::sin(al);
  }

  double tipDeflection() { return u[0][2 * (nNode - 1)]; }
  double slopeDeg(int node) { return u[0][2 * node + 1] * 180 / M_PI; }
};
