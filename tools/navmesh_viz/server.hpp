#ifndef NAVMESH_VIZ_SERVER_H_
#define NAVMESH_VIZ_SERVER_H_

#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/navmesh/triangulation/navmeshTriangulation.hpp>

#include <cstdint>
#include <set>
#include <vector>

namespace navmesh_viz {

// Serves geometry at GET /geometry?r=N (the region set at ring radius N, indexing
// regionSetsByRadius) and Polyanya path queries at GET /path?sx&sy&sz&gx&gy&gz
// (absolute coords). Pathfinding always spans all loaded regions. Blocks until
// interrupted.
void runServer(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::vector<std::set<uint16_t>> &regionSetsByRadius, int port);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_SERVER_H_
