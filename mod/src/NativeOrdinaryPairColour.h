#pragma once
#include "NativeOrdinaryProbe.h"
#include "NativeOrdinarySceneScope.h"

// Independent pair attribution at existing native colour hooks. No extra hook,
// native arguments/banks/pool writes. A separately guarded synchronous consumer
// may retain GPU output; the observer itself never claims GPU/runtime proof.
class NativeOrdinaryPairColour {
public:
    using Scope=ordinary_scene_scope::Scope;using Evidence=ordinary_scene_scope::Evidence;
    using Provider=std::function<std::optional<Scope>()>;
    enum class Refusal:uint32_t {none,scope,context,changed,resource,exception};
    struct Event {
        uint64_t serial{};Scope scope;uint32_t thread{};NativeOrdinaryProbe::RawBoundary boundary;
        Evidence before,after;NativeRenderProbe::Output output;Refusal refusal{Refusal::none};
    };
    // nullptr invalidates any consumer scope on this thread. Event pointers are
    // synchronous and valid only for the duration of the callback.
    using Consumer=std::function<void(const Event*)>;
    struct Sample {bool armed{},installed{},instruction_guards{};uint64_t refused{},errors{},dropped{},raw_errors{};std::vector<Event> events;uint64_t aborted{},consumer_errors{};};
    static constexpr size_t CAPACITY=64;
    static NativeOrdinaryPairColour& get(){static auto* value=new NativeOrdinaryPairColour;return *value;}
    bool arm(Provider provider,NativeRenderProbe::OutputObserver outputs,Consumer consumer={}){return configure(std::move(provider),std::move(outputs),false,std::move(consumer));}
    bool arm_in_game(HMODULE module,NativeRenderProbe::OutputObserver outputs,Consumer consumer={}){
        if(!NativeOrdinaryProbe::guarded_in_game(module))return false;
        return configure([]()->std::optional<Scope>{const auto scene=NativeRenderProbe::current_scene();const auto pair=NativeRenderProbe::current_ordinary_pair();if(!scene || !pair)return {};return Scope{*scene,*pair};},std::move(outputs),true,std::move(consumer));
    }
    bool armed()const{return m_armed.load(std::memory_order_acquire);}
    Sample sample(uint64_t dispatch)const{
        std::lock_guard lock{m_mutex};Sample value{armed(),NativeOrdinaryProbe::get().installed(),m_guards,m_refused,m_errors,m_dropped,NativeOrdinaryProbe::get().raw_errors(),{}};
        for(const auto& e:m_events)if(e.scope.scene.dispatch_sequence==dispatch)value.events.push_back(e);value.aborted=m_aborted;value.consumer_errors=m_consumer_errors;return value;
    }
    static void write(std::ostream& out,const Sample& sample,uint32_t width,uint32_t height){
        out<<"{\"model\":\"ordinary_pair_colour_boundaries_v1\",\"cpu_only\":true,\"pip_enabled\":false,\"resource_leases\":false,\"final_writer_verified\":false,\"gpu_completion_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false,\"armed\":"<<(sample.armed?"true":"false")<<",\"installed\":"<<(sample.installed?"true":"false")<<",\"instruction_guards\":"<<(sample.instruction_guards?"true":"false")<<",\"refused\":"<<sample.refused<<",\"errors\":"<<sample.errors<<",\"dropped\":"<<sample.dropped<<",\"raw_errors\":"<<sample.raw_errors<<",\"aborted\":"<<sample.aborted<<",\"consumer_errors\":"<<sample.consumer_errors<<",\"display_width\":"<<width<<",\"display_height\":"<<height<<",\"events\":[";
        for(size_t i=0;i<sample.events.size();++i){if(i)out<<',';const auto& e=sample.events[i];const auto& s=e.scope.scene;const auto& p=e.scope.pair;const auto& eye=s.slot?p.right:p.left;
            out<<"{\"serial\":"<<e.serial<<",\"dispatch\":"<<s.dispatch_sequence<<",\"ordinal\":"<<s.ordinal<<",\"slot\":"<<s.slot<<",\"entry\":"<<s.entry<<",\"list\":"<<s.list<<",\"renderer\":"<<s.renderer<<",\"render_id\":"<<eye.render_id<<",\"revision\":"<<eye.revision<<",\"token\":"<<eye.token<<",\"producer_list\":"<<p.producer_list<<",\"producer_revision\":"<<p.producer_revision<<",\"left_entry\":"<<p.left.entry<<",\"right_entry\":"<<p.right.entry<<",\"thread\":"<<e.thread<<",\"refusal\":"<<uint32_t(e.refusal)<<",\"stage\":"<<uint32_t(e.boundary.stage)<<",\"wrapper\":"<<e.boundary.wrapper<<",\"entry_argument\":"<<e.boundary.entry<<",\"renderer_argument\":"<<e.boundary.renderer_state<<",\"resource\":"<<e.output.resource<<",\"status\":"<<uint32_t(e.output.status)<<",\"epoch\":"<<e.output.descriptors.epoch<<",\"serial_at_capture\":"<<e.output.descriptors.serial<<",\"scene_begin_epoch\":"<<s.resource_before.descriptors.epoch<<",\"scene_begin_serial\":"<<s.resource_before.descriptors.serial<<",\"before\":";
            ordinary_scene_scope::write(out,e.before);out<<",\"after\":";ordinary_scene_scope::write(out,e.after);out<<",\"rtvs\":[";
            for(size_t j=0;j<e.output.descriptors.views.size();++j){if(j)out<<',';const auto& v=e.output.descriptors.views[j];
                out<<"{\"resource\":"<<v.resource<<",\"identity\":"<<v.identity<<",\"handle\":"<<v.handle<<",\"known\":"<<(v.known?"true":"false")<<",\"created\":"<<v.created<<",\"bound\":"<<v.bound<<",\"command_list\":"<<v.command_list<<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"storage\":"<<v.storage<<",\"format\":"<<v.format<<",\"resource_dimension\":"<<v.resource_dimension<<",\"view_dimension\":"<<v.dimension<<",\"flags\":"<<v.flags<<",\"arrays\":"<<v.depth_or_array<<",\"mips\":"<<v.mips<<",\"samples\":"<<v.samples<<",\"planes\":"<<v.planes<<",\"subresources\":"<<v.subresources<<'}';
            }out<<"]}";
        }out<<"]}";
    }
private:
    bool configure(Provider provider,NativeRenderProbe::OutputObserver outputs,bool guards,Consumer consumer){
        std::lock_guard lock{m_install};if(armed() || !provider || !outputs || NativeOrdinaryProbe::get().installed())return false;
        m_provider=std::move(provider);m_outputs=std::move(outputs);m_consumer=std::move(consumer);m_guards=guards;
        if(!NativeOrdinaryProbe::get().attach_raw([this](const auto& raw){record(raw);}))return false;
        m_armed.store(true,std::memory_order_release);return true;
    }
    void record(const NativeOrdinaryProbe::RawBoundary& raw){
        if(!armed())return;std::optional<Event> event;
        if(raw.aborted){++m_aborted;notify(nullptr);return;}
        try{const auto scope=m_provider();if(!scope){notify(nullptr);return;}event.emplace();auto& e=*event;e.scope=*scope;e.thread=GetCurrentThreadId();e.boundary=raw;
            e.before=ordinary_scene_scope::inspect(*scope);const auto again=ordinary_scene_scope::inspect(*scope);if(e.before!=again)e.before.mask|=512;
            if(e.before.mask)e.refusal=Refusal::scope;
            else if((raw.stage==NativeOrdinaryProbe::Stage::post && raw.entry!=scope->scene.entry) ||
                    (raw.stage!=NativeOrdinaryProbe::Stage::post && raw.renderer_state!=scope->scene.renderer+0x10))e.refusal=Refusal::context;
            else{
                e.output=NativeRenderProbe::read_output(raw.wrapper,m_outputs);e.after=ordinary_scene_scope::inspect(*scope);const auto after=m_provider();
                if(!after || !ordinary_scene_scope::same(*scope,*after) || e.after!=e.before)e.refusal=Refusal::changed;
                else if(e.output.status!=NativeRenderProbe::OutputStatus::observed)e.refusal=Refusal::resource;
            }
        }catch(...){std::lock_guard lock{m_mutex};++m_errors;if(event)event->refusal=Refusal::exception;}
        bool retained{};
        try{if(event){std::lock_guard lock{m_mutex};auto& e=*event;e.serial=++m_serial;if(e.refusal!=Refusal::none)++m_refused;
            if(m_events.size()==CAPACITY){m_events.pop_front();++m_dropped;}m_events.push_back(e);retained=true;}}
        catch(...){std::lock_guard lock{m_mutex};++m_errors;}
        notify(retained?&*event:nullptr); // Outside observer lock; still inside the actual native Scene.
    }
    void notify(const Event* event)noexcept{if(!m_consumer)return;try{m_consumer(event);}catch(...){++m_consumer_errors;
        if(event)try{m_consumer(nullptr);}catch(...){++m_consumer_errors;}}}
    std::atomic_bool m_armed{};bool m_guards{};Provider m_provider;NativeRenderProbe::OutputObserver m_outputs;
    Consumer m_consumer;std::atomic<uint64_t> m_aborted{},m_consumer_errors{};
    std::mutex m_install;mutable std::mutex m_mutex;std::deque<Event> m_events;uint64_t m_serial{},m_refused{},m_errors{},m_dropped{};
};
