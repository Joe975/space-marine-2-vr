#include "ModLog.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
// Cyrillic, Japanese and Hangul: no single-byte Western code page holds them all, so
// path::string() throws here on any PC.
const std::wstring NAME=L"sm2vr_\u0416\u30de\uD55C";
void text() {
    const std::filesystem::path path=std::filesystem::path{L"C:\\Users"}/NAME/"x.log";
    require(path_text(path)=="C:\\Users\\sm2vr_\xD0\x96\xE3\x83\x9E\xED\x95\x9C\\x.log","a path reaches the log as UTF-8");
    require(path_text("plain.txt")=="plain.txt","plain paths are unchanged");
}
void file() {
    const auto folder=std::filesystem::temp_directory_path()/NAME;
    std::filesystem::remove_all(folder);std::filesystem::create_directories(folder);
    const auto path=folder/"sm2vr.log";
    {std::ofstream{path}<<"last run";}
    {
        auto logger=open_mod_log("mod_log_test",path);
        logger->set_pattern("%v");
        logger->warn("folder {}",path_text(folder));
        logger->flush();
        spdlog::drop("mod_log_test");
    }
    std::ifstream input{path};std::stringstream written;written<<input.rdbuf();input.close();
    require(written.str().starts_with("folder "),"the log opens in a folder the code page can't hold, and starts empty");
    require(written.str().find("sm2vr_\xD0\x96")!=std::string::npos,"the name is readable in the log");
    std::filesystem::remove_all(folder);
}
}
int main() {try {text();file();std::cout<<"mod log paths passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
