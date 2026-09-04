#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

// CTest runs cases in separate processes. A fixed name lets one case delete
// another case's files. Reserve a fresh directory atomically; never reclaim an
// existing path. Callers own and remove only the directory returned here.
inline std::filesystem::path CreateUnitTestDirectory(const std::string& tag) {
  auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
  // Windows TEMP commonly ends in a separator, while parent_path() does not.
  if (parent.has_relative_path() && parent.filename().empty()) {
    parent = parent.parent_path();
  }
  std::random_device random;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    const auto candidate = parent / (tag + "-" + std::to_string(random()) +
                                     "-" + std::to_string(random()));
    if (candidate.parent_path() != parent) {
      throw std::runtime_error("test directory tag must be a basename");
    }
    if (std::filesystem::create_directory(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("could not reserve a unique test directory");
}
