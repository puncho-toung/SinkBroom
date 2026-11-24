/*
 Copyright (C) 2026 sinkbroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mdl/BrushSnap.h"

#include <cmath>
#include <stdexcept>
#include <utility>

// The core is the Phase 2C draft (plans/map-graph/phase2c/stages/2C-3/replan/astra-2),
// checked 28 of 28 there; the editor's chunk command clips with Brush::clip instead of the
// draft's own clipper, so the draft's chunk part is left out here.
namespace tb::mdl::brushsnap
{
namespace
{
struct I3
{
  int64_t x, y, z;
  bool operator==(const I3& b) const { return x == b.x && y == b.y && z == b.z; }
};
I3 Sub(I3 a, I3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
I3 Cross(I3 a, I3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
int64_t Dot(I3 a, I3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
int Sign(int64_t a) { return (a > 0) - (a < 0); }
int Axis(I3 n) {
    const auto x = std::abs(n.x), y = std::abs(n.y), z = std::abs(n.z);
    return x >= y && x >= z ? 0 : y >= z ? 1 : 2;
}
int64_t Component(I3 p, int a) { return a == 0 ? p.x : a == 1 ? p.y : p.z; }
double Component(V3 p, int a) { return a == 0 ? p.x : a == 1 ? p.y : p.z; }
int64_t Turn(I3 a, I3 b, I3 c, int axis) {
    const int u = (axis+1)%3, v = (axis+2)%3;
    return (Component(b,u)-Component(a,u))*(Component(c,v)-Component(a,v))
         - (Component(b,v)-Component(a,v))*(Component(c,u)-Component(a,u));
}
V3 Sub(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V3 Mul(V3 a, double k) { return {a.x*k, a.y*k, a.z*k}; }
V3 Cross(V3 a, V3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double Dot(V3 a, V3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
double Length(V3 a) { return std::sqrt(Dot(a,a)); }
int Axis(V3 n) {
    const auto x = std::abs(n.x), y = std::abs(n.y), z = std::abs(n.z);
    return x >= y && x >= z ? 0 : y >= z ? 1 : 2;
}
bool Round(const Poly& p, int32_t q, std::vector<I3>& out, uint32_t& bad, bool& same) {
    if (q <= 0 || p.v.size() > UINT32_MAX || p.f.size() > UINT32_MAX) { return false; }
    same = true;
    out.clear();
    out.reserve(p.v.size());
    for (uint32_t i = 0; i < p.v.size(); ++i) {
        I3 r{};
        for (int a = 0; a < 3; ++a) {
            const double x = Component(p.v[i],a);
            if (!std::isfinite(x) || std::abs(x) > MaxCoordinate) { bad = i; return false; }
            const double t = x / q;
            const double y = q * (t > 0 ? std::floor(t+0.5) : std::ceil(t-0.5));
            if (std::abs(y) > MaxCoordinate) { bad = i; return false; }
            (a == 0 ? r.x : a == 1 ? r.y : r.z) = static_cast<int64_t>(y);
            same = same && x == y;
        }
        out.push_back(r);
    }
    return true;
}
bool FacePlane(const std::vector<I3>& v, const std::vector<uint32_t>& f, I3& n) {
    if (f.size() < 3) { return false; }
    for (const auto i : f) { if (i >= v.size()) { return false; } }
    for (size_t i = 1; i+1 < f.size(); ++i) {
        n = Cross(Sub(v[f[i]],v[f[0]]),Sub(v[f[i+1]],v[f[0]]));
        if (n.x || n.y || n.z) { return true; }
    }
    return false;
}
struct DPlane { V3 n, p; };
DPlane Plane(const Poly& p, uint32_t f) {
    if (f >= p.f.size() || p.f[f].size() < 3) { throw std::invalid_argument("invalid face"); }
    PlanePoints pts;
    if (p.facePoints.size() == p.f.size()) { pts = p.facePoints[f]; }
    else { for (int i = 0; i < 3; ++i) { pts[i] = p.v.at(p.f[f][i]); } }
    V3 n = Cross(Sub(pts[1],pts[0]),Sub(pts[2],pts[0]));
    const double len = Length(n);
    if (len == 0) { throw std::invalid_argument("degenerate face plane"); }
    return {Mul(n,1/len),pts[0]};
}
std::vector<DPlane> Planes(const Poly& p) {
    std::vector<DPlane> out;
    out.reserve(p.f.size());
    for (uint32_t f = 0; f < p.f.size(); ++f) { out.push_back(Plane(p,f)); }
    return out;
}
bool OnPlane(const Poly& p, uint32_t f, DPlane plane, double epsilon = PlaneEpsilon) {
    for (auto i : p.f[f]) {
        if (std::abs(Dot(plane.n,Sub(p.v.at(i),plane.p))) > epsilon) { return false; }
    }
    return true;
}
// Separating axis test in the dominant projection; exact integer area criterion.
bool Overlap(const std::vector<I3>& a, const std::vector<uint32_t>& af,
             const std::vector<I3>& b, const std::vector<uint32_t>& bf, int axis) {
    const auto separated = [axis](const auto& av, const auto& face, const auto& bv, const auto& other) {
        for (size_t e = 0; e < face.size(); ++e) {
            const I3 p = av[face[e]], q = av[face[(e+1)%face.size()]];
            int interior = 0;
            for (auto i : face) { if ((interior = Sign(Turn(p,q,av[i],axis))) != 0) { break; } }
            bool crosses = false;
            for (auto i : other) { if (Sign(Turn(p,q,bv[i],axis)) == interior && interior) { crosses = true; break; } }
            if (!crosses) { return true; }
        }
        return false;
    };
    return !separated(a,af,b,bf) && !separated(b,bf,a,af);
}
bool Overlap(const Poly& a, uint32_t fa, const Poly& b, uint32_t fb, int axis) {
    // Coplanarity uses TB tolerances; positive projected overlap is a strict test.
    const auto separated = [axis](const Poly& av, uint32_t face, const Poly& bv, uint32_t other) {
        const int u = (axis+1)%3, v = (axis+2)%3;
        const auto& loop = av.f[face];
        for (size_t e = 0; e < loop.size(); ++e) {
            const V3 p = av.v[loop[e]], q = av.v[loop[(e+1)%loop.size()]];
            const double dx = Component(q,u)-Component(p,u), dy = Component(q,v)-Component(p,v);
            const double len = std::hypot(dx,dy);
            if (len == 0) { continue; }
            const auto side = [&](V3 r) {
                return (dx*(Component(r,v)-Component(p,v))-dy*(Component(r,u)-Component(p,u)))/len;
            };
            double s = 0;
            for (auto i : loop) { if ((s = side(av.v[i])) != 0) { break; } }
            if (s == 0) { return true; }
            bool crosses = false;
            for (auto i : bv.f[other]) {
                if ((s > 0 ? side(bv.v[i]) : -side(bv.v[i])) > OverlapEpsilon) { crosses = true; break; }
            }
            if (!crosses) { return true; }
        }
        return false;
    };
    return !separated(a,fa,b,fb) && !separated(b,fb,a,fa);
}
// The contacts between two brushes, given their face planes.
void PairContacts(const Poly& pa, const std::vector<DPlane>& ap, uint32_t a,
                  const Poly& pb, const std::vector<DPlane>& bp, uint32_t b,
                  std::vector<Contact>* out) {
    for (uint32_t fa = 0; fa < pa.f.size(); ++fa) {
        for (uint32_t fb = 0; fb < pb.f.size(); ++fb) {
            if (1+Dot(ap[fa].n,bp[fb].n) >= NormalEpsilon) { continue; }
            if (OnPlane(pa,fa,bp[fb],ContactEpsilon) && OnPlane(pb,fb,ap[fa],ContactEpsilon)
                && Overlap(pa,fa,pb,fb,Axis(ap[fa].n))) {
                out->push_back({a,fa,b,fb});
            }
        }
    }
}
void Finish(const std::vector<Poly>& selected, const std::vector<Poly>& unselected,
            std::vector<Contact> contacts, int32_t q, SnapPlan* out) {
    *out = SnapPlan{};
    out->commit = true;
    out->verdicts.reserve(selected.size());
    for (const auto& p : selected) {
        const auto v = CheckSnap(p,q);
        out->verdicts.push_back(v);
        out->commit = out->commit && (v.verdict == SAME || v.verdict == OK);
    }
    for (const auto& c : contacts) {
        const bool bs = c.b < selected.size();
        const auto& b = bs ? selected[c.b] : unselected[c.b-selected.size()];
        if (!ContactSurvives(selected[c.a],c.fa,b,c.fb,q,bs)) { out->opened.push_back(c); }
    }
    out->commit = out->commit && out->opened.empty();
    out->contacts = std::move(contacts);
}
} // namespace

bool RoundVertices(const Poly& p, int32_t q, std::vector<V3>* out) {
    if (!out) { return false; }
    std::vector<I3> r;
    bool same = false; uint32_t bad = NoIndex;
    if (!Round(p,q,r,bad,same)) { out->clear(); return false; }
    out->clear();
    out->reserve(r.size());
    for (auto v : r) { out->push_back({double(v.x),double(v.y),double(v.z)}); }
    return true;
}

SnapVerdict CheckSnap(const Poly& p, int32_t q) {
    std::vector<I3> v;
    bool same = false; uint32_t bad = NoIndex;
    if (!Round(p,q,v,bad,same)) { return {RANGE,NoIndex,bad}; }
    for (uint32_t i = 0; i < v.size(); ++i) {
        for (uint32_t j = 0; j < i; ++j) { if (v[i] == v[j]) { return {MERGE,NoIndex,i}; } }
    }
    if (v.size() < 4 || p.f.size() < 4) { return {FLAT,NoIndex,NoIndex}; }
    I3 rankNormal{};
    for (size_t i = 2; i < v.size(); ++i) {
        rankNormal = Cross(Sub(v[1],v[0]),Sub(v[i],v[0]));
        if (rankNormal.x || rankNormal.y || rankNormal.z) { break; }
    }
    bool fullDimensional = false;
    for (auto point : v) {
        if (Dot(rankNormal,Sub(point,v[0])) != 0) { fullDimensional = true; break; }
    }
    if (!fullDimensional) { return {FLAT,NoIndex,NoIndex}; }
    for (uint32_t fi = 0; fi < p.f.size(); ++fi) {
        const auto& f = p.f[fi]; I3 n{};
        if (!FacePlane(v,f,n)) { return {FLAT,fi,NoIndex}; }
        std::vector<bool> member(v.size(),false);
        for (auto i : f) {
            if (member[i]) { return {FACESET,fi,i}; }
            member[i] = true;
            if (Dot(n,Sub(v[i],v[f[0]])) != 0) { return {FACESET,fi,i}; }
        }
        // C3 is unoriented: a thin tetrahedron may reverse without changing facets.
        int inside = 0;
        for (uint32_t i = 0; i < v.size(); ++i) {
            if (member[i]) { continue; }
            const int side = Sign(Dot(n,Sub(v[i],v[f[0]])));
            if (!side || (inside && side != inside)) { return {FACESET,fi,i}; }
            inside = side;
        }
        if (!inside) { return {FLAT,fi,NoIndex}; }
        const int axis = Axis(n);
        int winding = 0;
        for (size_t e = 0; e < f.size(); ++e) {
            const auto a = f[e], b = f[(e+1)%f.size()];
            for (auto c : f) {
                if (c == a || c == b) { continue; }
                const int s = Sign(Turn(v[a],v[b],v[c],axis));
                if (!s || (winding && s != winding)) { return {FACESET,fi,c}; }
                winding = s;
            }
        }
    }
    return {static_cast<uint8_t>(same ? SAME : OK),NoIndex,NoIndex};
}

void FindContacts(const std::vector<Poly>& selected, const std::vector<Poly>& unselected,
                  std::vector<Contact>* out) {
    if (!out) { throw std::invalid_argument("null contact output"); }
    out->clear();
    const size_t count = selected.size()+unselected.size();
    std::vector<std::vector<DPlane>> planes;
    planes.reserve(count);
    for (const auto& p : selected) { planes.push_back(Planes(p)); }
    for (const auto& p : unselected) { planes.push_back(Planes(p)); }
    for (uint32_t a = 0; a < selected.size(); ++a) {
        for (uint32_t b = a+1; b < count; ++b) {
            const auto& pb = b < selected.size() ? selected[b] : unselected[b-selected.size()];
            PairContacts(selected[a],planes[a],a,pb,planes[b],b,out);
        }
    }
}

void FindContacts(const std::vector<Poly>& selected, const std::vector<Poly>& unselected,
                  const std::vector<ContactPair>& pairs, std::vector<Contact>* out) {
    if (!out) { throw std::invalid_argument("null contact output"); }
    out->clear();
    const size_t count = selected.size()+unselected.size();
    // Each brush's planes once, on first use; the vector never grows, so references stay valid.
    std::vector<std::vector<DPlane>> planes(count);
    std::vector<bool> ready(count,false);
    const auto poly = [&](uint32_t i) -> const Poly& {
        return i < selected.size() ? selected[i] : unselected[i-selected.size()];
    };
    const auto planesOf = [&](uint32_t i) -> const std::vector<DPlane>& {
        if (!ready[i]) { planes[i] = Planes(poly(i)); ready[i] = true; }
        return planes[i];
    };
    for (const auto& [a,b] : pairs) {
        // a selected; b any other brush; two selected brushes only as (lower, higher), as the all-pairs version
        if (a >= selected.size() || b >= count || a == b || (b < selected.size() && b < a)) {
            throw std::invalid_argument("invalid contact pair");
        }
        const auto& ap = planesOf(a);
        const auto& bp = planesOf(b);
        PairContacts(poly(a),ap,a,poly(b),bp,b,out);
    }
}

bool ContactSurvives(const Poly& a, uint32_t fa, const Poly& b, uint32_t fb,
                     int32_t q, bool bSelected) {
    if (fa >= a.f.size() || fb >= b.f.size()) { return false; }
    std::vector<I3> av, bv;
    bool same = false; uint32_t bad = NoIndex;
    if (!Round(a,q,av,bad,same)) { return false; }
    I3 an{};
    if (!FacePlane(av,a.f[fa],an)) { return false; }
    if (bSelected) {
        if (!Round(b,q,bv,bad,same)) { return false; }
        I3 bn{};
        if (!FacePlane(bv,b.f[fb],bn)) { return false; }
        for (auto i : a.f[fa]) { if (Dot(bn,Sub(av[i],bv[b.f[fb][0]])) != 0) { return false; } }
        for (auto i : b.f[fb]) { if (Dot(an,Sub(bv[i],av[a.f[fa][0]])) != 0) { return false; } }
        return Overlap(av,a.f[fa],bv,b.f[fb],Axis(an));
    }
    Poly rounded = a;
    rounded.facePoints.clear();
    if (!RoundVertices(a,q,&rounded.v)) { return false; }
    const auto bp = Plane(b,fb);
    // Use the rounded integer plane without relying on the first three loop vertices.
    const V3 nd{double(an.x),double(an.y),double(an.z)};
    const DPlane ap{Mul(nd,1/Length(nd)),rounded.v[a.f[fa][0]]};
    return OnPlane(rounded,fa,bp) && OnPlane(b,fb,ap) && Overlap(rounded,fa,b,fb,Axis(an));
}

void PlanSafeSnap(const std::vector<Poly>& selected, const std::vector<Poly>& unselected,
                  int32_t q, SnapPlan* out) {
    if (!out) { throw std::invalid_argument("null snap plan"); }
    std::vector<Contact> contacts;
    FindContacts(selected,unselected,&contacts);
    Finish(selected,unselected,std::move(contacts),q,out);
}

void PlanSafeSnap(const std::vector<Poly>& selected, const std::vector<Poly>& unselected,
                  const std::vector<ContactPair>& pairs, int32_t q, SnapPlan* out) {
    if (!out) { throw std::invalid_argument("null snap plan"); }
    std::vector<Contact> contacts;
    FindContacts(selected,unselected,pairs,&contacts);
    Finish(selected,unselected,std::move(contacts),q,out);
}
} // namespace tb::mdl::brushsnap
