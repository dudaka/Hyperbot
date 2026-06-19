#ifndef SRO_PK2_NAVMESH_PARSER_H_
#define SRO_PK2_NAVMESH_PARSER_H_

#include <silkroad_lib/math/vector3.hpp>
#include <silkroad_lib/navmesh/navmesh.hpp>
#include <silkroad_lib/pk2/pk2ReaderModern.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace sro::pk2 {

struct MapInfo {
public:
  uint16_t mapWidth, mapHeight;
  std::array<uint8_t,8192> regionData;
};

struct ObjectFileInfo {
  bool flag;
  std::string filePath;
};

class NavmeshParser {
public:
  NavmeshParser(Pk2ReaderModern &pk2Reader);
  navmesh::Navmesh parseNavmesh();

  // When set, only these region ids are parsed (still subject to the map's
  // real enable bit). Overrides the built-in interesting-area parse-reduction
  // hack; lets a tool target a specific region set. Default keeps prior behavior.
  void setRegionAllowList(std::set<uint16_t> regions);

private:
  Pk2ReaderModern &pk2Reader_;
  std::map<int, ObjectFileInfo> objectFileInfoMap_;
  MapInfo mapInfo_;
  std::optional<std::set<uint16_t>> regionAllowList_;

  void buildObjectFileInfoMap();
  void parseMapInfo();
  bool regionIsEnabled(uint16_t regionId) const;
  void parseRegion(uint16_t regionId, navmesh::Navmesh &navmesh);
  void parseRegionObjectInstances(std::istringstream &navmeshData, navmesh::Region &region, navmesh::Navmesh &navmesh);
  void parseRegionObjectResources(navmesh::Region &region, navmesh::Navmesh &navmesh);
  void parseRegionCellQuads(std::istringstream &navmeshData, navmesh::Region &region, navmesh::Navmesh &navmesh);
  void parseRegionGlobalEdges(std::istringstream &navmeshData, navmesh::Region &region) const;
  void parseRegionInternalEdges(std::istringstream &navmeshData, navmesh::Region &region) const;
  void parseRegionTileMap(std::istringstream &navmeshData, navmesh::Region &region) const;
  void parseRegionHeightMap(std::istringstream &navmeshData, navmesh::Region &region) const;

  navmesh::ObjectResource parseObjectResource(const std::string &path);
  navmesh::ObjectResource parseCompoundResource(const std::string &path);
  navmesh::ObjectResource parseObjectBms(const std::string &path);
};

} // namespace sro::pk2

#endif // SRO_PK2_NAVMESH_PARSER_H_