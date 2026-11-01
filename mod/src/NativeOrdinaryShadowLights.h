#pragma once
#include "NativeOrdinaryShadowCache.h"
#include "GameBuild.h"
#include <algorithm>
#include <cstring>
#include <optional>

// Both eyes shadow the same local lights (docs/VIEWER_FLICKER_2026-10-27.md).
// Each view picks its own shadowed local lights (light job 0x1efa550: frustum, distance to the
// view, delayed occlusion), and shadow preparation 0x1f891f0(owner, slot, ...) builds that view's
// records from its list. The rendered shadows share one atlas whose rows are the record index, with
// no per-view term. Far apart eyes (camera mode at tabletop scale) often disagree near a cut-off,
// so the second eye loses or gains a light's shadow for a few frames: one-frame light flips seen
// in that eye only. Before slot 1's preparation, give slot 1 slot 0's list.
// List block at [0x4faf810]: slot s holds int index[16] at +0x50+s*0x44 and int count at +0x90+s*0x44.
class NativeOrdinaryShadowLights {
public:
    using Native=void(*)(uintptr_t,int32_t,uint8_t,uint8_t);
    using Pair=NativeCameraEntries::OrdinaryPair;
    static constexpr size_t LIST=0x44,FIRST=0x50;
    using Lists=std::array<uint8_t,2*LIST>;  // both slots' lists, read from FIRST
    enum class Result {refused,same,copied};
    static NativeOrdinaryShadowLights& get(){static auto* p=new NativeOrdinaryShadowLights;return *p;}
    // On in every refresh mode except the A/B mode that keeps each eye's own selection.
    static bool shares(const std::string& mode){return NativeOrdinaryShadowCache::refreshes(mode) && mode!="light_union_shadow_refresh_native_shadow_lights";}
    // Copies slot 0's list over slot 1's. Refuses a list that isn't a sane count of light indices.
    static Result align(Lists& lists){
        int32_t count{};std::memcpy(&count,lists.data()+0x40,4);
        if(count<0 || count>16)return Result::refused;
        for(int32_t i=0;i<count;++i){int32_t index{};std::memcpy(&index,lists.data()+4*i,4);if(index<0)return Result::refused;}
        if(std::equal(lists.begin(),lists.begin()+LIST,lists.begin()+LIST))return Result::same;
        std::copy(lists.begin(),lists.begin()+LIST,lists.begin()+LIST);return Result::copied;
    }
    using Provider=std::function<bool()>;
    using Memory=std::function<bool(uintptr_t,Lists&,bool)>;  // read (false) or write (true) both lists
    bool install(void* target,Provider stereo,std::function<uintptr_t()> owner,std::function<uintptr_t()> lists,Memory memory,std::function<bool()> enabled,uintptr_t caller=0){
        if(m_native || !target || !stereo || !owner || !lists || !memory || !enabled)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_stereo=std::move(stereo);m_owner=std::move(owner);m_lists=std::move(lists);m_memory=std::move(memory);m_enabled=std::move(enabled);m_caller=caller;
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1f891f0,15,"ce5eb6c11aa0d49814935d969bd707f0e610be16d1782a0658054749f0b5dd20");
        constexpr game_build::Code call=game_build::hashed(0x1f86a60,5,"ade705984673d161aa8641a3b84cf68727b863298d95b4c1922814c9aea6f820");
        std::array<uint8_t,15> bytes{};std::array<uint8_t,5> site{};
        if(!path.is_absolute() || !game_build::rva(0x1f891f0) || !game_build::rva(0x1f86a60) || !game_build::rva(0x1f86a65) || !game_build::rva(0x4faf810) || !game_build::rva(0x4faef88) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f891f0),bytes) || !game_build::code(0x1f891f0,bytes,expected) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f86a60),site) || !game_build::code(0x1f86a60,site,call))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f891f0)),
            []{for(const auto& s:NativeCameraEntries::get().status())if(s.valid && NativeOrdinaryShadowCache::pair_for_view(s.list+8+0xa60))return true;return false;},
            [base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4faef88),owner);return owner;},
            [base]{uintptr_t lists{};ordinary_scene_scope::read(base+game_build::rva(0x4faf810),lists);return lists;},
            [](uintptr_t lists,Lists& value,bool write){SIZE_T n{};const auto at=reinterpret_cast<void*>(lists+FIRST);
                return write?WriteProcessMemory(GetCurrentProcess(),at,value.data(),value.size(),&n) && n==value.size():ReadProcessMemory(GetCurrentProcess(),at,value.data(),value.size(),&n) && n==value.size();},
            [path,next=std::chrono::steady_clock::time_point{},enabled=false]()mutable{
                const auto now=std::chrono::steady_clock::now();if(now<next)return enabled;next=now+std::chrono::seconds(1);
                std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();enabled=shares(line);}return enabled;
            },base+game_build::rva(0x1f86a65));
    }
    // calls, slot-1 calls from the verified site in stereo, lists already equal, copied, refused
    bool installed()const{return m_native!=nullptr;}
    std::array<uint64_t,5> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
