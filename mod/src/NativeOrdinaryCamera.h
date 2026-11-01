#pragma once
#include "NativeCameraEntries.h"
#include "NativeRenderProbe.h"
#include "NativeOrdinaryPacket.h"
#include <Windows.h>
#include <cstring>
#include <functional>
#include <algorithm>
#include <ostream>

// Four isolated desktop-only second-view appends through the actual native add
// function. No PIP API, outer job replay, cached resource lease or headset path.
class NativeOrdinaryCamera {
public:
    using Args=std::array<uint64_t,9>;
    using Add=std::function<int64_t(void*,void*,const Args&)>;
    using Renderer=std::function<uintptr_t()>;
    // Odd spacing observes both native render-counter ping-pong phases.
    struct Config {unsigned samples{4},cadence{241};float shift{.064f};bool consecutive{},performance{};};
    struct Statistics {uint64_t appended{},attempts{},last_render_id{};uint32_t thread{};std::array<uint64_t,7> refusals{};bool stopped{};};
    static bool performance_conflict(std::wstring_view name,std::wstring_view value){
        if(value.empty() || value==L"0")return false;
        std::wstring normalized{name};for(auto& c:normalized)if(c>=L'a' && c<=L'z')c-=L'a'-L'A';
        return normalized==L"SM2VR_OPENVR" || normalized==L"SM2VR_OPENXR" || normalized==L"SM2VR_SYNTHETIC_EYES" || normalized.starts_with(L"SM2VR_PIP") ||
            (normalized.starts_with(L"SM2VR_ORDINARY_") && normalized!=L"SM2VR_ORDINARY_PERF_SECOND" && normalized!=L"SM2VR_ORDINARY_PERF_CONTROL");
    }
    static bool performance_control(std::string_view mode){return mode=="on";}
    void performance_enabled(bool enabled){m_perf_enabled.store(enabled,std::memory_order_release);}
    Statistics statistics()const{std::lock_guard lock{m_mutex};auto result=m_statistics;result.stopped=m_stopped.load();return result;}
    struct Receipt {uint64_t render_id{},producer_revision{};uint32_t token{},thread{};uintptr_t list{},renderer{};int32_t left{-1},right{-1};uint64_t left_flags{},right_flags{};bool appended{},source_unchanged{},bank_zero{};unsigned refusal{};};
    static NativeOrdinaryCamera& get(){static auto* value=new NativeOrdinaryCamera;return *value;}
    bool arm(Renderer renderer,const Config& config){
        std::lock_guard lock{m_arm_mutex};if(m_armed || !renderer || !config.samples || config.samples>4 || !config.cadence || config.cadence>600 || !std::isfinite(config.shift) || config.shift<=0 || config.shift>.1f)return false;
        m_renderer=std::move(renderer);m_config=config;m_armed.store(true,std::memory_order_release);return true;
    }
    void on_main(void* native_list,const void* source,const FrameSyncClock::Frame* frame,int32_t index,const Args& args,const Add& add){
        if(!m_armed.load(std::memory_order_acquire))return;
        if(m_busy.test_and_set(std::memory_order_acquire))return;
        struct Release{std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{m_busy};
        if(m_stopped || (!m_config.performance && m_receipts.size()>=m_config.samples) || !frame || frame->invalid || !frame->render_id || !source || !native_list || !add)return;
        if(m_config.performance && !m_perf_enabled.load(std::memory_order_acquire))return;
        const auto list=reinterpret_cast<uintptr_t>(native_list);int32_t count{};std::array<uint8_t,0x388> camera{};float fov{};
        if(!read(reinterpret_cast<uintptr_t>(source),camera))return;std::memcpy(&fov,camera.data()+0x2d0,4);
        if(frame->render_id<=m_last_seen)return;
        if(!std::isfinite(fov) || fov<40 || fov>150 || index!=0 || !read(list+0x29808,count) || count!=1 || (args[0]&0x50000044)){m_eligible=0;return;}
        m_last_seen=frame->render_id;++m_eligible;
        // Explicit continuous-history control still warms up for the full normal
        // cadence, then appends at most four adjacent eligible camera jobs.
        if(m_eligible<m_config.cadence || (!m_config.performance && !m_config.consecutive && m_eligible%m_config.cadence))return;
        Receipt r;r.render_id=frame->render_id;r.token=frame->token;r.thread=GetCurrentThreadId();r.list=list;r.renderer=m_renderer();r.left=index;
        std::array<uint64_t,11> bank{};
        if(!r.renderer || !read(r.renderer+0x258,bank) || !read(list+8,r.left_flags) || (r.left_flags&0x50000044)){r.refusal=1;store(r);return;}
        r.bank_zero=std::ranges::all_of(bank,[](auto v){return v==0;});
        // The second bank must be unused on the first append. Subsequent appends
        // are limited to this same native renderer, with native-owned history.
        if((!m_attempts && !r.bank_zero) || (m_owner && m_owner!=r.renderer)){r.refusal=2;store(r);return;}
        vr_camera::Matrix world{},view{};std::memcpy(world.data(),camera.data(),64);std::memcpy(view.data(),camera.data()+0x40,64);
        if(!vr_camera::rigid(world,false) || !vr_camera::rigid(view,false)){r.refusal=3;store(r);return;}
        const auto original=camera;
        for(size_t n=0;n<3;++n)world[12+n]+=world[n]*m_config.shift;
        for(size_t n=0;n<3;++n)view[12+n]=-(world[12]*view[n]+world[13]*view[4+n]+world[14]*view[8+n]);
        std::memcpy(camera.data(),world.data(),64);std::memcpy(camera.data()+0x40,view.data(),64);
        auto ticket=NativeCameraEntries::get().begin(list,NativeCameraEntries::Role::ordinary_right,*frame);
        if(!ticket){r.refusal=4;store(r);return;}
        r.producer_revision=ticket->revision;
        int64_t result{};try{result=add(native_list,camera.data(),args);}catch(...){NativeCameraEntries::get().finish(*ticket,-1,std::nullopt);r.refusal=6;store(r);return;}r.right=int32_t(result);r.appended=true;
        vr_camera::Matrix actual{};std::optional<vr_camera::Matrix> observed;
        const auto pointer=NativeCameraEntries::address(list,r.right);
        if(r.right==1 && read(list+0x29808,count) && count==2 && pointer && read(*pointer,r.right_flags) && !(r.right_flags&0x50000044) && read(*pointer+0x34,actual))observed=actual;
        NativeCameraEntries::get().finish(*ticket,r.right,observed);
        std::array<uint8_t,0x388> after{};r.source_unchanged=read(reinterpret_cast<uintptr_t>(source),after);
        // Compare against the original source, not the deliberately shifted copy.
        r.source_unchanged=r.source_unchanged && after==original;
        if(!observed || !r.source_unchanged || r.right_flags!=r.left_flags)r.refusal=5;
        m_owner=r.renderer;store(r);
    }
    std::vector<Receipt> receipts()const{std::lock_guard lock{m_mutex};return m_receipts;}
    void request_stop(){m_stopped.store(true,std::memory_order_release);}
    // Lifts a consumer-requested stop (swap-chain reset) on an armed control; the bounded
    // sample count and bank ownership are kept. Refuses while an append is in flight.
    bool restart(){if(!m_armed.load(std::memory_order_acquire) || m_busy.test(std::memory_order_acquire))return false;m_stopped.store(false,std::memory_order_release);return true;}
    static void write_capture(std::ostream& out,const NativeRenderProbe::Snapshot& s,const Receipt& r){
        out<<"{\"model\":\"ordinary_second_camera_v2\",\"cpu_only\":true,\"pip_enabled\":false,\"gpu_pair_verified\":false,\"histories_verified\":false,\"runtime_submission\":false,\"dispatch\":"<<s.sequence<<",\"scene_calls\":"<<s.scene_calls<<",\"scene_refused\":"<<s.scene_refused<<",\"scene_dropped\":"<<s.scene_dropped<<",\"renderer\":"<<s.renderer<<",\"list\":"<<s.list;
        out<<",\"append\":{\"render_id\":"<<r.render_id<<",\"producer_revision\":"<<r.producer_revision<<",\"token\":"<<r.token<<",\"thread\":"<<r.thread<<",\"list\":"<<r.list<<",\"renderer\":"<<r.renderer<<",\"left\":"<<r.left<<",\"right\":"<<r.right<<",\"left_flags\":"<<r.left_flags<<",\"right_flags\":"<<r.right_flags<<",\"refusal\":"<<r.refusal<<",\"appended\":"<<(r.appended?"true":"false")<<",\"source_unchanged\":"<<(r.source_unchanged?"true":"false")<<",\"bank_zero\":"<<(r.bank_zero?"true":"false")<<'}';
        out<<",\"ordinary_pair\":";if(!s.ordinary_pair)out<<"null";else {const auto& p=*s.ordinary_pair;out<<"{\"render_id\":"<<p.left.render_id<<",\"token\":"<<p.left.token<<",\"revision\":"<<p.left.revision<<",\"producer_list\":"<<p.producer_list<<",\"producer_revision\":"<<p.producer_revision<<",\"add_list\":"<<p.left.add_list<<",\"left_entry\":"<<p.left.entry<<",\"right_entry\":"<<p.right.entry<<",\"left_slot\":"<<p.left.slot<<",\"right_slot\":"<<p.right.slot<<'}';}
        out<<",\"views\":[";for(size_t i=0;i<s.before.size();++i){if(i)out<<',';const auto& v=s.before[i];out<<"{\"slot\":"<<v.slot<<",\"flags\":"<<v.flags;
            const auto matrix=[&](const char* name,const auto& values){out<<",\""<<name<<"\":[";for(size_t n=0;n<16;++n){if(n)out<<',';out<<values[n];}out<<']';};
            matrix("world",v.world);matrix("view",v.view);matrix("projection",v.projection);matrix("inverse_projection",v.inverse_projection);out<<",\"native_packet\":";if(v.packet)v.packet->write(out);else out<<"null";out<<'}';}
        out<<"],\"packet_transaction\":";NativeOrdinaryPacket::get().write_receipt(out,s.sequence);
        out<<",\"scenes\":[";for(size_t i=0;i<s.scenes.size();++i){if(i)out<<',';const auto& v=s.scenes[i];out<<"{\"slot\":"<<v.slot<<",\"entry\":"<<v.entry<<",\"active_entry_after\":"<<v.active_entry_after<<",\"refusal\":"<<uint32_t(v.refusal)<<",\"counter_before\":"<<v.render_counter_before<<",\"counter_after\":"<<v.render_counter_after;
            const auto temporal=[&](const char* name,const auto& value){out<<",\""<<name<<"\":";if(!value){out<<"null";return;}out<<"{\"previous_words\":[";for(size_t n=0;n<11;++n){if(n)out<<',';out<<value->previous[n];}out<<"],\"next_words\":[";for(size_t n=0;n<11;++n){if(n)out<<',';out<<value->next[n];}out<<"]}";};temporal("temporal_before",v.temporal_before);temporal("temporal_after",v.temporal_after);
            const auto bank=[&](const char* name,const auto& value){out<<",\""<<name<<"\":";if(!value){out<<"null";return;}out<<"{\"address\":"<<value->address<<",\"words\":[";for(size_t n=0;n<11;++n){if(n)out<<',';out<<value->words[n];}out<<"]}";};bank("persistent_before",v.persistent_before);bank("persistent_after",v.persistent_after);
            out<<",\"packet_before\":";if(v.packet_before)v.packet_before->write(out);else out<<"null";out<<",\"packet_after\":";if(v.packet_after)v.packet_after->write(out);else out<<"null";out<<'}';}out<<"]}\n";
    }
private:
    template<class T>static bool read(uintptr_t p,T& out){SIZE_T n{};return p && p<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),&out,sizeof(T),&n) && n==sizeof(T);}
    void store(const Receipt& r){std::lock_guard lock{m_mutex};++m_attempts;++m_statistics.attempts;m_statistics.appended+=r.appended;m_statistics.last_render_id=r.render_id;m_statistics.thread=r.thread;if(r.refusal<m_statistics.refusals.size())m_statistics.refusals[r.refusal]+=r.refusal!=0;if(!m_config.performance)m_receipts.push_back(r);if(r.refusal)m_stopped=true;}
    Renderer m_renderer;Config m_config;std::atomic_bool m_armed{},m_stopped{};uint64_t m_eligible{},m_last_seen{};uintptr_t m_owner{};
    std::atomic_flag m_busy=ATOMIC_FLAG_INIT;
    std::mutex m_arm_mutex;mutable std::mutex m_mutex;std::vector<Receipt> m_receipts;
    std::atomic_bool m_perf_enabled{};uint64_t m_attempts{};Statistics m_statistics;
};
