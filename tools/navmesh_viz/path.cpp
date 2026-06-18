#include "path.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Portability shim for Pathfinder's hash_combine.h, which calls
// internal::distribute(std::hash<T>{}(v)). That argument is size_t; on platforms
// where size_t is distinct from both uint32_t and uint64_t (e.g. macOS arm64,
// where size_t is unsigned long but uint64_t is unsigned long long), the two
// distribute() overloads are ambiguous. Because the call is a qualified-id, its
// candidate set is fixed at hash_combine's definition point, so this overload
// must be declared BEFORE the Pathfinder headers below. SFINAE keeps it inert
// where size_t already equals one of those types (e.g. Linux), so it never
// conflicts with Pathfinder's own definitions there.
namespace pathfinder {
namespace internal {
// Matches the declarations in hash_combine.h (redeclared harmlessly when that
// header is included below); needed so the shim's body can resolve them.
std::uint32_t distribute(const std::uint32_t &n);
std::uint64_t distribute(const std::uint64_t &n);
template <typename T>
std::enable_if_t<std::is_same_v<T, std::size_t> &&
                     !std::is_same_v<std::size_t, std::uint32_t> &&
                     !std::is_same_v<std::size_t, std::uint64_t>,
                 std::uint64_t>
distribute(const T &n) {
  return distribute(static_cast<std::uint64_t>(n));
}
} // namespace internal
} // namespace pathfinder

#include "pathfinder.h"
#include "path.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace navmesh_viz {

namespace {

constexpr double kAgentRadius = 3.14;

// Collects every stacked surface height (terrain plus each object floor) present
// at the absolute (x, z) column, expressed as world Y. Empty if the region is
// not loaded (e.g. just outside the cluster).
std::vector<float> surfaceHeightsAtPoint(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    float absoluteX, float absoluteZ) {
  std::vector<float> heights;
  const auto regionAndPoint =
      triangulation.transformAbsolutePointIntoRegion({absoluteX, 0.0f, absoluteZ});
  const uint16_t regionId = regionAndPoint.first;
  const sro::math::Vector3 &regionPoint = regionAndPoint.second;
  const auto &regionMap = navmesh.getRegionMap();
  auto it = regionMap.find(regionId);
  if (it == regionMap.end()) {
    return heights;
  }
  heights.push_back(it->second.getHeightAtPoint(regionPoint));

  // Each (object instance, area) over this column is a separate stacked floor.
  for (uint32_t instanceId : it->second.objectInstanceIds) {
    sro::navmesh::ObjectResource resource;
    try {
      resource = navmesh.getTransformedObjectResourceForRegion(instanceId, regionId);
    } catch (...) {
      continue;
    }
    const std::set<uint32_t> areaIds(
        resource.cellAreaIds.begin(), resource.cellAreaIds.end());
    for (uint32_t areaId : areaIds) {
      try {
        heights.push_back(resource.getHeight(regionPoint, areaId));
      } catch (...) {
        // The point is not on this floor; skip it.
      }
    }
  }
  return heights;
}

// Assigns a height to each 2D waypoint by choosing, per waypoint, one of its
// stacked surfaces (terrain or an object floor) so that the total vertical
// movement along the path is minimized, anchored to the clicked start and goal
// heights. A purely greedy nearest-altitude walk stays stuck on terrain because
// each surface is locally continuous; this global choice instead commits to the
// object floor that actually reaches the goal, so the path climbs onto a
// structure and follows it. Waypoints carry only (x, z); heights[i] is the Y.
std::vector<float> reconstructHeights(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::vector<std::pair<float, float>> &points,
    float startY, float goalY) {
  const size_t n = points.size();
  std::vector<std::vector<float>> candidates(n);
  for (size_t i = 0; i < n; ++i) {
    candidates[i] =
        surfaceHeightsAtPoint(navmesh, triangulation, points[i].first, points[i].second);
    if (candidates[i].empty()) {
      // Region outside the loaded cluster: anchor to the nearest clicked height.
      candidates[i].push_back(i + 1 == n ? goalY : startY);
    }
  }

  // DP over the candidate lattice. cost[i][j] = least total |dY| to reach
  // candidate j of waypoint i (including the start anchor); back[i][j] is the
  // chosen predecessor candidate.
  const float kInf = std::numeric_limits<float>::max();
  std::vector<std::vector<float>> cost(n);
  std::vector<std::vector<int>> back(n);
  for (size_t i = 0; i < n; ++i) {
    cost[i].assign(candidates[i].size(), kInf);
    back[i].assign(candidates[i].size(), -1);
  }
  for (size_t j = 0; j < candidates[0].size(); ++j) {
    cost[0][j] = std::abs(candidates[0][j] - startY);
  }
  for (size_t i = 1; i < n; ++i) {
    for (size_t j = 0; j < candidates[i].size(); ++j) {
      for (size_t k = 0; k < candidates[i - 1].size(); ++k) {
        const float c =
            cost[i - 1][k] + std::abs(candidates[i][j] - candidates[i - 1][k]);
        if (c < cost[i][j]) {
          cost[i][j] = c;
          back[i][j] = static_cast<int>(k);
        }
      }
    }
  }

  // Add the goal anchor to the last waypoint, then pick the cheapest end and
  // backtrack the chosen surface for every waypoint.
  int chosen = 0;
  float bestCost = kInf;
  for (size_t j = 0; j < candidates[n - 1].size(); ++j) {
    const float c = cost[n - 1][j] + std::abs(candidates[n - 1][j] - goalY);
    if (c < bestCost) {
      bestCost = c;
      chosen = static_cast<int>(j);
    }
  }
  std::vector<float> heights(n);
  for (size_t i = n; i-- > 0;) {
    heights[i] = candidates[i][chosen];
    if (i > 0) {
      chosen = back[i][chosen];
    }
  }
  return heights;
}

} // namespace

