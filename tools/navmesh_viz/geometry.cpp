#include "geometry.hpp"

#include <silkroad_lib/math/vector3.hpp>

#include <array>
#include <ostream>
#include <sstream>
#include <vector>

namespace navmesh_viz {

namespace {

constexpr int kVerticesPerSide = 97; // 96 tiles + 1
constexpr int kTilesPerSide = 96;
constexpr float kTileSize = 20.0f; // units per tile; region = 1920 units

// Streams a flat float array as JSON, e.g. [1.5,2,3].
void writeFloatArray(std::ostream &out, const std::vector<float> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
}

void writeUintArray(std::ostream &out, const std::vector<uint32_t> &values) {
  out << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
}

// Appends a vertex (already in absolute coords) to a flat positions array.
void pushVertex(std::vector<float> &positions, const sro::math::Vector3 &v) {
  positions.push_back(v.x);
  positions.push_back(v.y);
  positions.push_back(v.z);
}

void writeTerrain(
    std::ostream &out, const sro::navmesh::Region &region,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation) {
  std::vector<float> positions;
  positions.reserve(kVerticesPerSide * kVerticesPerSide * 3);
  for (int zi = 0; zi < kVerticesPerSide; ++zi) {
    for (int xi = 0; xi < kVerticesPerSide; ++xi) {
      const sro::math::Vector3 local(xi * kTileSize,
                                     region.tileVertexHeights[zi][xi],
                                     zi * kTileSize);
      pushVertex(positions,
                 triangulation.transformRegionPointIntoAbsolute(local, region.id));
    }
  }

  std::vector<uint32_t> indices;
  std::vector<uint32_t> walkable; // one flag per cell, row-major (z, x)
  indices.reserve(kTilesPerSide * kTilesPerSide * 6);
  walkable.reserve(kTilesPerSide * kTilesPerSide);
  for (int zi = 0; zi < kTilesPerSide; ++zi) {
    for (int xi = 0; xi < kTilesPerSide; ++xi) {
      const uint32_t v00 = zi * kVerticesPerSide + xi;
      const uint32_t v01 = v00 + 1;
      const uint32_t v10 = v00 + kVerticesPerSide;
      const uint32_t v11 = v10 + 1;
      indices.insert(indices.end(), {v00, v10, v11, v00, v11, v01});
      walkable.push_back(region.enabledTiles[zi][xi] ? 1u : 0u);
    }
  }

  out << "\"terrain\":{\"positions\":";
  writeFloatArray(out, positions);
  out << ",\"indices\":";
  writeUintArray(out, indices);
  out << ",\"walkable\":";
  writeUintArray(out, walkable);
  out << '}';
}

// Returns false if the object could not be transformed/extracted.
bool writeObject(
    std::ostream &out, const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    uint16_t regionId, uint32_t instanceId) {
  sro::navmesh::ObjectResource resource;
  try {
    resource = navmesh.getTransformedObjectResourceForRegion(instanceId, regionId);
  } catch (...) {
    return false;
  }
  if (resource.cells.empty()) {
    return false;
  }

  std::vector<float> positions;
  positions.reserve(resource.vertices.size() * 3);
  for (const auto &vertex : resource.vertices) {
    pushVertex(positions,
               triangulation.transformRegionPointIntoAbsolute(vertex, regionId));
  }

  std::vector<uint32_t> indices;
  std::vector<uint32_t> areaIds; // one per triangle, to separate floors
  indices.reserve(resource.cells.size() * 3);
  areaIds.reserve(resource.cells.size());
  for (size_t i = 0; i < resource.cells.size(); ++i) {
    const auto &cell = resource.cells[i];
    indices.insert(indices.end(), {cell.vertex0, cell.vertex1, cell.vertex2});
    areaIds.push_back(i < resource.cellAreaIds.size() ? resource.cellAreaIds[i] : 0u);
  }

  // Outline (perimeter) edges with their raw flag, for navmesh inspection.
  // Flat triples: [srcVertexIndex, destVertexIndex, flag, ...].
  std::vector<uint32_t> outlineEdges;
  outlineEdges.reserve(resource.outlineEdges.size() * 3);
  for (const auto &edge : resource.outlineEdges) {
    outlineEdges.push_back(static_cast<uint32_t>(edge.srcVertex));
    outlineEdges.push_back(static_cast<uint32_t>(edge.destVertex));
    outlineEdges.push_back(static_cast<uint32_t>(edge.flag));
  }

  out << "{\"instanceId\":" << instanceId << ",\"positions\":";
  writeFloatArray(out, positions);
  out << ",\"indices\":";
  writeUintArray(out, indices);
  out << ",\"areaIds\":";
  writeUintArray(out, areaIds);
  out << ",\"outlineEdges\":";
  writeUintArray(out, outlineEdges);
  out << '}';
  return true;
}

} // namespace

std::string buildGeometryJson(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::set<uint16_t> &regionIds) {
  const auto &regionMap = navmesh.getRegionMap();
  std::ostringstream out;
  out << "{\"regions\":[";
  bool firstRegion = true;
  for (uint16_t regionId : regionIds) {
    auto it = regionMap.find(regionId);
    if (it == regionMap.end()) {
      continue;
    }
    const sro::navmesh::Region &region = it->second;
    if (!firstRegion) {
      out << ',';
    }
    firstRegion = false;

    out << "{\"id\":" << regionId << ',';
    writeTerrain(out, region, triangulation);
    out << ",\"objects\":[";
    bool firstObject = true;
    for (uint32_t instanceId : region.objectInstanceIds) {
      std::ostringstream objectOut;
      if (!writeObject(objectOut, navmesh, triangulation, regionId, instanceId)) {
        continue;
      }
      if (!firstObject) {
        out << ',';
      }
      firstObject = false;
      out << objectOut.str();
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

} // namespace navmesh_viz
