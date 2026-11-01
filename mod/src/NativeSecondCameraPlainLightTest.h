#pragma once
#include "NativeOrdinaryShadowCache.h"
#include <MinHook.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

// Candidate fix for the second-eye light flicker (docs/RIGHT_EYE_FLICKER_2026-10-28.md), off by
// default. Each view prepares an occlusion query per light (0x1efae60(lm, light, cam), one 0x278-byte
// record appended to lm+0x9b9a8+cam*0x10). Record byte +0x4f picks the test: set when the light is
// clean in the far-light shadow cache (0x1e8a9b0), the light is tested against its cached shadow map
// instead of the plain proxy. The cache is ranked from one camera and re-rendered on age timers, so
// for camera 1 a light flips between the two tests in a fixed cycle at a still view, and the cached
// test fails for camera 1's view (build/flicker_re/NOTES.md). Turning the whole cache off stopped the
// slot-1 flicker. With "plain" in the control file, camera 1's records always take the plain test.
// Addresses: 0x1efae60 in the reference build, 0x1efb1a0 in 0x6ad10fcb (found by its first bytes in
// build/gamebin; not in GameBuildTable.inc); both checked against the same 16 prologue bytes.
class NativeSecondCameraPlainLightTest {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uint32_t,uintptr_t);
    enum class Mode {native,plain};
    static constexpr uint32_t REFERENCE_BUILD=0x6ac3e716,PATCHED_BUILD=0x6ad10fcb;
    static constexpr sha256::Digest PROLOGUE=sha256::parse("3f8510cf75e6358b11eb586b66d90ccf5031e986abc4cf4636db267aaedea0dc");  // its first 16 bytes
    static constexpr size_t LISTS=0x9b9a8,LIST=0x10,RECORD=0x278,TEST_BYTE=0x4f;
    static uintptr_t prepare_rva(uint32_t timestamp){return timestamp==REFERENCE_BUILD?0x1efae60:timestamp==PATCHED_BUILD?0x1efb1a0:0;}
    static std::optional<Mode> parse(std::string_view line){
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        if(line=="native")return Mode::native;
        if(line=="plain")return Mode::plain;
        return std::nullopt;
    }
    // The record the call just appended, for camera `cam`'s list: list array + (count-1)*0x278.
    // Returns 0 for an implausible list.
    static uintptr_t last_record(uintptr_t array,int32_t count){
        if(!array || count<1 || count>4096)return 0;
        return array+size_t(count-1)*RECORD;
    }
    enum class Change {none,cleared,already_plain,refused};
    // The decision on the appended record's test byte (pure; 1 = cached test, 0 = plain).
    static Change decide(Mode mode,uint32_t cam,bool stereo,uintptr_t record,std::optional<uint8_t> test){
        if(mode!=Mode::plain || cam!=1 || !stereo)return Change::none;
        if(!record || !test || *test>1)return Change::refused;
        return *test?Change::cleared:Change::already_plain;
    }
    static NativeSecondCameraPlainLightTest& get(){static auto* p=new NativeSecondCameraPlainLightTest;return *p;}
    bool install_in_game(uintptr_t base,uint32_t timestamp,const std::filesystem::path& path){
        const auto rva=prepare_rva(timestamp);std::array<uint8_t,16> bytes{};
        if(m_native || !rva || !path.is_absolute() || !ordinary_scene_scope::read(base+rva,bytes) || sha256::hash(bytes)!=PROLOGUE)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        auto* target=reinterpret_cast<void*>(base+rva);
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_path=path;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool installed()const{return m_native!=nullptr;}
    // calls, camera-1 calls in stereo while plain, cleared, already plain, refused
    std::array<uint64_t,5> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
    Mode mode()const{std::lock_guard lock{m_mutex};return m_mode;}
private:
    static bool stereo(){for(const auto& s:NativeCameraEntries::get().status())if(s.valid && NativeOrdinaryShadowCache::pair_for_view(s.list+8+0xa60))return true;return false;}
    static void on_call(uintptr_t lm,uintptr_t light,uint32_t cam,uintptr_t fourth){
        auto& self=get();
        self.m_native(lm,light,cam,fourth);
        try{
            std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            const auto now=std::chrono::steady_clock::now();
            if(now>=self.m_next){self.m_next=now+std::chrono::seconds(1);std::ifstream file{self.m_path};std::string line;if(std::getline(file,line))if(const auto m=parse(line))self.m_mode=*m;}
            if(self.m_mode!=Mode::plain || cam!=1 || !lm)return;
            if(now>=self.m_stereo_next){self.m_stereo_next=now+std::chrono::milliseconds(20);self.m_stereo=stereo();}  // ~800 calls a frame
            const bool is_stereo=self.m_stereo;if(!is_stereo)return;
            ++self.m_counts[1];
            uintptr_t array{};int32_t count{};std::optional<uint8_t> test;uintptr_t record{};
            if(ordinary_scene_scope::read(lm+LISTS+cam*LIST,array) && ordinary_scene_scope::read(lm+LISTS+cam*LIST+8,count))record=last_record(array,count);
            if(record){uint8_t v{};if(ordinary_scene_scope::read(record+TEST_BYTE,v))test=v;}
            switch(decide(self.m_mode,cam,is_stereo,record,test)){
                case Change::cleared:{SIZE_T n{};const uint8_t zero=0;
                    if(WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(record+TEST_BYTE),&zero,1,&n) && n==1)++self.m_counts[2];else ++self.m_counts[4];break;}
                case Change::already_plain:++self.m_counts[3];break;
                case Change::refused:++self.m_counts[4];break;
                default:break;
            }
        }catch(...){}
    }
    Native m_native{};std::filesystem::path m_path;std::chrono::steady_clock::time_point m_next{},m_stereo_next{};Mode m_mode{Mode::native};bool m_stereo{};
    mutable std::mutex m_mutex;std::array<uint64_t,5> m_counts{};
};
