#ifndef SRO_PK2_READER_MODERN_H_
#define SRO_PK2_READER_MODERN_H_

#include <silkroad_lib/pk2/pk2.hpp>
#include <silkroad_lib/pk2/pk2Reader.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sro::pk2 {

namespace fs = std::filesystem;

// Reads entries from a Silkroad .pk2 archive. If constructed with a directory
// path instead of a .pk2 file, it serves loose (already-extracted) files from
// that directory, mapping archive entry names like "navmesh\\foo.nvm" to
// "<dir>/navmesh/foo.nvm". The public interface is identical in both modes.
class Pk2ReaderModern {
public:
  Pk2ReaderModern(const fs::path &pk2Path);
  ~Pk2ReaderModern();
  bool hasEntry(const std::string &entryName);
  PK2Entry getEntry(const std::string &entryName);
  std::vector<uint8_t> getEntryData(PK2Entry &entry);
  std::vector<char> getEntryDataChar(PK2Entry &entry);
  void clearCache();
private:
  fs::path resolveLoosePath(const std::string &entryName) const;

  std::mutex mutex_;
  fs::path pk2Path_;
  PK2Reader pk2Reader_;
  bool looseMode_{false};
  int64_t looseCounter_{0};
  std::unordered_map<int64_t, fs::path> looseEntries_;
};

} // namespace sro::pk2

#endif // SRO_PK2_READER_MODERN_H_