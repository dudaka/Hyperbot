// navmesh_viz backend: loads the navmesh from an extracted sro-data Data
// directory and either dumps geometry/path to validate, or serves them over
// HTTP for the three.js client.
//
//   navmesh_viz dump  [dataPath] [radius] [out.json]
//   navmesh_viz serve [dataPath] [radius] [port]

#include "geometry.hpp"
#include "path.hpp"
#include "server.hpp"
#include "zones.hpp"

#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/navmesh/triangulation/navmeshTriangulation.hpp>
#include <silkroad_lib/pk2/navmeshParser.hpp>
#include <silkroad_lib/pk2/pk2ReaderModern.hpp>
#include <silkroad_lib/position_math.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace {

constexpr uint16_t kCenterRegionId = 0x5c87;

// The 9x9 scope (R=4) uses its own center, 2 regions south of kCenterRegionId,
// so it is not concentric with the smaller scopes (switching into it shifts the
// view south).
constexpr uint16_t k9x9CenterRegionId = 0x5a87;

// Region ids in a square ring of radius R around the center (R=0 -> just the
// center, R=1 -> 3x3, R=2 -> 5x5, R=3 -> 7x7, R=4 -> 9x9), clamped to valid
// sector bytes.
std::set<uint16_t> clusterRegionIds(uint16_t centerId, int radius) {
  const auto [centerX, centerZ] = sro::position_math::sectorsFromWorldRegionId(centerId);
  std::set<uint16_t> ids;
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = static_cast<int>(centerX) + dx;
      const int z = static_cast<int>(centerZ) + dz;
      if (x < 0 || x > 255 || z < 0 || z > 255) {
        continue;
      }
      ids.insert(sro::position_math::worldRegionIdFromSectors(
          static_cast<sro::Sector>(x), static_cast<sro::Sector>(z)));
    }
  }
  return ids;
}

// Runs a sample path query between two terrain points inside the center region
// to confirm the pathfinding + height reconstruction work end to end.
void runSamplePathQuery(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation) {
  const sro::navmesh::Region &center = navmesh.getRegionMap().at(kCenterRegionId);
  auto absoluteTerrainPoint = [&](float localX, float localZ) {
    const float height = center.getHeightAtPoint({localX, 0.0f, localZ});
    return triangulation.transformRegionPointIntoAbsolute(
        {localX, height, localZ}, kCenterRegionId);
  };
  const sro::math::Vector3 startAbs = absoluteTerrainPoint(480.0f, 480.0f);
  const sro::math::Vector3 goalAbs = absoluteTerrainPoint(1440.0f, 1440.0f);
  std::cout << "Path query S=(" << startAbs.x << "," << startAbs.y << ","
            << startAbs.z << ") G=(" << goalAbs.x << "," << goalAbs.y << ","
            << goalAbs.z << ")\n";
  const navmesh_viz::PathResult path =
      navmesh_viz::findPath(navmesh, triangulation, startAbs, goalAbs);
  if (!path.ok) {
    std::cout << "Path failed: " << path.error << "\n";
    return;
  }
  std::cout << "Path: " << path.waypoints.size() << " waypoint(s)\n";
  for (const auto &wp : path.waypoints) {
    std::cout << "  (" << wp.x << ", " << wp.y << ", " << wp.z << ")\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = (argc > 1) ? argv[1] : "dump";
  const std::filesystem::path dataPath =
      (argc > 2) ? std::filesystem::path(argv[2])
                 : std::filesystem::path("sro-data/Data");
  const int radius = (argc > 3) ? std::atoi(argv[3]) : 0;

  std::cout << "Loading navmesh from \"" << dataPath.string()
            << "\" (center=0x5c87, 9x9 center=0x5a87, ring radius=" << radius
            << ")\n";
  // Load every region up to the requested radius; in serve mode the client can
  // then request any smaller ring without a reload. Each radius picks its center
  // (the 9x9 recenters on 0x5a87), so the allow-list is the union of all rings.
  std::vector<std::set<uint16_t>> regionSetsByRadius;
  std::set<uint16_t> allRegions;
  for (int r = 0; r <= radius; ++r) {
    const uint16_t center = (r >= 4) ? k9x9CenterRegionId : kCenterRegionId;
    std::set<uint16_t> ring = clusterRegionIds(center, r);
    allRegions.insert(ring.begin(), ring.end());
    regionSetsByRadius.push_back(std::move(ring));
  }
  sro::pk2::Pk2ReaderModern reader{dataPath};
  sro::pk2::NavmeshParser parser{reader};
  parser.setRegionAllowList(allRegions);
  sro::navmesh::Navmesh navmesh = parser.parseNavmesh();

  const auto &regionMap = navmesh.getRegionMap();
  if (regionMap.find(kCenterRegionId) == regionMap.end()) {
    std::cerr << "Center region 0x5c87 not present\n";
    return 1;
  }
  if (radius >= 4 && regionMap.find(k9x9CenterRegionId) == regionMap.end()) {
    std::cerr << "9x9 center region 0x5a87 not present\n";
    return 1;
  }
  std::cout << "Parsed " << regionMap.size() << " region(s)\n";

  std::cout << "Building triangulation...\n";
  sro::navmesh::triangulation::NavmeshTriangulation triangulation{navmesh};
  std::cout << "Triangulation built\n";

  if (mode == "serve") {
    const int port = (argc > 4) ? std::atoi(argv[4]) : 5577;
    // The zone tables live alongside the navmesh data, under Media (sibling of Data).
    const std::filesystem::path textdataDir =
        dataPath.parent_path() / "Media" / "server_dep" / "silkroad" / "textdata";
    std::map<std::string, std::set<uint16_t>> zones =
        navmesh_viz::parseZoneTable(textdataDir);
    std::cout << "Parsed " << zones.size() << " zone(s) from \""
              << textdataDir.string() << "\"\n";
    navmesh_viz::runServer(navmesh, triangulation, regionSetsByRadius, reader, zones,
                           port);
    return 0;
  }

  // dump mode: write geometry to a file and run a sample path query.
  const std::string json = navmesh_viz::buildGeometryJson(
      navmesh, triangulation, regionSetsByRadius.back());
  const std::filesystem::path outPath =
      (argc > 4) ? std::filesystem::path(argv[4])
                 : std::filesystem::path("geometry.json");
  std::ofstream outFile(outPath, std::ios::binary);
  outFile << json;
  std::cout << "Wrote geometry to \"" << outPath.string() << "\" (" << json.size()
            << " bytes)\n";
  runSamplePathQuery(navmesh, triangulation);
  return 0;
}
