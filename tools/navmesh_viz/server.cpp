#include "server.hpp"

#include "geometry.hpp"
#include "path.hpp"

#include <silkroad_lib/math/vector3.hpp>
#include <silkroad_lib/pk2/navmeshParser.hpp>

#include <httplib.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace navmesh_viz {

namespace {

// Reads a required float query parameter, throwing if absent/unparseable.
float requireFloatParam(const httplib::Request &req, const char *name) {
  if (!req.has_param(name)) {
    throw std::runtime_error(std::string("missing query parameter: ") + name);
  }
  return std::stof(req.get_param_value(name));
}

// Escapes a string for embedding in a JSON string literal (zone names, errors).
std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// An on-demand-built, cached navmesh + triangulation + geometry for one zone.
struct ZoneView {
  std::unique_ptr<sro::navmesh::Navmesh> navmesh;
  std::unique_ptr<sro::navmesh::triangulation::NavmeshTriangulation> triangulation;
  std::string geometry;
};

std::string pathResultToJson(const PathResult &path) {
  std::ostringstream out;
  if (!path.ok) {
    out << "{\"ok\":false,\"error\":\"" << path.error << "\"}";
    return out.str();
  }
  out << "{\"ok\":true,\"waypoints\":[";
  for (size_t i = 0; i < path.waypoints.size(); ++i) {
    const auto &wp = path.waypoints[i];
    if (i != 0) {
      out << ',';
    }
    out << '[' << wp.x << ',' << wp.y << ',' << wp.z << ']';
  }
  out << "]}";
  return out.str();
}

} // namespace

