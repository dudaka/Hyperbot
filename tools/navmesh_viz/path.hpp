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
// absolute waypoints. Each waypoint's height is reconstructed by choosing one
// stacked surface per waypoint (terrain or an object floor) so total vertical
// movement is minimized, anchored to the clicked start/goal heights - so the
// path climbs onto a structure and follows it to the goal floor.
PathResult findPath(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const sro::math::Vector3 &startAbsolute,
    const sro::math::Vector3 &goalAbsolute);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_PATH_H_
