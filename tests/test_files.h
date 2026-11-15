// Fixture file access for the host tests. DK_REPO_ROOT is set by the Makefile so tests run from any cwd.
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

inline std::string RepoPath(const std::string& rel) { return std::string(DK_REPO_ROOT) + "/" + rel; }

inline bool FileExists(const std::string& path) { return std::ifstream(path).good(); }

// Empty on failure.
inline std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
