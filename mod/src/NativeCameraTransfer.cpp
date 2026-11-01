#include "NativeCameraTransfer.h"
#include "NativeCameraEntries.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <algorithm>
#include <memory>
#include <cstring>

namespace {
template<class T> bool read(uintptr_t address,T& value) {SIZE_T bytes{};return address && address<=UINTPTR_MAX-sizeof(T) &&
    ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(T),&bytes) && bytes==sizeof(T);}
}
NativeCameraTransfer& NativeCameraTransfer::get() {static auto* value=new NativeCameraTransfer;return *value;}
bool NativeCameraTransfer::matches(uint32_t stamp,std::span<const uint8_t> reset,std::span<const uint8_t> copy) {
    return game_build::code(stamp,RESET_RVA,reset,RESET_BYTES) && game_build::code(stamp,COPY_RVA,copy,COPY_BYTES);
}
bool NativeCameraTransfer::install_in_game(HMODULE module) {
    const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    std::array<uint8_t,RESET_BYTES.size()> reset{};std::array<uint8_t,COPY_BYTES.size()> copy{};
    if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) ||
       nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !game_build::rva(nt.FileHeader.TimeDateStamp,RESET_RVA) || !game_build::rva(nt.FileHeader.TimeDateStamp,COPY_RVA) ||
       nt.OptionalHeader.SizeOfImage<game_build::rva(nt.FileHeader.TimeDateStamp,COPY_RVA)+copy.size() ||
       !read(base+game_build::rva(nt.FileHeader.TimeDateStamp,RESET_RVA),reset) || !read(base+game_build::rva(nt.FileHeader.TimeDateStamp,COPY_RVA),copy) ||
       !matches(nt.FileHeader.TimeDateStamp,reset,copy)) return false;
    return install(reinterpret_cast<void*>(base+game_build::rva(nt.FileHeader.TimeDateStamp,RESET_RVA)),reinterpret_cast<void*>(base+game_build::rva(nt.FileHeader.TimeDateStamp,COPY_RVA)));
}
bool NativeCameraTransfer::install(void* reset,void* copy) {
    std::lock_guard lock{m_install};if(m_reset || !reset || !copy) return false;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void *original_reset{},*original_copy{};
    if(MH_CreateHook(reset,reinterpret_cast<void*>(&on_reset),&original_reset)!=MH_OK) return false;
    if(MH_CreateHook(copy,reinterpret_cast<void*>(&on_copy),&original_copy)!=MH_OK) {MH_RemoveHook(reset);return false;}
    m_reset=reinterpret_cast<Reset>(original_reset);m_copy=reinterpret_cast<Copy>(original_copy);
    if(MH_EnableHook(reset)==MH_OK && MH_EnableHook(copy)==MH_OK) return true;
    MH_DisableHook(reset);MH_DisableHook(copy);MH_RemoveHook(reset);MH_RemoveHook(copy);m_reset=nullptr;m_copy=nullptr;return false;
}
void NativeCameraTransfer::on_reset(void* list) {
    auto& self=get();++self.m_resets;try {NativeCameraEntries::get().reset(reinterpret_cast<uintptr_t>(list));} catch(...) {}
    self.m_reset.load()(list);
}
void NativeCameraTransfer::on_copy(void* a,void* b) {
    auto& self=get();++self.m_copies;auto& entries=NativeCameraEntries::get();
    std::optional<NativeCameraEntries::CopyTicket> ticket;
    // 340 KB, once per thread that copies lists: every byte is read over before use, so no per-copy
    // allocation and zero fill (about once a frame, MOD_REVIEW_2026-10-29).
    struct Buffers {std::array<uint8_t,BYTES> b,after;};thread_local std::unique_ptr<Buffers> scratch;Buffers* buffers{};
    bool before{};
    try {
        ticket=entries.begin_copy(reinterpret_cast<uintptr_t>(a),reinterpret_cast<uintptr_t>(b));
        if(ticket && ticket->before_b.valid) {
            if(!scratch) scratch=std::make_unique<Buffers>();
            buffers=scratch.get();before=read(reinterpret_cast<uintptr_t>(b),buffers->b); // The native copy never re-enters this hook.
            const auto matches=[](const auto& content,uintptr_t list,const auto& bytes) {
                if(!content.valid) return true;int32_t count{};std::memcpy(&count,bytes.data()+0x29808,4);if(count<0 || count>64) return false;
                for(const auto& entry:content.entries) {
                    const auto address=NativeCameraEntries::address(list,entry.slot);
                    if(!address || entry.address!=*address || entry.slot>=count) return false;
                    vr_camera::Matrix world{};uint64_t flags{};const auto offset=8+size_t(entry.slot)*0xa60;
                    std::memcpy(world.data(),bytes.data()+offset+0x34,sizeof(world));std::memcpy(&flags,bytes.data()+offset,8);
                    if((entry.role==NativeCameraEntries::Role::pip && !(flags&0x40)) || (entry.role==NativeCameraEntries::Role::main && (flags&0x50000044))) return false;
                    for(size_t i=0;i<16;++i) if(!std::isfinite(world[i]) || std::abs(world[i]-entry.world[i])>1e-4f) return false;
                }
                return true;
            };
            before=before && matches(ticket->before_b,reinterpret_cast<uintptr_t>(b),buffers->b);
        }
    } catch(...) {try {entries.reset(reinterpret_cast<uintptr_t>(a));entries.reset(reinterpret_cast<uintptr_t>(b));} catch(...) {}}
    self.m_copy.load()(a,b);
    bool verified{};
    try {
        bool exact=before && read(reinterpret_cast<uintptr_t>(a),buffers->after) && buffers->after==buffers->b;
        exact=exact && read(reinterpret_cast<uintptr_t>(b),buffers->after) && buffers->after==buffers->b;
        if(ticket) verified=entries.finish_copy(*ticket,exact);
    } catch(...) {try {entries.reset(reinterpret_cast<uintptr_t>(a));entries.reset(reinterpret_cast<uintptr_t>(b));} catch(...) {}}
    if(verified) ++self.m_verified;else ++self.m_refused;
}
