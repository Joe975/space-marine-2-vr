#pragma once
#include "PathText.h"
#include <fstream>
#include <memory>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

// spdlog's file sink takes a narrow name, which can't hold every Windows path. An ofstream on a
// filesystem::path opens the wide name.
class ModLogSink final:public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ModLogSink(const std::filesystem::path& path):m_file{path,std::ios::binary|std::ios::trunc} {}
protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        spdlog::memory_buf_t text;formatter_->format(message,text);
        m_file.write(text.data(),static_cast<std::streamsize>(text.size()));
    }
    void flush_() override {m_file.flush();}
private:
    std::ofstream m_file;
};

inline std::shared_ptr<spdlog::logger> open_mod_log(const std::string& name,const std::filesystem::path& path) {
    auto logger=std::make_shared<spdlog::logger>(name,std::make_shared<ModLogSink>(path));
    spdlog::initialize_logger(logger);
    return logger;
}
