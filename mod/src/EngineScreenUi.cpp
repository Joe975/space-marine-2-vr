#include "EngineScreenUi.h"
#include "EngineCameraProbe.h"
#include "GameBuild.h"
#include "BoundedTable.h"
#include <MinHook.h>
#include <algorithm>
#include <intrin.h>
#include <spdlog/spdlog.h>

namespace {
template<class T> bool read(uintptr_t address,T& out) {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&out,sizeof(out),&count) && count==sizeof(out);
}
}
EngineScreenUi& EngineScreenUi::get() { static auto* instance=new EngineScreenUi;return *instance; }
bool EngineScreenUi::matches_build(uint32_t timestamp,std::span<const uint8_t> bytes) {
    return game_build::code(timestamp,LAYOUT_RVA,bytes,PROLOGUE);
}
bool EngineScreenUi::install_in_game(HMODULE game) {
    if(installed()) return true;
    const auto base=reinterpret_cast<const uint8_t*>(game);
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto layout=game_build::rva(nt->FileHeader.TimeDateStamp,LAYOUT_RVA);
    if(!layout || layout+PROLOGUE.size()>nt->OptionalHeader.SizeOfImage ||
        !matches_build(nt->FileHeader.TimeDateStamp,{base+layout,PROLOGUE.size()})) return false;
    return install(const_cast<uint8_t*>(base)+layout,reinterpret_cast<uintptr_t>(base));
}
bool EngineScreenUi::install(void* target,uintptr_t module) {
    std::lock_guard lock{m_install_mutex};
    if(installed()) return true;
    auto status=MH_Initialize();
    if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) return false;
    void* original{};
    status=MH_CreateHook(target,reinterpret_cast<void*>(&on_layout),&original);
    if(status!=MH_OK) return false;
    m_original=reinterpret_cast<Layout*>(original);m_module=module;
    status=MH_EnableHook(target);
    if(status!=MH_OK) { MH_RemoveHook(target);return false; }
    m_installed=true;spdlog::info("ScreenUi: movie layout hook installed; passive viewport capture");return true;
}
void EngineScreenUi::on_layout(void* object,const int* size,const int* origin) {
    auto& self=get();
    try {
        Sample sample{};sample.object=reinterpret_cast<uintptr_t>(object);
        sample.caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-self.m_module;
        if(read(reinterpret_cast<uintptr_t>(size),sample.size) && read(reinterpret_cast<uintptr_t>(origin),sample.origin)) {
            read(sample.object,sample.flags);
            if(read(sample.object+0x18,sample.renderer)) {
                uintptr_t vtable{};
                if(read(sample.renderer,vtable)) read(vtable+0x190,sample.viewport_function);
            }
            std::lock_guard lock{self.m_mutex};++self.m_snapshot.calls;
            // A full table gives the least recently seen row to the new movie (BoundedTable.h).
            sample.received=std::chrono::steady_clock::now();
            if(bounded_table::upsert(self.m_snapshot.movies,sample,[&](const Sample& row){return row.object==sample.object;},16)==bounded_table::Result::dropped)++self.m_snapshot.overflow;
        }
    } catch(...) {}
    self.m_original.load()(object,size,origin);
}
EngineScreenUi::Snapshot EngineScreenUi::snapshot() const {std::lock_guard lock{m_mutex};return m_snapshot;}