void runServer(
    const sro::navmesh::Navmesh &navmesh,
    const sro::navmesh::triangulation::NavmeshTriangulation &triangulation,
    const std::vector<std::set<uint16_t>> &regionSetsByRadius,
    sro::pk2::Pk2ReaderModern &reader,
    const std::map<std::string, std::set<uint16_t>> &zones, int port) {
  // Geometry per ring radius is fixed, so build and cache each once up front.
  std::vector<std::string> geometryByRadius;
  for (size_t r = 0; r < regionSetsByRadius.size(); ++r) {
    geometryByRadius.push_back(
        buildGeometryJson(navmesh, triangulation, regionSetsByRadius[r]));
    std::cout << "Cached geometry r=" << r << " ("
              << geometryByRadius[r].size() << " bytes)\n";
  }
  const int maxRadius = static_cast<int>(geometryByRadius.size()) - 1;

  // The zone list is fixed; build its JSON once.
  std::string zonesJson;
  {
    std::ostringstream z;
    z << '[';
    bool first = true;
    for (const auto &[name, regions] : zones) {
      if (!first) {
        z << ',';
      }
      first = false;
      z << "{\"name\":\"" << jsonEscape(name) << "\",\"regions\":" << regions.size()
        << '}';
    }
    z << ']';
    zonesJson = z.str();
  }

  // Zones are built lazily (a separate navmesh + triangulation per zone) and cached
  // forever. The map is node-stable, so a returned pointer stays valid after the
  // lock is released; the mutex serializes builds and cache access.
  std::map<std::string, ZoneView> zoneCache;
  std::mutex zoneMutex;
  auto getOrBuildZone = [&](const std::string &name) -> ZoneView * {
    auto zit = zones.find(name);
    if (zit == zones.end()) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(zoneMutex);
    auto cit = zoneCache.find(name);
    if (cit != zoneCache.end()) {
      return &cit->second;
    }
    const std::set<uint16_t> &regions = zit->second;
    sro::pk2::NavmeshParser parser{reader};
    parser.setRegionAllowList(regions);
    ZoneView view;
    view.navmesh = std::make_unique<sro::navmesh::Navmesh>();
    *view.navmesh = parser.parseNavmesh(); // move-assign (avoids a deep copy)
    view.triangulation =
        std::make_unique<sro::navmesh::triangulation::NavmeshTriangulation>(
            *view.navmesh);
    view.geometry =
        buildGeometryJson(*view.navmesh, *view.triangulation, regions);
    std::cout << "Built zone \"" << name << "\" (" << regions.size()
              << " region(s), " << view.geometry.size() << " bytes)\n";
    return &zoneCache.emplace(name, std::move(view)).first->second;
  };

  httplib::Server server;

  // Permissive CORS so the Vite dev server (a different origin) can call us.
  server.set_post_routing_handler(
      [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
      });
  server.Options(".*", [](const httplib::Request &, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "*");
  });

  server.Get("/geometry", [&](const httplib::Request &req, httplib::Response &res) {
    if (req.has_param("zone")) {
      const std::string name = req.get_param_value("zone");
      try {
        const ZoneView *view = getOrBuildZone(name);
        if (view == nullptr) {
          res.status = 404;
          res.set_content("{\"error\":\"unknown zone: " + jsonEscape(name) + "\"}",
                          "application/json");
          return;
        }
        res.set_content(view->geometry, "application/json");
      } catch (const std::exception &ex) {
        res.status = 500;
        res.set_content(std::string("{\"error\":\"") + jsonEscape(ex.what()) + "\"}",
                        "application/json");
      }
      return;
    }
    int r = maxRadius;
    if (req.has_param("r")) {
      r = std::atoi(req.get_param_value("r").c_str());
    }
    r = std::max(0, std::min(maxRadius, r));
    res.set_content(geometryByRadius[r], "application/json");
  });
  // The list of named zones (English name + region count), sorted by name.
  server.Get("/zones", [&](const httplib::Request &, httplib::Response &res) {
    res.set_content(zonesJson, "application/json");
  });
  // Report the max ring radius the client can request, plus the pathfinder's
  // agent (collision) radius so the client can draw the footprint to scale.
  server.Get("/info", [&](const httplib::Request &, httplib::Response &res) {
    res.set_content("{\"maxRadius\":" + std::to_string(maxRadius) +
                        ",\"agentRadius\":" + std::to_string(kAgentRadius) + "}",
                    "application/json");
  });

  server.Get("/path", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      const sro::math::Vector3 start(requireFloatParam(req, "sx"),
                                     requireFloatParam(req, "sy"),
                                     requireFloatParam(req, "sz"));
      const sro::math::Vector3 goal(requireFloatParam(req, "gx"),
                                    requireFloatParam(req, "gy"),
                                    requireFloatParam(req, "gz"));
      // A zone query runs within that zone's triangulation; otherwise the radius one.
      const sro::navmesh::Navmesh *nav = &navmesh;
      const sro::navmesh::triangulation::NavmeshTriangulation *tri = &triangulation;
      if (req.has_param("zone")) {
        const ZoneView *view = getOrBuildZone(req.get_param_value("zone"));
        if (view == nullptr) {
          res.status = 404;
          res.set_content("{\"ok\":false,\"error\":\"unknown zone\"}",
                          "application/json");
          return;
        }
        nav = view->navmesh.get();
        tri = view->triangulation.get();
      }
      const PathResult path = findPath(*nav, *tri, start, goal);
      res.set_content(pathResultToJson(path), "application/json");
    } catch (const std::exception &ex) {
      res.status = 400;
      res.set_content(std::string("{\"ok\":false,\"error\":\"") + ex.what() + "\"}",
                      "application/json");
    }
  });

  std::cout << "Serving on http://0.0.0.0:" << port
            << "  (GET /geometry[?r=N|?zone=NAME], GET /zones, "
               "GET /path?sx&sy&sz&gx&gy&gz[&zone=NAME])\n";
  if (!server.listen("0.0.0.0", port)) {
    std::cerr << "Failed to bind port " << port << "\n";
    std::exit(1);
  }
}

} // namespace navmesh_viz
