#include <silkroad_lib/pk2/pk2ReaderModern.hpp>

#include <absl/log/log.h>
#include <absl/strings/str_format.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace sro::pk2 {

namespace {

// Reads an entire file from disk into a vector of the requested element type.
template <typename T>
std::vector<T> readWholeFile(const fs::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error(absl::StrFormat("Failed to open loose file \"%s\"", path.string()));
  }
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<T> data(static_cast<size_t>(size));
  if (size > 0 && !file.read(reinterpret_cast<char *>(data.data()), size)) {
    throw std::runtime_error(absl::StrFormat("Failed to read loose file \"%s\"", path.string()));
  }
  return data;
}

} // anonymous namespace

Pk2ReaderModern::Pk2ReaderModern(const fs::path &pk2Path) : pk2Path_(pk2Path) {
  if (!fs::exists(pk2Path_)) {
    throw std::runtime_error(absl::StrFormat("PK2 path \"%s\" does not exist", pk2Path_.string()));
  }
  if (fs::is_directory(pk2Path_)) {
    looseMode_ = true;
    return;
  }
  bool pk2OpenResult = pk2Reader_.Open(pk2Path_.string().c_str());
  if (!pk2OpenResult) {
    throw std::runtime_error(absl::StrFormat("PK2Reader failed to open pk2 file \"%s\" (PK2Reader: \"%s\")", pk2Path_.string(), pk2Reader_.GetError()));
  }
}

Pk2ReaderModern::~Pk2ReaderModern() {
  if (!looseMode_) {
    pk2Reader_.Close();
  }
}

fs::path Pk2ReaderModern::resolveLoosePath(const std::string &entryName) const {
  std::string relative = entryName;
  std::replace(relative.begin(), relative.end(), '\\', '/');
  return pk2Path_ / relative;
}

bool Pk2ReaderModern::hasEntry(const std::string &entryName) {
  if (looseMode_) {
    return fs::exists(resolveLoosePath(entryName));
  }
  std::unique_lock<std::mutex> lock(mutex_);
  PK2Entry entry = {0};
  return pk2Reader_.GetEntry(entryName.c_str(), entry);
}

PK2Entry Pk2ReaderModern::getEntry(const std::string &entryName) {
  if (looseMode_) {
    const fs::path resolved = resolveLoosePath(entryName);
    if (!fs::exists(resolved)) {
      throw std::runtime_error("Loose entry \""+entryName+"\" not found at \""+resolved.string()+"\"");
    }
    std::unique_lock<std::mutex> lock(mutex_);
    PK2Entry entry = {0};
    entry.type = 2;
    entry.size = static_cast<uint32_t>(fs::file_size(resolved));
    // Use position as an opaque handle into looseEntries_; the name field is too
    // short to hold long resource paths, so we do not rely on it.
    entry.position = ++looseCounter_;
    const std::string base = resolved.filename().string();
    std::strncpy(entry.name, base.c_str(), sizeof(entry.name) - 1);
    looseEntries_[entry.position] = resolved;
    return entry;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  PK2Entry entry = {0};
  bool result = pk2Reader_.GetEntry(entryName.c_str(), entry);
  if (!result) {
    throw std::runtime_error("PK2Reader failed to get entry \""+entryName+"\" (PK2Reader: \""+pk2Reader_.GetError()+"\")");
  }
  return entry;
}

std::vector<uint8_t> Pk2ReaderModern::getEntryData(PK2Entry &entry) { // TODO: This function should take a reference to a vector as input instead
  std::unique_lock<std::mutex> lock(mutex_);
  if (looseMode_) {
    auto it = looseEntries_.find(entry.position);
    if (it == looseEntries_.end()) {
      throw std::runtime_error("Loose entry handle not found");
    }
    return readWholeFile<uint8_t>(it->second);
  }
  std::vector<uint8_t> data;
  bool result = pk2Reader_.ExtractToMemory(entry, data);
  if (!result) {
    throw std::runtime_error("PK2Reader failed to extract data from entry \""+std::string(entry.name)+"\" (PK2Reader: \""+pk2Reader_.GetError()+"\")");
  }
  return data;
}

std::vector<char> Pk2ReaderModern::getEntryDataChar(PK2Entry &entry) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (looseMode_) {
    auto it = looseEntries_.find(entry.position);
    if (it == looseEntries_.end()) {
      throw std::runtime_error("Loose entry handle not found");
    }
    return readWholeFile<char>(it->second);
  }
  std::vector<char> data;
  bool result = pk2Reader_.ExtractToMemoryChar(entry, data);
  if (!result) {
    throw std::runtime_error("PK2Reader failed to extract data from entry \""+std::string(entry.name)+"\" (PK2Reader: \""+pk2Reader_.GetError()+"\")");
  }
  return data;
}

void Pk2ReaderModern::clearCache() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (looseMode_) {
    looseEntries_.clear();
    return;
  }
  pk2Reader_.ClearCache();
}


} // namespace sro::pk2