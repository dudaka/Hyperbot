#include "zones.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace navmesh_viz {

namespace {

// Decodes a UTF-16 LE file to an ASCII-only string. The zone files only carry the
// numeric region id and the English name in fields we read, both ASCII; non-ASCII
// code units (Korean and other names we ignore) are dropped, while tabs and line
// breaks are preserved so the field/line structure survives.
std::string decodeUtf16LeAscii(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return {};
  }
  const std::vector<unsigned char> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string out;
  out.reserve(bytes.size() / 2);
  size_t i = 0;
  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    i = 2; // skip BOM
  }
  for (; i + 1 < bytes.size(); i += 2) {
    const unsigned int code = bytes[i] | (static_cast<unsigned int>(bytes[i + 1]) << 8);
    if (code == '\t' || code == '\n' || code == '\r' ||
        (code >= 0x20 && code <= 0x7E)) {
      out.push_back(static_cast<char>(code));
    }
  }
  return out;
}

// Splits a tab-delimited line into fields (empty fields preserved).
std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

void parseZoneFile(const std::filesystem::path &file,
                   std::map<std::string, std::set<uint16_t>> &zones) {
  const std::string text = decodeUtf16LeAscii(file);
  size_t lineStart = 0;
  while (lineStart <= text.size()) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = text.size();
    }
    std::string line = text.substr(lineStart, lineEnd - lineStart);
    lineStart = lineEnd + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (lineEnd == text.size()) {
        break;
      }
      continue;
    }
    const std::vector<std::string> fields = splitTabs(line);
    if (fields.size() < 10 || fields[9].empty()) {
      continue;
    }
    const std::string &idStr = fields[2];
    unsigned int regionId = 0;
    const auto result =
        std::from_chars(idStr.data(), idStr.data() + idStr.size(), regionId);
    if (result.ec != std::errc() || regionId > 0xFFFF) {
      continue;
    }
    zones[fields[9]].insert(static_cast<uint16_t>(regionId));
    if (lineEnd == text.size()) {
      break;
    }
  }
}

} // namespace

std::map<std::string, std::set<uint16_t>> parseZoneTable(
    const std::filesystem::path &textdataDir) {
  std::map<std::string, std::set<uint16_t>> zones;
  std::error_code ec;
  if (!std::filesystem::is_directory(textdataDir, ec)) {
    return zones;
  }
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(textdataDir)) {
    const std::filesystem::path &p = entry.path();
    const std::string name = p.filename().string();
    if (p.extension() == ".txt" && name.rfind("textzonename_", 0) == 0) {
      parseZoneFile(p, zones);
    }
  }
  return zones;
}

} // namespace navmesh_viz
