#ifndef NAVMESH_VIZ_PATH_H_
#define NAVMESH_VIZ_PATH_H_

#include <silkroad_lib/math/vector3.hpp>
#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/navmesh/triangulation/navmeshTriangulation.hpp>

#include <string>
#include <vector>

namespace navmesh_viz {

struct PathResult {
  bool ok{false};
  std::string error;
  std::vector<sro::math::Vector3> waypoints; // absolute frame, surface height
};

// Runs Polyanya between two absolute-frame points and returns the path as
// absolute waypoints, with each waypoint's height reconstructed from the
// terrain so the line follows the surface (waypoints are otherwise 2D).
PathResult findPath(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const sro::math::Vector3 &startAbsolute,
    const sro::math::Vector3 &goalAbsolute);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_PATH_H_
