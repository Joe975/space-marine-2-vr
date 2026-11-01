#pragma once
#include <d3d12.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "PerformanceTrace.h"

// Profiling diagnostic (SM2VR_PROFILE_QUEUE_TIMELINE=1, with -Profile): every D3D12 queue
// submission, GPU-side fence Signal/Wait, CPU fence Signal and CPU fence wait in the process, as
// trace rows, plus the time each queued Signal completed on the GPU. Built to answer what the
// render thread's depth-ready (ZMIPS_READY_0) wait is waiting for (docs/validation/
// depth-ready-fence-2026-10-21.md). Passive: every call goes to the original with the same
// arguments and its result is returned unchanged. A fence with a pending queued Signal is held by
// one COM reference until that value completes, so the completion watcher never touches a
// destroyed fence.
//
// Rows (value packs small IDs; queue and fence IDs are first-seen order, from 1):
//   queue_identity    value = queue*16 + D3D12_COMMAND_LIST_TYPE (once per queue)
//   queue_execute     value = queue*1000 + number of command lists
//   queue_signal      value = queue*2^48 + fence*2^32 + (fence value mod 2^32)
//   queue_wait        same packing: a GPU-side wait
//   fence_cpu_signal  value = fence*2^32 + value
//   fence_cpu_wait    value = fence*2^32 + value; SetEventOnCompletion (duration is the call: a
//                     null event blocks inside it)
//   fence_completed   same packing as queue_signal (queue 0 for a value a queue waited for), at the
//                     time the watcher saw the fence reach it
class GpuQueueTimeline {
public:
    using Emit=std::function<void(const char*,double,double,double)>;
    static GpuQueueTimeline& get(){static auto* value=new GpuQueueTimeline;return *value;}
    static constexpr double PACK_QUEUE=281474976710656.0,PACK_FENCE=4294967296.0;
    static double pack(uint32_t queue,uint32_t fence,uint64_t value){return double(queue)*PACK_QUEUE+double(fence)*PACK_FENCE+double(value&0xffffffffull);}
    static std::array<uint64_t,3> unpack(double packed){
        const auto whole=uint64_t(packed);return {whole>>48,(whole>>32)&0xffff,whole&0xffffffffull};}
    bool enabled()const{return m_enabled.load(std::memory_order_acquire);}
    // Hooks the queue's and a fence's vtable entries (shared by every queue/fence of the runtime).
    bool install(ID3D12Device* device,ID3D12CommandQueue* queue,Emit emit={}){
        std::lock_guard lock{m_install};
        if(m_installed)return true;
        if(!device || !queue)return false;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        auto** queue_table=*reinterpret_cast<void***>(queue);auto** fence_table=*reinterpret_cast<void***>(fence.Get());
        void* targets[]{queue_table[14],queue_table[15],fence_table[9],fence_table[10]};
        void* hooks[]{reinterpret_cast<void*>(&on_queue_signal),reinterpret_cast<void*>(&on_queue_wait),reinterpret_cast<void*>(&on_set_event),reinterpret_cast<void*>(&on_fence_signal)};
        void* originals[4]{};size_t made{};
        for(;made<4;++made)if(MH_CreateHook(targets[made],hooks[made],&originals[made])!=MH_OK)break;
        if(made!=4){while(made)MH_RemoveHook(targets[--made]);return false;}
        s_queue_signal=reinterpret_cast<QueueFence>(originals[0]);s_queue_wait=reinterpret_cast<QueueFence>(originals[1]);
        s_set_event=reinterpret_cast<SetEvent_>(originals[2]);s_fence_signal=reinterpret_cast<FenceSignal>(originals[3]);
        m_emit=emit?std::move(emit):Emit{[](const char* e,double s,double d,double v){PerformanceTrace::get().record(e,s,d,v);}};
        m_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!m_wake){for(auto t:targets)MH_RemoveHook(t);return false;}
        m_watcher=std::thread{[this]{watch();}};
        size_t enabled{};for(;enabled<4;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
        if(enabled!=4){for(auto t:targets){MH_DisableHook(t);MH_RemoveHook(t);}stop();return false;}
        m_targets.assign(std::begin(targets),std::end(targets));
        // ExecuteCommandLists may already be detoured by another probe (GpuCompletionProbe forwards
        // to executed()); hook it here only when it is free.
        void* execute=queue_table[10];void* original{};
        if(MH_CreateHook(execute,reinterpret_cast<void*>(&on_execute),&original)==MH_OK){
            s_execute=reinterpret_cast<Execute>(original);
            if(MH_EnableHook(execute)==MH_OK){m_targets.push_back(execute);m_execute_hooked=true;}else MH_RemoveHook(execute);
        }
        m_installed=true;m_enabled.store(true,std::memory_order_release);return true;
    }
    bool execute_hooked()const{return m_execute_hooked;}
    // Called from the existing ExecuteCommandLists detour (GpuCompletionProbe), before the original.
    void executed(ID3D12CommandQueue* queue,UINT count){
        if(!enabled())return;
        try{m_emit("queue_execute",PerformanceTrace::now_ms(),0,double(queue_id(queue))*1000+count);}catch(...){}
    }
    // Tests: unhook and stop the watcher (the trace keeps the rows).
    void uninstall(){
        std::lock_guard lock{m_install};if(!m_installed)return;
        m_enabled.store(false,std::memory_order_release);
        for(auto t:m_targets){MH_DisableHook(t);MH_RemoveHook(t);}m_targets.clear();stop();m_installed=false;m_execute_hooked=false;
    }
    size_t pending()const{std::lock_guard lock{m_mutex};size_t n{};for(auto& [f,w]:m_watched)n+=w.values.size();return n;}
private:
    using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
    inline static Execute s_execute{};
    static void STDMETHODCALLTYPE on_execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists){get().executed(queue,count);s_execute(queue,count,lists);}
    using QueueFence=HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,ID3D12Fence*,UINT64);
    using SetEvent_=HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*,UINT64,HANDLE);
    using FenceSignal=HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*,UINT64);
    struct Watched {Microsoft::WRL::ComPtr<ID3D12Fence> fence;uint32_t id{};std::vector<std::pair<uint64_t,uint32_t>> values;HANDLE event{};};
    inline static QueueFence s_queue_signal{},s_queue_wait{};inline static SetEvent_ s_set_event{};inline static FenceSignal s_fence_signal{};
    inline static thread_local bool s_inside{}; // The watcher's own SetEventOnCompletion calls are not traced.
    uint32_t queue_id(ID3D12CommandQueue* queue){
        std::lock_guard lock{m_mutex};auto& id=m_queues[reinterpret_cast<uintptr_t>(queue)];
        if(!id){id=uint32_t(m_queues.size());
            D3D12_COMMAND_QUEUE_DESC desc{};try{desc=queue->GetDesc();}catch(...){}
            m_emit("queue_identity",PerformanceTrace::now_ms(),0,double(id)*16+double(desc.Type));}
        return id;
    }
    uint32_t fence_id(ID3D12Fence* fence){
        std::lock_guard lock{m_mutex};auto& id=m_fences[reinterpret_cast<uintptr_t>(fence)];if(!id)id=uint32_t(m_fences.size());return id;
    }
    static HRESULT STDMETHODCALLTYPE on_queue_signal(ID3D12CommandQueue* queue,ID3D12Fence* fence,UINT64 value){
        auto& self=get();const auto at=PerformanceTrace::now_ms();const auto result=s_queue_signal(queue,fence,value);
        if(self.enabled() && !s_inside && SUCCEEDED(result) && fence)try{
            const auto q=self.queue_id(queue),f=self.fence_id(fence);self.m_emit("queue_signal",at,0,pack(q,f,value));self.watch_signal(fence,f,value,q);}catch(...){}
        return result;
    }
    static HRESULT STDMETHODCALLTYPE on_queue_wait(ID3D12CommandQueue* queue,ID3D12Fence* fence,UINT64 value){
        auto& self=get();const auto at=PerformanceTrace::now_ms();const auto result=s_queue_wait(queue,fence,value);
        // The awaited value is watched too (packed with queue 0): a fence signalled outside this
        // process (a VR runtime's compositor) still gets a completion time.
        if(self.enabled() && !s_inside && fence)try{const auto f=self.fence_id(fence);self.m_emit("queue_wait",at,0,pack(self.queue_id(queue),f,value));self.watch_signal(fence,f,value,0);}catch(...){}
        return result;
    }
    static HRESULT STDMETHODCALLTYPE on_set_event(ID3D12Fence* fence,UINT64 value,HANDLE event){
        auto& self=get();const auto at=PerformanceTrace::now_ms();const auto result=s_set_event(fence,value,event);
        if(self.enabled() && !s_inside && fence)try{self.m_emit("fence_cpu_wait",at,PerformanceTrace::now_ms()-at,pack(0,self.fence_id(fence),value));}catch(...){}
        return result;
    }
    static HRESULT STDMETHODCALLTYPE on_fence_signal(ID3D12Fence* fence,UINT64 value){
        auto& self=get();const auto at=PerformanceTrace::now_ms();const auto result=s_fence_signal(fence,value);
        if(self.enabled() && !s_inside && fence)try{self.m_emit("fence_cpu_signal",at,0,pack(0,self.fence_id(fence),value));}catch(...){}
        return result;
    }
    void watch_signal(ID3D12Fence* fence,uint32_t id,uint64_t value,uint32_t queue){
        {
            std::lock_guard lock{m_mutex};auto& w=m_watched[reinterpret_cast<uintptr_t>(fence)];
            if(!w.fence){if(m_watched.size()>60){m_watched.erase(reinterpret_cast<uintptr_t>(fence));return;}w.fence=fence;w.id=id;}
            if(w.values.size()>=256)return; // A fence nobody completes: stop queueing, keep the oldest.
            w.values.emplace_back(value,queue);
        }
        ::SetEvent(m_wake);
    }
    // One thread: an event per watched fence, armed for its oldest pending value.
    void watch(){
        s_inside=true;
        while(!m_stopping.load(std::memory_order_acquire)){
            std::vector<HANDLE> handles{m_wake};std::vector<uintptr_t> keys{0};
            {
                std::lock_guard lock{m_mutex};
                for(auto it=m_watched.begin();it!=m_watched.end();){
                    auto& w=it->second;
                    const auto completed=w.fence->GetCompletedValue();
                    const auto now=PerformanceTrace::now_ms();
                    auto done=std::partition(w.values.begin(),w.values.end(),[&](const auto& v){return v.first>completed;});
                    for(auto d=done;d!=w.values.end();++d)try{m_emit("fence_completed",now,0,pack(d->second,w.id,d->first));}catch(...){}
                    w.values.erase(done,w.values.end());
                    if(w.values.empty() || completed==UINT64_MAX){if(w.event)CloseHandle(w.event);it=m_watched.erase(it);continue;}
                    if(!w.event)w.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                    const auto lowest=std::min_element(w.values.begin(),w.values.end())->first;
                    if(w.event && SUCCEEDED(w.fence->SetEventOnCompletion(lowest,w.event)) && handles.size()<MAXIMUM_WAIT_OBJECTS){handles.push_back(w.event);keys.push_back(it->first);}
                    ++it;
                }
            }
            WaitForMultipleObjects(DWORD(handles.size()),handles.data(),FALSE,50);
        }
        std::lock_guard lock{m_mutex};for(auto& [k,w]:m_watched)if(w.event)CloseHandle(w.event);m_watched.clear();
    }
    void stop(){
        m_stopping.store(true,std::memory_order_release);if(m_wake)::SetEvent(m_wake);
        if(m_watcher.joinable())m_watcher.join();if(m_wake)CloseHandle(m_wake);m_wake=nullptr;m_stopping.store(false);
    }
    std::mutex m_install;mutable std::mutex m_mutex;std::atomic_bool m_enabled{},m_stopping{};bool m_installed{},m_execute_hooked{};
    Emit m_emit;std::map<uintptr_t,uint32_t> m_queues,m_fences;std::map<uintptr_t,Watched> m_watched;
    HANDLE m_wake{};std::thread m_watcher;std::vector<void*> m_targets;
};
