#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>

// Whether a polled settings or control file changed since the last poll: its last-write time and size,
// read with one metadata call. The render thread used to open and parse about a dozen such files every
// second whether or not they changed (MOD_REVIEW_2026-10-29). The first poll always reports a change, so
// the file is loaded once; a file that appears, disappears or is rewritten reports one change each time.
class FileStamp {
public:
    bool changed(const std::filesystem::path& path) {
        std::optional<Stamp> now;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))
            now=Stamp{(uint64_t(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime,(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow};
        const bool different=!m_polled || now!=m_last;
        m_polled=true;m_last=now;
        return different;
    }
    void forget() { m_polled=false;m_last.reset(); } // The next poll loads the file again.
private:
    struct Stamp { uint64_t written{},size{}; bool operator==(const Stamp&) const = default; };
    bool m_polled{};
    std::optional<Stamp> m_last;
};
