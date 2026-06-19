#ifndef NAVMESH_VIZ_ZONES_H_
#define NAVMESH_VIZ_ZONES_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace navmesh_viz {

// Parses the Silkroad textzonename_*.txt files in textdataDir into a mapping from
// English zone name to the set of region ids that belong to it. A zone is the set
// of regions sharing one English name. The files are UTF-16 LE, tab-delimited;
// field index 2 is the decimal region id and field index 9 is the English zone
// name. Non-ASCII fields (e.g. Korean names) are not needed and are dropped during
// decode. Returns an empty map if the directory is absent.
std::map<std::string, std::set<uint16_t>> parseZoneTable(
    const std::filesystem::path &textdataDir);

} // namespace navmesh_viz

#endif // NAVMESH_VIZ_ZONES_H_
