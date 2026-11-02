#pragma once
#include <filesystem>
#include <string>

// A path as UTF-8 for the log. path::string() converts to the system code page and throws on a
// character outside it: a Cyrillic user folder on a Western-locale PC ended the game at launch.
inline std::string path_text(const std::filesystem::path& path) {
    const auto text=path.u8string();
    return {reinterpret_cast<const char*>(text.data()),text.size()};
}