private:
    static void on_call(uintptr_t owner,int32_t slot,uint8_t a,uint8_t b){
        auto& self=get();
        try{
            std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            if(slot==1 && (!self.m_caller || reinterpret_cast<uintptr_t>(_ReturnAddress())==self.m_caller) && owner && owner==self.m_owner() && self.m_enabled() && self.m_stereo()){
                ++self.m_counts[1];
                const auto lists=self.m_lists();Lists value{};
                if(!lists || !self.m_memory(lists,value,false))++self.m_counts[4];
                else switch(align(value)){
                    case Result::same:++self.m_counts[2];break;
                    case Result::copied:if(self.m_memory(lists,value,true))++self.m_counts[3];else ++self.m_counts[4];break;
                    default:++self.m_counts[4];
                }
            }
        }catch(...){}
        self.m_native(owner,slot,a,b);
    }
    Native m_native{};Provider m_stereo;std::function<uintptr_t()> m_owner,m_lists;Memory m_memory;std::function<bool()> m_enabled;uintptr_t m_caller{};
    mutable std::mutex m_mutex;std::array<uint64_t,5> m_counts{};
};

// Diagnostic (shading.txt mode light_union_shadow_refresh_shared_spot_shadows): slot 1 skips
// rendering its spot shadows (0x1f84e40, one call per published spot record per Scene) and samples the atlas
// tiles slot 0 rendered this frame. Only when slot 0's record in the same row is the same spot,
// so the tile holds that light's depth. Tests whether the second eye's own shadow rendering is
// what flickers (docs/VIEWER_FLICKER_2026-10-27.md).
class NativeOrdinarySpotShadowShare {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uint32_t,uintptr_t);
    using Record=std::array<uint8_t,0xb0>;
    static constexpr size_t PUBLISHED=0xff60,BLOCK=0x3ed0,RECORD=0xfb0,FIRST=0x10;  // Scenes render from the published blocks (0x1f86ff0)
    static NativeOrdinarySpotShadowShare& get(){static auto* p=new NativeOrdinarySpotShadowShare;return *p;}
    static bool shares(const std::string& mode){return mode=="light_union_shadow_refresh_shared_spot_shadows";}
    // The row of `record` in slot 1's block of `owner`, if it is one.
    static std::optional<size_t> second_row(uintptr_t owner,uintptr_t record){
        const auto first=owner+PUBLISHED+BLOCK+FIRST;
        if(!owner || record<first || (record-first)%RECORD)return {};
        const auto row=(record-first)/RECORD;return row<4?std::optional{row}:std::nullopt;
    }
    // Both records are spots (kind 3) at the same position.
    static bool same_spot(const Record& a,const Record& b){
        int32_t ka{},kb{};std::memcpy(&ka,a.data()+0xa4,4);std::memcpy(&kb,b.data()+0xa4,4);
        return ka==3 && kb==3 && std::equal(a.begin()+0x80,a.begin()+0x8c,b.begin()+0x80);
    }
    using Read=std::function<bool(uintptr_t,Record&)>;
    bool install(void* target,std::function<bool(uintptr_t)> second_view,std::function<uintptr_t()> owner,Read read,std::function<bool()> enabled){
        if(m_native || !target || !second_view || !owner || !read || !enabled)return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&on_call),reinterpret_cast<void**>(&m_native))!=MH_OK)return false;
        m_second_view=std::move(second_view);m_owner=std::move(owner);m_read=std::move(read);m_enabled=std::move(enabled);
        if(MH_EnableHook(target)==MH_OK)return true;MH_RemoveHook(target);m_native=nullptr;return false;
    }
    bool install_in_game(uintptr_t base,const std::filesystem::path& path){
        constexpr game_build::Code expected=game_build::hashed(0x1f84e40,16,"442a19659fe9096de3a84e01a789d0005ab6710232eaa0a6a807ab732a813780");
        std::array<uint8_t,16> bytes{};
        if(!path.is_absolute() || !game_build::rva(0x1f84e40) || !game_build::rva(0x4faef88) ||
           !ordinary_scene_scope::read(base+game_build::rva(0x1f84e40),bytes) || !game_build::code(0x1f84e40,bytes,expected))return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(0x1f84e40)),
            [](uintptr_t view){const auto pair=NativeOrdinaryShadowCache::pair_for_view(view);return pair && view==pair->right.entry && pair->right.slot==1;},
            [base]{uintptr_t owner{};ordinary_scene_scope::read(base+game_build::rva(0x4faef88),owner);return owner;},
            [](uintptr_t at,Record& value){return ordinary_scene_scope::read(at,value);},
            [path,next=std::chrono::steady_clock::time_point{},enabled=false]()mutable{
                const auto now=std::chrono::steady_clock::now();if(now<next)return enabled;next=now+std::chrono::seconds(1);
                std::ifstream file{path};std::string line;if(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();enabled=shares(line);}return enabled;
            });
    }
    bool installed()const{return m_native!=nullptr;}
    // calls, slot-1 spot records while enabled, skipped (slot 0's tile reused), rendered (rows differ)
    std::array<uint64_t,4> stats()const{std::lock_guard lock{m_mutex};return m_counts;}
private:
    static void on_call(uintptr_t context,uintptr_t view,uint32_t index,uintptr_t record){
        auto& self=get();bool skip=false;
        try{
            std::lock_guard lock{self.m_mutex};++self.m_counts[0];
            if(self.m_enabled()){
                const auto owner=self.m_owner();const auto row=second_row(owner,record);
                if(row && self.m_second_view(view)){
                    ++self.m_counts[1];Record mine{},first{};
                    skip=self.m_read(record,mine) && self.m_read(owner+PUBLISHED+FIRST+*row*RECORD,first) && same_spot(mine,first);
                    ++self.m_counts[skip?2:3];
                }
            }
        }catch(...){skip=false;}
        if(!skip)self.m_native(context,view,index,record);
    }
    Native m_native{};std::function<bool(uintptr_t)> m_second_view;std::function<uintptr_t()> m_owner;Read m_read;std::function<bool()> m_enabled;
    mutable std::mutex m_mutex;std::array<uint64_t,4> m_counts{};
};
