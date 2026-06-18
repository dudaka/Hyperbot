cmake_minimum_required(VERSION 3.20)

# Standalone build for the navmesh visualization backend. It is intentionally
# decoupled from the root Hyperbot build: it needs only the navmesh path of
# silkroad_lib plus the Pathfinder submodule, so it avoids the bot, Qt, vcpkg,
# protobuf, gtest, gli and tracy. Abseil is found via CONFIG (e.g. Homebrew).
project(navmesh_viz LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

find_package(absl CONFIG REQUIRED)
find_package(Threads REQUIRED)

# cpp-httplib is header-only (e.g. Homebrew's /opt/homebrew/include/httplib.h).
find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)

# Pathfinder (Polyanya + vendored Triangle); built from the repo submodule.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pathfinder
                 ${CMAKE_BINARY_DIR}/pathfinder_build)

set(SILKROAD_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../silkroad_lib)

# Subset of silkroad_lib that the navmesh path needs (mirrors that target's
# CMakeLists, minus the media/icon sources that pull in gli).
set(SILKROAD_NAVMESH_SOURCES
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/blowfish.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/shared_io.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/position.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/position_math.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/math/vector3.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/math/matrix4x4.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/navmesh/navmesh.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/navmesh/triangulation/navmeshTriangulation.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/navmesh/triangulation/singleRegionNavmeshTriangulation.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/navmesh/triangulation/singleRegionNavmeshTriangulationState.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/pk2/navmeshParser.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/pk2/pk2ReaderModern.cpp
  ${SILKROAD_LIB_DIR}/src/silkroad_lib/pk2/pk2Reader.cpp
)

add_executable(navmesh_viz
  main.cpp geometry.cpp path.cpp server.cpp ${SILKROAD_NAVMESH_SOURCES})
target_include_directories(navmesh_viz PRIVATE
  ${SILKROAD_LIB_DIR}/src ${HTTPLIB_INCLUDE_DIR})
target_link_libraries(navmesh_viz PRIVATE
  Pathfinder absl::str_format absl::log Threads::Threads)
