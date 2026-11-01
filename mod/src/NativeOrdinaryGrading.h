#pragma once
#include "NativeCallCensus.h"
#include "NativeOrdinarySceneScope.h"
#include "GameBuild.h"
#include <MinHook.h>
#include <cstring>

// Share the already prepared main grading/exposure READ for the exact ordinary
// pair. The right HDR core retains its native index and its own preparation.
class NativeOrdinaryGrading {
public:
    using Native=void(*)(uintptr_t,uintptr_t,uintptr_t,const void*,int32_t);
    using Scope=ordinary_scene_scope::Scope;
    struct Input {Scope scope;uintptr_t core{};int32_t index{-1};std::array<uint8_t,21> grading{};std::array<uint8_t,6> descriptor{};};
    struct Record {uint64_t dispatch{};std::array<int32_t,2> indices{-1,-1};std::array<float,2> exposure{};std::array<uint32_t,2> effects{};uint32_t applied{};std::array<std::array<uint8_t,21>,2> grading{};std::array<std::array<float,8>,2> hdr{};std::array<bool,2> hdr_read{};
        std::array<std::array<uint32_t,102>,4> core_words{};std::array<bool,4> core_read{};};
    static NativeOrdinaryGrading& get(){static auto* value=new NativeOrdinaryGrading;return *value;}
    bool install(void* target){
        if(m_original || !target)return false;const auto result=MH_Initialize();if(result!=MH_OK && result!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* original{};if(MH_CreateHook(target,reinterpret_cast<void*>(&on_scene),&original)!=MH_OK)return false;
        m_original=reinterpret_cast<Native>(original);if(MH_EnableHook(target)==MH_OK)return true;
        MH_RemoveHook(target);m_original=nullptr;return false;
    }
    bool install_in_game(uintptr_t base){
        wchar_t adaptation[8]{};m_share_adaptation=!(GetEnvironmentVariableW(L"SM2VR_ORDINARY_SHARED_ADAPTATION",adaptation,8)==1 && adaptation[0]==L'0');
        wchar_t fixed[8]{};m_fixed=GetEnvironmentVariableW(L"SM2VR_ORDINARY_FIXED_EXPOSURE",fixed,8)==1 && fixed[0]==L'1';
        constexpr game_build::Code expected=game_build::hashed(0x1e92740,16,"7c62cad323fc153a26dd1b48dc2dcc1f716cfa7a1d10b71a674930e56576819c");std::array<uint8_t,16> bytes{};
        return ordinary_scene_scope::read(base+game_build::rva(0x1e92740),bytes) && game_build::code(0x1e92740,bytes,expected) && install(reinterpret_cast<void*>(base+game_build::rva(0x1e92740)));
    }
    bool installed()const{return m_original!=nullptr;}
    static std::optional<std::array<uint8_t,6>> fixed_descriptor(bool enabled,const std::optional<Input>& input){
        if(!enabled || !input || input->scope.scene.slot>1 || input->index<0 || input->index>1)return {};
        auto result=input->descriptor;const float exposure=1.0f;std::memcpy(result.data(),&exposure,4);return result;
    }
    auto descriptor(const std::optional<Input>& input)const{return fixed_descriptor(m_fixed,input);}
    static bool matches(const Input& left,const Input& right){
        auto adjusted=right.scope;adjusted.scene.slot=0;adjusted.scene.ordinal=1;adjusted.scene.entry=adjusted.pair.left.entry;
        return left.index==0 && right.index==1 && left.scope.scene.slot==0 && right.scope.scene.slot==1 && left.core==right.core &&
            ordinary_scene_scope::same(left.scope,adjusted);
    }
    // All pointers are copied at the authenticated native call, never retained.
    template<class Call>void invoke(const std::optional<Input>& input,Call call){
        struct Restore {std::optional<Input> previous{s_active};bool shared{s_shared_step};~Restore(){s_active=previous;s_shared_step=shared;}} restore;s_active.reset();
        if(input && input->scope.scene.slot<2){
            std::lock_guard lock{m_mutex};const auto slot=input->scope.scene.slot;const auto dispatch=input->scope.scene.dispatch_sequence;
            auto found=std::ranges::find_if(m_records,[&](const auto& r){return r.dispatch==dispatch;});
            if(found==m_records.end() && m_records.size()<4){m_records.push_back(Record{dispatch});found=std::prev(m_records.end());}
            if(found!=m_records.end()){found->indices[slot]=input->index;std::memcpy(&found->exposure[slot],input->descriptor.data(),4);found->effects[slot]=input->descriptor[5];found->grading[slot]=input->grading;}
            if(found!=m_records.end())found->core_read[slot*2]=ordinary_scene_scope::read(input->core+size_t(input->index)*0x198,found->core_words[slot*2]);
            if(!slot)m_left=std::pair{GetCurrentThreadId(),*input};
            else if(m_left && m_left->first==GetCurrentThreadId() && matches(m_left->second,*input))s_active=m_left->second;
        }
        const bool shared=share_adaptation_before(input);s_shared_step=shared;
        call();
        s_shared_step=restore.shared;if(input && input->scope.scene.slot<2)share_adaptation_after(*input,shared);
        if(input && input->scope.scene.slot<2){std::lock_guard lock{m_mutex};for(auto& r:m_records)if(r.dispatch==input->scope.scene.dispatch_sequence){const auto slot=input->scope.scene.slot;r.hdr_read[slot]=ordinary_scene_scope::read(input->core+size_t(input->index)*0x198+0x174,r.hdr[slot]);r.core_read[slot*2+1]=ordinary_scene_scope::read(input->core+size_t(input->index)*0x198,r.core_words[slot*2+1]);}}
    }
    // SECOND_EYE_SIDE_EFFECTS 3, checked 26 Oct. Both eyes call the grading step on HDR core index 0
    // (census 26 Oct: grading and grading_index0 both 1.00 mono / 2.00 stereo, so every call is index
    // 0; grading records 17-23 Oct: indices [0,0]), and the step integrates the core's eye-adaptation
    // state (+0x184, four floats): in one dispatch the right eye's result moved on from the left
    // eye's (23 Oct, dispatch 17838: 0.0265 left, 0.0490 right). So adaptation stepped twice per
    // stereo frame, at double speed. The right eye now steps from the state the left eye started from
    // (same inputs, same result: hdr_after matched in both eyes, 23 Oct 10:15, a27cdaa) and the state
    // is put back to the left eye's result afterwards: one step per frame. NativeAdaptationMirror then
    // copies that result to index 1. SM2VR_ORDINARY_SHARED_ADAPTATION=0 turns it off for A/B.
    static constexpr size_t ADAPTATION_OFFSET=0x184;
    using Adaptation=std::array<uint8_t,16>;
    struct AdaptationStats {uint64_t shared{},skipped{};};
    void share_adaptation(bool value){m_share_adaptation=value;}
    AdaptationStats adaptation_stats()const{std::lock_guard lock{m_mutex};return m_adaptation_stats;}
    // OrdinaryAdaptationShare: one line, totals since launch. skipped = right-eye steps left alone.
    std::string adaptation_line()const{const auto s=adaptation_stats();return std::string{"enabled="}+(m_share_adaptation?"true":"false")+" shared="+std::to_string(s.shared)+" skipped="+std::to_string(s.skipped);}
    static std::optional<Input> read_input(const Scope& scope,uintptr_t core,int32_t index,const void* descriptor,uintptr_t grading){
        if(scope.scene.slot>1 || index<0 || index>1 || ordinary_scene_scope::inspect(scope).mask)return {};
        Input input{scope,core,index};
        if(!ordinary_scene_scope::read(grading,input.grading) || !ordinary_scene_scope::read(reinterpret_cast<uintptr_t>(descriptor),input.descriptor))return {};
        float exposure{};std::memcpy(&exposure,input.descriptor.data(),4);
        if(!std::isfinite(exposure) || input.descriptor[4]>1 || input.descriptor[5]>1)return {};
        return input;
    }
    void write(std::ostream& out,uint64_t dispatch){
        std::lock_guard lock{m_mutex};const auto found=std::ranges::find_if(m_records,[&](const auto& r){return r.dispatch==dispatch;});
        if(found==m_records.end()){out<<"null";return;}const auto& r=*found;
        out<<"{\"dispatch\":"<<r.dispatch<<",\"fixed_exposure\":"<<(m_fixed?"true":"false")<<",\"indices\":["<<r.indices[0]<<','<<r.indices[1]<<"],\"exposure\":["<<r.exposure[0]<<','<<r.exposure[1]<<"],\"effects\":["<<r.effects[0]<<','<<r.effects[1]<<"],\"read_overrides\":"<<r.applied<<",\"grading_bytes\":[";
        for(size_t slot=0;slot<2;++slot){if(slot)out<<',';out<<'[';for(size_t i=0;i<21;++i){if(i)out<<',';out<<unsigned(r.grading[slot][i]);}out<<']';}out<<"],\"hdr_after\":[";
        for(size_t slot=0;slot<2;++slot){if(slot)out<<',';if(!r.hdr_read[slot]){out<<"null";continue;}out<<'[';for(size_t i=0;i<8;++i){if(i)out<<',';if(std::isfinite(r.hdr[slot][i]))out<<r.hdr[slot][i];else out<<"null";}out<<']';}out<<"],\"core_words_left_before_after_right_before_after\":[";
        for(size_t k=0;k<4;++k){if(k)out<<',';if(!r.core_read[k]){out<<"null";continue;}out<<'[';for(size_t i=0;i<102;++i){if(i)out<<',';out<<r.core_words[k][i];}out<<']';}out<<"]}";
    }
private:
    // The left eye records the state it starts from and the state it leaves; the right eye of the
    // same job, thread, core and index steps from the first and is reset to the second.
    bool share_adaptation_before(const std::optional<Input>& input){
        if(!m_share_adaptation || !input || input->scope.scene.slot>1)return false;
        std::lock_guard lock{m_mutex};const auto address=input->core+size_t(input->index)*0x198+ADAPTATION_OFFSET;
        if(!input->scope.scene.slot){m_adaptation.reset();Adaptation before{};
            if(ordinary_scene_scope::read(address,before))m_adaptation=AdaptationLeft{GetCurrentThreadId(),input->scope.scene.dispatch_sequence,input->core,input->index,before,std::nullopt};
            return false;}
        const bool same=m_adaptation && m_adaptation->after && m_adaptation->thread==GetCurrentThreadId() && m_adaptation->dispatch==input->scope.scene.dispatch_sequence &&
            m_adaptation->core==input->core && m_adaptation->index==input->index;
        if(!same || !write(address,m_adaptation->before)){++m_adaptation_stats.skipped;m_adaptation.reset();return false;}
        return true;
    }
    void share_adaptation_after(const Input& input,bool shared){
        if(!m_share_adaptation)return;std::lock_guard lock{m_mutex};const auto address=input.core+size_t(input.index)*0x198+ADAPTATION_OFFSET;
        if(!input.scope.scene.slot){Adaptation after{};if(m_adaptation && ordinary_scene_scope::read(address,after))m_adaptation->after=after;else m_adaptation.reset();return;}
        if(shared && m_adaptation && write(address,*m_adaptation->after))++m_adaptation_stats.shared;
        m_adaptation.reset();
    }
    static bool write(uintptr_t address,const Adaptation& value){SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value.data(),value.size(),&n) && n==value.size();}
    static void on_scene(uintptr_t a,uintptr_t b,uintptr_t c,const void* d,int32_t index){
        // grading_index0 counts the index 0 steps that advance the adaptation state; a right-eye step
        // that share_adaptation undoes is not one (NativeCallCensus.h).
        NativeCallCensus::get().count(NativeCallCensus::Hook::grading);if(index==0 && !s_shared_step)NativeCallCensus::get().count(NativeCallCensus::Hook::grading_index0);
        auto& self=get();if(s_active && a==s_active->core && index==1){
            index=s_active->index;d=s_active->grading.data();std::lock_guard lock{self.m_mutex};
            for(auto& r:self.m_records)if(r.dispatch==s_active->scope.scene.dispatch_sequence)++r.applied;
        }
        self.m_original(a,b,c,d,index);
    }
    struct AdaptationLeft {DWORD thread{};uint64_t dispatch{};uintptr_t core{};int32_t index{};Adaptation before{};std::optional<Adaptation> after;};
    std::optional<AdaptationLeft> m_adaptation;AdaptationStats m_adaptation_stats;bool m_share_adaptation{true};
    Native m_original{};bool m_fixed{};mutable std::mutex m_mutex;std::optional<std::pair<uint32_t,Input>> m_left;std::vector<Record> m_records;
    inline static thread_local std::optional<Input> s_active;
    inline static thread_local bool s_shared_step{};
};
