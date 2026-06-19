#ifndef NAVMESH_VIZ_GEOMETRY_H_
#define NAVMESH_VIZ_GEOMETRY_H_

#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/navmesh/triangulation/navmeshTriangulation.hpp>

#include <cstdint>
#include <set>
#include <string>

namespace navmesh_viz {

// Builds a JSON document describing the terrain + BMS object meshes for the
// given regions, in the absolute pathfinder plane (Y-up). The same absolute
// frame is used for path queries, so client picks line up with the pathfinder.
std::string buildGeometryJson(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::set<uint16_t> &regionIds);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_GEOMETRY_H_
