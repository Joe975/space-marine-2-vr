#include "NativePipTonemap.h"
#include "GameBuild.h"
#include "NativeRenderProbe.h"
#include <MinHook.h>
#include <algorithm>
#include <cstring>
namespace {
template<class T> bool read(uintptr_t address,T& value) {SIZE_T n{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&n) && n==sizeof(T);}
}
bool NativePipTonemap::matches(uint32_t stamp,std::span<const uint8_t> bytes) {return game_build::code(stamp,RVA,bytes,BYTES);}
bool NativePipTonemap::owned(uintptr_t renderer,int32_t record,std::span<const vr_camera::Matrix> poses) {
    uintptr_t entry{};uint64_t flags{};int32_t index{};vr_camera::Matrix world{};
    if(record<0 || !renderer || renderer>UINTPTR_MAX-0x148 || !read(renderer+0x140,entry) || !entry || entry>UINTPTR_MAX-0xa54 ||
       !read(entry,flags) || !(flags&0x40) || !read(entry+0xa50,index) || index!=record || !read(entry+0x34,world) || !vr_camera::rigid(world,false)) return false;
    return std::ranges::any_of(poses,[&](const auto& pose){for(size_t i=0;i<16;++i) if(!std::isfinite(pose[i]) || std::abs(world[i]-pose[i])>1e-4f) return false;return true;});
}
std::optional<int32_t> NativePipTonemap::primary_index(uintptr_t list) {
    int32_t count{};if(!list || list>UINTPTR_MAX-0x2996c || !read(list+0x29968,count) || count<2 || count>64) return std::nullopt;
    std::optional<int32_t> main,pip;
    for(int32_t slot=0;slot<count;++slot) {
        const auto entry=list+0x168+size_t(slot)*0xa60;uint64_t flags{};int32_t index{};
        if(!read(entry,flags)) return std::nullopt;
        if(!(flags&0x40) && ((flags&0x50000044) || !(flags&0x20000))) continue;
        if(!read(entry+0x188,index) || index<0 || index>1) return std::nullopt;
        if(flags&0x40) {if(pip) return std::nullopt;pip=index;}
        else if(!(flags&0x50000044) && (flags&0x20000)) {if(main) return std::nullopt;main=index;}
        else return std::nullopt;
    }
    return main && pip && *main!=*pip?main:std::nullopt;
}
std::optional<NativePipTonemap::OuterGrading> NativePipTonemap::outer_grading(uintptr_t pipeline,int32_t index,float scale,float minimum) {
    if(index<0 || index>1 || !pipeline || pipeline>UINTPTR_MAX-0x630 || !std::isfinite(scale) || scale<=0 || !std::isfinite(minimum) || minimum<=0) return std::nullopt;
    uintptr_t component{};uint32_t mask{};std::array<uint8_t,20> values{},again{};
    if(!read(pipeline+0x628,component) || !component || component>UINTPTR_MAX-0xc8 || !read(component+8+index*4,mask) || !(mask&2) ||
       !read(component+0x58+index*0x5c,values)) return std::nullopt;
    float first{},divisor{},gain{},second{};uint32_t mode{};
    std::memcpy(&first,values.data(),4);std::memcpy(&divisor,values.data()+4,4);std::memcpy(&gain,values.data()+8,4);
    std::memcpy(&second,values.data()+12,4);std::memcpy(&mode,values.data()+16,4);
    if(!std::isfinite(first) || !std::isfinite(divisor) || !std::isfinite(gain) || !std::isfinite(second) || mode>3) return std::nullopt;
    const float inverse=scale/std::max(divisor,minimum),scaled=inverse*gain;
    if(!std::isfinite(inverse) || !std::isfinite(scaled)) return std::nullopt;
    uintptr_t same{};uint32_t same_mask{};
    if(!read(pipeline+0x628,same) || same!=component || !read(component+8+index*4,same_mask) || same_mask!=mask ||
       !read(component+0x58+index*0x5c,again) || again!=values) return std::nullopt;
    OuterGrading output{};std::memcpy(output.data(),&mode,4);std::memcpy(output.data()+4,&first,4);std::memcpy(output.data()+8,&second,4);
    std::memcpy(output.data()+12,&inverse,4);std::memcpy(output.data()+16,&scaled,4);return output;
}
bool NativePipTonemap::install_in_game(HMODULE module,std::function<bool()> owned,bool main_grading,bool scene_effects) {
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};std::array<uint8_t,BYTES.size()> bytes{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) ||
       nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.SizeOfImage<RVA+BYTES.size() ||
       !read(base+game_build::rva(RVA),bytes) || !matches(nt.FileHeader.TimeDateStamp,bytes)) return false;
    void* outer{};
    const auto entry_owned=[base](uintptr_t entry) {
        const auto receipt=NativeRenderProbe::current_camera_entries();uintptr_t active{};const auto renderer=NativeRenderProbe::current_renderer();
        int32_t warmup{};
        return receipt && renderer && renderer<=UINTPTR_MAX-0x148 && read(renderer+0x140,active) && active==entry && entry==receipt->pip_entry &&
            primary_index(receipt->render_list)==0 && read(base+game_build::rva(0x4f9d010)+0x438,warmup) && warmup==5;
    };
    if(scene_effects) {
        std::array<uint8_t,OUTER_BYTES.size()> bytes{};
        if(nt.OptionalHeader.SizeOfImage<OUTER_RVA+bytes.size() || !read(base+game_build::rva(OUTER_RVA),bytes) || !game_build::code(OUTER_RVA,bytes,OUTER_BYTES)) return false;
        outer=reinterpret_cast<void*>(base+game_build::rva(OUTER_RVA));
    }
    if(main_grading) {
        std::array<uint8_t,SCENE_BYTES.size()> scene{};
        if(nt.OptionalHeader.SizeOfImage<SCENE_RVA+scene.size() || !read(base+game_build::rva(SCENE_RVA),scene) || !game_build::code(SCENE_RVA,scene,SCENE_BYTES)) return false;
        return install(reinterpret_cast<void*>(base+game_build::rva(RVA)),std::move(owned),reinterpret_cast<void*>(base+game_build::rva(SCENE_RVA)),[base] {
            uintptr_t list{};return read(base+game_build::rva(0x4f7c6c8),list)?primary_index(list):std::nullopt;
        },outer,entry_owned,[base](uintptr_t pipeline,int32_t index)->std::optional<OuterGrading> {
            if(index!=0 || pipeline!=base+game_build::rva(0x4faf840)) return std::nullopt;
            uint8_t enabled{};float scale{},minimum{};
            if(!read(base+game_build::rva(0x4faff88)+size_t(index)*0x210,enabled) || enabled>1 || !read(base+game_build::rva(0x3c8435c),minimum) ||
               !read(enabled?base+game_build::rva(0x4faff8c)+size_t(index)*0x210:base+game_build::rva(0x3c844b4),scale)) return std::nullopt;
            return outer_grading(pipeline,index,scale,minimum);
        });
    }
    return install(reinterpret_cast<void*>(base+game_build::rva(RVA)),std::move(owned),nullptr,{},outer,entry_owned);
}
bool NativePipTonemap::install(void* address,std::function<bool()> owned,void* scene,std::function<std::optional<int32_t>()> primary,void* outer,std::function<bool(uintptr_t)> entry_owned,std::function<std::optional<OuterGrading>(uintptr_t,int32_t)> outer_grading) {
    if(m_original || !address || !owned || bool(scene)!=bool(primary) || (outer && !entry_owned)) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original{};if(MH_CreateHook(address,reinterpret_cast<void*>(&on_core),&original)!=MH_OK) return false;
    void* scene_original{};
    if(scene && MH_CreateHook(scene,reinterpret_cast<void*>(&on_scene),&scene_original)!=MH_OK) {MH_RemoveHook(address);return false;}
    void* outer_original{};
    if(outer && MH_CreateHook(outer,reinterpret_cast<void*>(&on_outer),&outer_original)!=MH_OK) {
        if(scene) MH_RemoveHook(scene);MH_RemoveHook(address);return false;
    }
    m_owned=std::move(owned);m_original=reinterpret_cast<Core>(original);m_scene=reinterpret_cast<Scene>(scene_original);m_primary=std::move(primary);
    m_outer=reinterpret_cast<Outer>(outer_original);m_entry_owned=std::move(entry_owned);
    m_outer_grading=std::move(outer_grading);
    if((!scene || MH_EnableHook(scene)==MH_OK) && MH_EnableHook(address)==MH_OK && (!outer || MH_EnableHook(outer)==MH_OK)) return true;
    if(outer) {MH_DisableHook(outer);MH_RemoveHook(outer);}m_outer=nullptr;
    MH_DisableHook(address);
    if(scene) {MH_DisableHook(scene);MH_RemoveHook(scene);}m_original=nullptr;m_scene=nullptr;MH_RemoveHook(address);return false;
}
void NativePipTonemap::on_core(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uint32_t f,int32_t g,const void* descriptor,uintptr_t i) {
    auto& self=get();alignas(4) std::array<uint8_t,8> copy{};const void* selected=descriptor;
    struct Scope {std::optional<Grading> previous{s_grading};Scope(){s_grading.reset();}~Scope(){s_grading=previous;}} scope;
    try {
        std::array<uint8_t,6> source{};float exposure{};
        if(self.m_owned() && read(reinterpret_cast<uintptr_t>(descriptor),source)) {
            std::memcpy(&exposure,source.data(),4);
            if(std::isfinite(exposure) && exposure>0 && source[4]<=1 && (source[5]==1 || (s_effects && source[5]==0))) {
                std::memcpy(copy.data(),source.data(),source.size());const float scene_exposure=0;
                std::memcpy(copy.data(),&scene_exposure,4);selected=copy.data();++self.m_applied;
                if(self.m_primary && g>=0 && g<=1) {
                    const auto primary=self.m_primary();
                    if(primary && *primary>=0 && *primary<=1 && *primary!=g) s_grading=Grading{g,*primary,s_outer_grading};
                }
            }
        }
    } catch(...) {}
    self.m_original(a,b,c,d,e,f,g,selected,i);
}
void NativePipTonemap::on_outer(uintptr_t a,uintptr_t b,float c,uintptr_t entry,uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,const void* descriptor) {
    auto& self=get();alignas(4) std::array<uint8_t,8> copy{};const void* selected=descriptor;
    struct Scope {bool previous{s_effects};std::optional<OuterGrading> grading{s_outer_grading};Scope(){s_effects=false;s_outer_grading.reset();}~Scope(){s_effects=previous;s_outer_grading=grading;}} scope;
    try {
        std::array<uint8_t,6> source{};float exposure{};uint64_t flags{};int32_t index{};
        if(std::isfinite(c) && c>=0 && c<=1 && self.m_owned() && self.m_entry_owned(entry) && entry && entry<=UINTPTR_MAX-0x18c && read(entry,flags) && (flags&0x40) &&
           read(entry+0x188,index) && index==1 && read(reinterpret_cast<uintptr_t>(descriptor),source)) {
            std::memcpy(&exposure,source.data(),4);
            if(std::isfinite(exposure) && exposure>0 && source[4]<=1 && source[5]==1) {
                std::memcpy(copy.data(),source.data(),source.size());copy[5]=0;selected=copy.data();s_effects=true;++self.m_effects_applied;
                if(self.m_outer_grading && self.m_primary) {const auto main=self.m_primary();if(main && *main==0) s_outer_grading=self.m_outer_grading(a,*main);}
            }
        }
    } catch(...) {}
    self.m_outer(a,b,c,entry,e,f,g,h,selected);
}
void NativePipTonemap::on_scene(uintptr_t a,uintptr_t b,uintptr_t c,const void* d,int32_t index) {
    auto& self=get();
    if(s_grading && index==s_grading->pip) {
        index=s_grading->main;++self.m_grading_applied;
        if(s_grading->outer) {d=s_grading->outer->data();++self.m_outer_grading_applied;}
    }
    // Only the read/bind helper sees the main index. Core HDR preparation still
    // receives the original PIP index and cannot update main HDR histories here.
    self.m_scene(a,b,c,d,index);
}
