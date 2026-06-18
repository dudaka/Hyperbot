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

namespace navmesh_viz {

namespace {

constexpr double kAgentRadius = 3.14;

// Reconstructs a surface height for an absolute (x, z) point by looking up the
// terrain height in whichever region it falls in. Falls back to inputY if the
// region is unknown (e.g. just outside the loaded cluster).
float reconstructHeight(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    float absoluteX, float absoluteZ, float fallbackY) {
  const auto regionAndPoint =
      triangulation.transformAbsolutePointIntoRegion({absoluteX, 0.0f, absoluteZ});
  const auto &regionMap = navmesh.getRegionMap();
  auto it = regionMap.find(regionAndPoint.first);
  if (it == regionMap.end()) {
    return fallbackY;
  }
  return it->second.getHeightAtPoint(regionAndPoint.second);
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

  // Appends an absolute waypoint with reconstructed surface height, dropping
  // exact duplicates of the previous point. The two segments flanking an arc
  // share endpoints only when collinear; otherwise we keep the chord.
  auto addPoint = [&](double x, double z) {
    const float fx = static_cast<float>(x);
    const float fz = static_cast<float>(z);
    if (!result.waypoints.empty()) {
      const auto &last = result.waypoints.back();
      if (last.x == fx && last.z == fz) {
        return;
      }
    }
    const float y = reconstructHeight(navmesh, triangulation, fx, fz, startAbsolute.y);
    result.waypoints.emplace_back(fx, y, fz);
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
      addPoint(straight->startPoint.x(), straight->startPoint.y());
      addPoint(straight->endPoint.x(), straight->endPoint.y());
    }
  } catch (const std::exception &ex) {
    result.error = ex.what();
    return result;
  }

  if (result.waypoints.empty()) {
    result.error = "No path found";
    return result;
  }
  result.ok = true;
  return result;
}

} // namespace navmesh_viz
