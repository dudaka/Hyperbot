#include "server.hpp"

#include "geometry.hpp"
#include "path.hpp"

#include <silkroad_lib/math/vector3.hpp>

#include <httplib.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
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
    const std::vector<std::set<uint16_t>> &regionSetsByRadius, int port) {
  // Geometry per ring radius is fixed, so build and cache each once up front.
  std::vector<std::string> geometryByRadius;
  for (size_t r = 0; r < regionSetsByRadius.size(); ++r) {
    geometryByRadius.push_back(
        buildGeometryJson(navmesh, triangulation, regionSetsByRadius[r]));
    std::cout << "Cached geometry r=" << r << " ("
              << geometryByRadius[r].size() << " bytes)\n";
  }
  const int maxRadius = static_cast<int>(geometryByRadius.size()) - 1;

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
    int r = maxRadius;
    if (req.has_param("r")) {
      r = std::atoi(req.get_param_value("r").c_str());
    }
    r = std::max(0, std::min(maxRadius, r));
    res.set_content(geometryByRadius[r], "application/json");
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
      const PathResult path = findPath(navmesh, triangulation, start, goal);
      res.set_content(pathResultToJson(path), "application/json");
    } catch (const std::exception &ex) {
      res.status = 400;
      res.set_content(std::string("{\"ok\":false,\"error\":\"") + ex.what() + "\"}",
                      "application/json");
    }
  });

  std::cout << "Serving on http://0.0.0.0:" << port
            << "  (GET /geometry, GET /path?sx&sy&sz&gx&gy&gz)\n";
  if (!server.listen("0.0.0.0", port)) {
    std::cerr << "Failed to bind port " << port << "\n";
    std::exit(1);
  }
}

} // namespace navmesh_viz
