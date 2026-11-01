#pragma once
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
namespace replay_extract {
using Json=nlohmann::json;namespace fs=std::filesystem;
inline void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
constexpr uint64_t MAX_TOTAL=32ULL*1024*1024,MAX_BUFFER=65536;constexpr size_t MAX_FILES=256;
struct Options {std::string command;fs::path capture,output;uint32_t event{};uint64_t bytes=4096;bool postvs{},weak{},preview{};uint32_t preview_slot{};uint32_t first{},last{},max_events=128;};
inline uint64_t number(const std::string& text){uint64_t v{};const auto r=std::from_chars(text.data(),text.data()+text.size(),v);check(!text.empty() && r.ec==std::errc{} && r.ptr==text.data()+text.size(),"invalid unsigned decimal argument");return v;}
inline Options parse(int argc,const char* const* argv){
    check(argc>=4,"usage: inventory CAPTURE OUTDIR | event CAPTURE OUTDIR EID [--postvs] [--max-bytes N]");Options o;o.command=argv[1];o.capture=argv[2];o.output=argv[3];
    check(o.command=="inventory" || o.command=="event" || o.command=="scan","unsupported command");check(o.capture.is_absolute() && o.output.is_absolute(),"absolute capture/output paths required");
    int next=4;if(o.command=="event"){check(argc>4,"exact positive action EID required");const auto e=number(argv[4]);check(e && e<=UINT32_MAX,"action EID outside uint32 range");o.event=uint32_t(e);next=5;}
    if(o.command=="scan"){check(argc>5,"scan requires exact first/last positive EIDs");const auto first=number(argv[4]),last=number(argv[5]);check(first && first<=last && last<=UINT32_MAX,"invalid scan EID range");o.first=uint32_t(first);o.last=uint32_t(last);next=6;}
    bool seen_events=false;
    bool seen_bytes=false;for(int i=next;i<argc;++i){const std::string a=argv[i];if(a=="--postvs"){check(o.command=="event" && !o.postvs,"duplicate/inapplicable postvs");o.postvs=true;}
        else if(a=="--preview-slot"){check(o.command=="event" && !o.preview && i+1<argc,"invalid preview-slot option");const auto slot=number(argv[++i]);check(slot<8,"preview slot must be0..7");o.preview=true;o.preview_slot=uint32_t(slot);}
        else if(a=="--weak"){check(o.command=="scan" && !o.weak,"duplicate/inapplicable weak");o.weak=true;}
        else if(a=="--max-events"){check(o.command=="scan" && !seen_events && i+1<argc,"invalid max-events option");seen_events=true;const auto n=number(argv[++i]);check(n && n<=256,"scan event cap must be1..256");o.max_events=uint32_t(n);}
        else if(a=="--max-bytes"){check((o.command=="event" || o.command=="scan") && !seen_bytes && i+1<argc,"invalid max-bytes option");seen_bytes=true;o.bytes=number(argv[++i]);check(o.bytes && o.bytes<=MAX_BUFFER,"max-bytes must be1..65536");}else check(false,"unknown argument");}return o;
}
inline uint64_t bounded_span(uint64_t offset,uint64_t declared,uint64_t length,uint64_t cap){check(cap && cap<=MAX_BUFFER,"invalid buffer read cap");check(offset<=length,"buffer offset outside resource");check(declared<=length-offset,"buffer view outside resource");return std::min(declared,cap);}
struct Deadline {std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();void check_time()const{check(std::chrono::steady_clock::now()-start<std::chrono::seconds(120),"extractor120s inter-call deadline exceeded");}};
inline Json format(uint32_t type,uint32_t count,uint32_t width,uint32_t component,bool bgra,bool srgb){return {{"type_enum",type},{"component_count",count},{"component_byte_width",width},{"component_type_enum",component},{"bgra",bgra},{"srgb",srgb}};}
struct Budget {
    uint64_t total{},text_bytes{};size_t files{};
    void reserve(uint64_t n){check(files<MAX_FILES && n<=MAX_TOTAL-total,"export file/byte budget exceeded");++files;total+=n;}
    std::string text(const std::string& s){check(s.size()<=4096 && s.size()<=4*1024*1024-text_bytes,"metadata text budget exceeded");text_bytes+=s.size();return s;}
};
struct Output {
    fs::path path;Budget budget;
    explicit Output(const fs::path& p):path(p){check(p.is_absolute() && !fs::exists(p) && fs::is_directory(p.parent_path()),"fresh absolute output directory with existing parent required");check(fs::create_directory(p),"output directory creation failed");}
    void bytes(const std::string& name,const void* data,size_t n){check(name.find_first_of("/\\:")==std::string::npos && !name.empty(),"unsafe export name");const auto file=path/name;check(!fs::exists(file),"export overwrite refused");budget.reserve(n);std::ofstream out{file,std::ios::binary};check(bool(out),"export file open failed");out.write(static_cast<const char*>(data),std::streamsize(n));out.close();check(bool(out),"export file write failed");}
    void json(const std::string& name,const Json& value){const auto s=value.dump(2);bytes(name,s.data(),s.size());}
};
template<class T> struct Shutdown {void operator()(T* p)const{if(p)p->Shutdown();}};
template<class T> using Owned=std::unique_ptr<T,Shutdown<T>>;
}
