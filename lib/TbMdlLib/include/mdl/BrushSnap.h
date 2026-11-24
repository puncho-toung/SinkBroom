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

#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

// The safe snap check (Phase 2C, M3.7): rounding a convex brush's vertices to a grid keeps
// the brush only if its faces stay the same and no face contact with a neighbour opens.
// Every test after rounding is exact in 64-bit integers.
namespace tb::mdl::brushsnap
{
struct V3
{
  double x, y, z;
};
// A face's three defining points, parallel to Poly::f.
using PlanePoints = std::array<V3, 3>;
// A convex brush: its vertices, one loop of vertex indices a face, and the faces' points.
struct Poly
{
  std::vector<V3> v;
  std::vector<std::vector<uint32_t>> f;
  std::vector<PlanePoints> facePoints;
};
enum Verdict : uint8_t
{
  SAME,
  OK,
  MERGE,
  FACESET,
  FLAT,
  RANGE
};
constexpr uint32_t NoIndex = UINT32_MAX;
constexpr double PlaneEpsilon = 0.0001;
// Faces this close count as a contact: the map compiler merges parallel planes within 0.01
// (dmap's DIST_EPSILON), so such faces seal in the game. Whether a contact survives the
// snap is still decided exactly.
constexpr double ContactEpsilon = 0.01;
constexpr double NormalEpsilon = 0.00001;
// Two faces overlap only where one crosses the other by more than this: float noise in the
// editor's vertices (about 1e-11) must not make a contact of two faces that touch on a line.
constexpr double OverlapEpsilon = 0.000001;
// The largest coordinate the check accepts; with it every exact product fits in int64.
constexpr double MaxCoordinate = 131072.0;
struct SnapVerdict
{
  uint8_t verdict = RANGE;
  uint32_t face = NoIndex, vertex = NoIndex;
};
SnapVerdict CheckSnap(const Poly& p, int32_t q);
// b indexes selected followed by unselected; a always indexes selected.
struct Contact
{
  uint32_t a, fa, b, fb;
};
// A pair of brushes to test for contacts, in the index space of Contact: first selected,
// second any other brush, and two selected brushes as (lower, higher). Each pair must be
// given once; an invalid pair throws std::invalid_argument.
using ContactPair = std::pair<uint32_t, uint32_t>;
// Every pair of brushes (for small sets and checks).
void FindContacts(
  const std::vector<Poly>& selected,
  const std::vector<Poly>& unselected,
  std::vector<Contact>* out);
// Only the given pairs (an editor gives the pairs whose bounds touch).
void FindContacts(
  const std::vector<Poly>& selected,
  const std::vector<Poly>& unselected,
  const std::vector<ContactPair>& pairs,
  std::vector<Contact>* out);
bool ContactSurvives(
  const Poly& a, uint32_t fa, const Poly& b, uint32_t fb, int32_t q, bool bSelected);
struct SnapPlan
{
  std::vector<SnapVerdict> verdicts;
  // every contact found, and those the snap would open
  std::vector<Contact> contacts;
  std::vector<Contact> opened;
  bool commit = false;
};
void PlanSafeSnap(
  const std::vector<Poly>& selected,
  const std::vector<Poly>& unselected,
  int32_t q,
  SnapPlan* out);
void PlanSafeSnap(
  const std::vector<Poly>& selected,
  const std::vector<Poly>& unselected,
  const std::vector<ContactPair>& pairs,
  int32_t q,
  SnapPlan* out);
// The rounded vertices of p; snap never changes an input object.
bool RoundVertices(const Poly& p, int32_t q, std::vector<V3>* out);
} // namespace tb::mdl::brushsnap