PathResult findPath(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const sro::math::Vector3 &startAbsolute,
    const sro::math::Vector3 &goalAbsolute) {
  PathResult result;

  pathfinder::PathfinderConfig config(pathfinder::PathfinderAlgorithm::kPolyanya);
  config.setAgentRadius(kAgentRadius);
  config.setTimeout(std::chrono::milliseconds(150));
  pathfinder::Pathfinder<sro::navmesh::triangulation::NavmeshTriangulation> pathfinder(
      triangulation, config);

  // Collects the 2D (x, z) waypoints, dropping exact duplicates of the previous
  // point. Heights are reconstructed in a second pass.
  std::vector<std::pair<float, float>> points;
  auto addPoint = [&](double x, double z) {
    const float fx = static_cast<float>(x);
    const float fz = static_cast<float>(z);
    if (!points.empty() && points.back().first == fx && points.back().second == fz) {
      return;
    }
    points.emplace_back(fx, fz);
  };

  // Polyanya emits only corner waypoints, so a single long straight segment can
  // span terrain whose height varies a lot. Subdivide each segment into short
  // steps so the second pass can sample the surface along it and the path hugs
  // the ground instead of cutting a straight chord through it.
  constexpr double kMaxStep = 30.0; // units between samples (terrain tile = 20)
  auto addSegment = [&](double sx, double sz, double ex, double ez) {
    addPoint(sx, sz);
    const double dx = ex - sx;
    const double dz = ez - sz;
    const double length = std::sqrt(dx * dx + dz * dz);
    const int steps = std::max(1, static_cast<int>(std::ceil(length / kMaxStep)));
    for (int s = 1; s <= steps; ++s) {
      const double t = static_cast<double>(s) / steps;
      addPoint(sx + dx * t, sz + dz * t);
    }
  };

  // Build the waypoints while the result (which owns the segment objects) is
  // still alive - the segment pointers must not outlive it.
  try {
    const auto pathfindingResult =
        pathfinder.findShortestPath(startAbsolute, goalAbsolute);
    for (const auto &segment : pathfindingResult.shortestPath) {
      auto *straight = dynamic_cast<pathfinder::StraightPathSegment *>(segment.get());
      if (straight == nullptr) {
        continue; // ignore arc segments; the chord between straights is kept
      }
      addSegment(straight->startPoint.x(), straight->startPoint.y(),
                 straight->endPoint.x(), straight->endPoint.y());
    }
  } catch (const std::exception &ex) {
    result.error = ex.what();
    return result;
  }

  if (points.empty()) {
    result.error = "No path found";
    return result;
  }

  const std::vector<float> heights = reconstructHeights(
      navmesh, triangulation, points, startAbsolute.y, goalAbsolute.y);
  result.waypoints.reserve(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    result.waypoints.emplace_back(points[i].first, heights[i], points[i].second);
  }
  result.ok = true;
  return result;
}

} // namespace navmesh_viz
