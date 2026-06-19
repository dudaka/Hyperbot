#ifndef NAVMESH_VIZ_SERVER_H_
#define NAVMESH_VIZ_SERVER_H_

#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/navmesh/triangulation/navmeshTriangulation.hpp>
#include <silkroad_lib/pk2/pk2ReaderModern.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace navmesh_viz {

// Serves geometry at GET /geometry?r=N (the radius ring) or ?zone=NAME (a named
// zone, built on demand from `zones` via `reader` and cached), the zone list at GET
// /zones, and Polyanya path queries at GET /path?sx&sy&sz&gx&gy&gz[&zone=NAME]
// (absolute coords; a zone query runs within that zone's triangulation, otherwise
// the radius triangulation). Blocks until interrupted.
void runServer(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::vector<std::set<uint16_t>> &regionSetsByRadius,
    sro::pk2::Pk2ReaderModern &reader,
    const std::map<std::string, std::set<uint16_t>> &zones, int port);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_SERVER_H_
