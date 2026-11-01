#pragma once
#include "FrameSyncClock.h"
#include "VrCameraPose.h"
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// Read-only provenance for the actual native add-list slots. It never writes
// frame tags into engine memory and never labels an image by pose proximity.
class NativeCameraEntries {
public:
    enum class Role {other,main,pip,ordinary_right};
    struct Ticket {uintptr_t list{};uint64_t revision{};Role role{};std::optional<FrameSyncClock::Frame> frame;};
    struct Entry {uintptr_t address{};int32_t slot{-1};Role role{};vr_camera::Matrix world{};};
    struct Pair {
        uintptr_t add_list{},render_list{},main_entry{},pip_entry{};
        int32_t main_slot{-1},pip_slot{-1};uint64_t revision{},render_id{};uint32_t token{};
    };
    struct Primary {
        uintptr_t add_list{},render_list{},entry{};
        int32_t slot{-1};uint64_t revision{},render_id{};uint32_t token{};
    };
    struct OrdinaryPair {Primary left,right;uintptr_t producer_list{};uint64_t producer_revision{};};
    struct Status {uintptr_t list{};uint64_t revision{},inflight{},main_calls{},pip_calls{},missing_frames{},render_id{};uint32_t token{};bool valid{};size_t entries{};};
    struct Content {bool valid{};std::optional<FrameSyncClock::Frame> frame;std::vector<Entry> entries;uintptr_t producer_list{};uint64_t producer_revision{};};
    struct CopyTicket {uintptr_t a{},b{};uint64_t revision_a{},revision_b{};Content before_a,before_b;};
    void reset(uintptr_t list) {
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(list);if(found==m_lists.end()) return;
        found->second.valid=false;found->second.entries.clear();found->second.frame.reset();found->second.producer_list=0;found->second.producer_revision=0;found->second.revision=++m_revision;
    }
    std::optional<CopyTicket> begin_copy(uintptr_t a,uintptr_t b) {
        if(!m_enabled || !a || !b || a==b) return std::nullopt;
        std::lock_guard lock{m_mutex};
        const auto needed=size_t(!m_lists.contains(a))+size_t(!m_lists.contains(b));if(m_lists.size()+needed>8) m_lists.clear();
        auto& left=m_lists[a];auto& right=m_lists[b];CopyTicket value;value.a=a;value.b=b;
        const auto copy=[](const auto& source) {Content result;result.valid=source.valid && !source.inflight;result.frame=source.frame;
            result.producer_list=source.producer_list;result.producer_revision=source.producer_revision;
            for(const auto& [slot,entry]:source.entries) result.entries.push_back(entry);return result;};
        value.before_a=copy(left);value.before_b=copy(right);
        left.valid=right.valid=false;++left.inflight;++right.inflight;
        value.revision_a=left.revision=++m_revision;value.revision_b=right.revision=++m_revision;return value;
    }
    bool finish_copy(const CopyTicket& value,bool exact_bytes) {
        std::lock_guard lock{m_mutex};const auto a=m_lists.find(value.a),b=m_lists.find(value.b);
        if(a==m_lists.end() || b==m_lists.end()) return false;
        auto& left=a->second;auto& right=b->second;
        if(left.revision!=value.revision_a || right.revision!=value.revision_b || left.inflight!=1 || right.inflight!=1) {
            left.valid=right.valid=false;if(left.inflight) --left.inflight;if(right.inflight) --right.inflight;return false;
        }
        const auto restore=[&](auto& target,uintptr_t list,const Content& source) {
            target.entries.clear();target.frame=source.frame;target.valid=exact_bytes && source.valid;target.inflight=0;target.revision=++m_revision;
            target.producer_list=source.producer_list;target.producer_revision=source.producer_revision;
            if(target.valid) for(auto entry:source.entries) {const auto address=NativeCameraEntries::address(list,entry.slot);
                if(!address) {target.valid=false;target.entries.clear();break;}entry.address=*address;target.entries.emplace(entry.slot,entry);}
        };
        restore(left,value.a,value.before_b);restore(right,value.b,value.before_b);
        return exact_bytes && (left.valid || right.valid);
    }
    std::vector<Status> status() const {
        std::lock_guard lock{m_mutex};std::vector<Status> result;
        for(const auto& [list,state]:m_lists) result.push_back({list,state.revision,state.inflight,state.main_calls,state.pip_calls,state.missing_frames,
            state.frame?state.frame->render_id:0,state.frame?state.frame->token:0,state.valid,state.entries.size()});
        return result;
    }
    static NativeCameraEntries& get() {static auto* value=new NativeCameraEntries;return *value;}
    void enable() {m_enabled=true;}
    static std::optional<uintptr_t> address(uintptr_t list,int32_t slot) {
        if(!list || slot<0 || slot>=64 || list>UINTPTR_MAX-(8+size_t(slot)*0xa60)) return std::nullopt;
        return list+8+size_t(slot)*0xa60;
    }
    std::optional<Ticket> begin(uintptr_t list,Role role,std::optional<FrameSyncClock::Frame> frame) {
        if(!m_enabled || !list) return std::nullopt;
        std::lock_guard lock{m_mutex};
        if(!m_lists.contains(list) && m_lists.size()>=8) m_lists.clear();
        auto& state=m_lists[list];++state.inflight;state.revision=++m_revision;
        if(role==Role::main) ++state.main_calls;
        if(role==Role::pip) ++state.pip_calls;
        if(role!=Role::other && (!frame || frame->invalid || !frame->render_id)) ++state.missing_frames;
        if(role==Role::main) {
            const bool duplicate=state.frame && frame && state.frame->render_id==frame->render_id;
            state.entries.clear();state.frame=frame;state.valid=frame && !frame->invalid && frame->render_id && !duplicate;
            state.producer_list=list;state.producer_revision=state.revision;
        }
        if(role==Role::ordinary_right && state.producer_list==list)state.producer_revision=state.revision;
        return Ticket{list,state.revision,role,frame};
    }
    void finish(const Ticket& ticket,int32_t slot,std::optional<vr_camera::Matrix> world) {
        std::lock_guard lock{m_mutex};auto found=m_lists.find(ticket.list);if(found==m_lists.end()) return;
        auto& state=found->second;if(state.inflight) --state.inflight;
        // Any overlapping producer may have overwritten a partially read slot.
        if(state.revision!=ticket.revision) {state.valid=false;return;}
        state.entries.erase(slot);const auto entry=address(ticket.list,slot);
        if(!state.valid || ticket.role==Role::other || !entry || !world || !vr_camera::rigid(*world,false) || !ticket.frame ||
           ticket.frame->invalid || !state.frame || ticket.frame->render_id!=state.frame->render_id || ticket.frame->token!=state.frame->token) return;
        state.entries.emplace(slot,Entry{*entry,slot,ticket.role,*world});
    }
    std::optional<uint64_t> revision(uintptr_t render_list) const {
        if(!render_list || render_list>UINTPTR_MAX-0x160) return std::nullopt;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(render_list+0x160);
        if(found==m_lists.end() || !found->second.valid || found->second.inflight) return std::nullopt;
        return found->second.revision;
    }
    std::optional<Pair> pair(uintptr_t render_list,std::span<const Entry> actual,uint64_t before_read) const {
        if(!render_list || render_list>UINTPTR_MAX-0x160) return std::nullopt;
        const auto add_list=render_list+0x160;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(add_list);
        if(found==m_lists.end() || !found->second.valid || found->second.inflight || !found->second.frame || found->second.revision!=before_read) return std::nullopt;
        const auto& state=found->second;Pair result;result.add_list=add_list;result.render_list=render_list;
        result.revision=state.revision;result.render_id=state.frame->render_id;result.token=state.frame->token;
        for(const auto& view:actual) {
            if(view.role==Role::other) continue;
            if(view.role!=Role::main && view.role!=Role::pip)return std::nullopt;
            const auto receipt=state.entries.find(view.slot);
            if(receipt==state.entries.end() || receipt->second.role!=view.role || receipt->second.address!=view.address) return std::nullopt;
            for(size_t i=0;i<16;++i) if(!std::isfinite(view.world[i]) || std::abs(view.world[i]-receipt->second.world[i])>1e-4f) return std::nullopt;
            auto& slot=view.role==Role::main?result.main_slot:result.pip_slot;
            auto& target=view.role==Role::main?result.main_entry:result.pip_entry;
            if(slot!=-1) return std::nullopt;slot=view.slot;target=view.address;
        }
        return result.main_slot>=0 && result.pip_slot>=0?std::optional{result}:std::nullopt;
    }
    bool current(const Pair& receipt) const {
        if(!receipt.render_list || receipt.render_list>UINTPTR_MAX-0x160 || receipt.add_list!=receipt.render_list+0x160 ||
           receipt.main_slot==receipt.pip_slot) return false;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(receipt.add_list);
        if(found==m_lists.end() || !found->second.valid || found->second.inflight || found->second.revision!=receipt.revision ||
           !found->second.frame || found->second.frame->render_id!=receipt.render_id || found->second.frame->token!=receipt.token) return false;
        const auto matches=[&](int32_t slot,uintptr_t pointer,Role role) {
            const auto address=NativeCameraEntries::address(receipt.add_list,slot);const auto entry=found->second.entries.find(slot);
            return address && pointer==*address && entry!=found->second.entries.end() && entry->second.address==pointer && entry->second.role==role;
        };
        return matches(receipt.main_slot,receipt.main_entry,Role::main) && matches(receipt.pip_slot,receipt.pip_entry,Role::pip);
    }
    std::optional<Primary> primary(uintptr_t render_list,std::span<const Entry> actual,uint64_t revision) const {
        if(!render_list || render_list>UINTPTR_MAX-0x160 || actual.size()!=1 || actual[0].role!=Role::main) return std::nullopt;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(render_list+0x160);
        if(found==m_lists.end()) return std::nullopt;const auto& state=found->second;
        if(!state.valid || state.inflight || !state.frame || state.revision!=revision || state.entries.size()!=1) return std::nullopt;
        const auto& view=actual[0];const auto entry=state.entries.find(view.slot);const auto pointer=address(render_list+0x160,view.slot);
        if(entry==state.entries.end() || !pointer || view.address!=*pointer || entry->second.address!=view.address || entry->second.role!=Role::main) return std::nullopt;
        for(size_t i=0;i<16;++i) if(!std::isfinite(view.world[i]) || std::abs(view.world[i]-entry->second.world[i])>1e-4f) return std::nullopt;
        return Primary{render_list+0x160,render_list,view.address,view.slot,revision,state.frame->render_id,state.frame->token};
    }
    bool current(const Primary& value) const {
        if(!value.render_list || value.render_list>UINTPTR_MAX-0x160 || value.add_list!=value.render_list+0x160) return false;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(value.add_list);const auto pointer=address(value.add_list,value.slot);
        if(found==m_lists.end() || !pointer || value.entry!=*pointer) return false;
        const auto& state=found->second;const auto entry=state.entries.find(value.slot);
        return state.valid && !state.inflight && state.revision==value.revision && state.frame && state.entries.size()==1 &&
            state.frame->render_id==value.render_id && state.frame->token==value.token && entry!=state.entries.end() &&
            entry->second.address==value.entry && entry->second.role==Role::main;
    }
    // Engine branch flags identify both as ordinary; only the actual add tickets
    // distinguish the additional ordinary eye. Keep sole-main/PIP contracts strict.
    std::optional<OrdinaryPair> ordinary_pair(uintptr_t render_list,std::span<const Entry> actual,uint64_t revision) const {
        if(!render_list || render_list>UINTPTR_MAX-0x160 || actual.size()!=2)return std::nullopt;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(render_list+0x160);
        if(found==m_lists.end())return std::nullopt;const auto& state=found->second;
        if(!state.valid || state.inflight || !state.frame || state.revision!=revision || state.entries.size()!=2)return std::nullopt;
        if(!state.producer_list || !state.producer_revision)return std::nullopt;
        OrdinaryPair pair;pair.producer_list=state.producer_list;pair.producer_revision=state.producer_revision;
        for(size_t i=0;i<2;++i){const auto& view=actual[i];const auto stored=state.entries.find(int32_t(i));const auto pointer=address(render_list+0x160,int32_t(i));
            if(view.role!=Role::main || view.slot!=int32_t(i) || !pointer || view.address!=*pointer || stored==state.entries.end() || stored->second.address!=view.address || stored->second.role!=(i?Role::ordinary_right:Role::main))return std::nullopt;
            for(size_t n=0;n<16;++n)if(!std::isfinite(view.world[n]) || std::abs(view.world[n]-stored->second.world[n])>1e-4f)return std::nullopt;
            (i?pair.right:pair.left)={render_list+0x160,render_list,*pointer,int32_t(i),revision,state.frame->render_id,state.frame->token};
        }return pair;
    }
    bool current(const OrdinaryPair& pair) const {
        const auto& a=pair.left;const auto& b=pair.right;
        if(!a.render_list || a.render_list>UINTPTR_MAX-0x160 || a.add_list!=a.render_list+0x160 || a.add_list!=b.add_list || a.render_list!=b.render_list || a.revision!=b.revision || a.render_id!=b.render_id || a.token!=b.token || a.slot!=0 || b.slot!=1)return false;
        std::lock_guard lock{m_mutex};const auto found=m_lists.find(a.add_list);if(found==m_lists.end())return false;const auto& state=found->second;
        if(!state.valid || state.inflight || !state.frame || state.entries.size()!=2 || state.revision!=a.revision || state.frame->render_id!=a.render_id || state.frame->token!=a.token || !pair.producer_list || !pair.producer_revision || state.producer_list!=pair.producer_list || state.producer_revision!=pair.producer_revision)return false;
        for(size_t i=0;i<2;++i){const auto& p=i?b:a;const auto pointer=address(a.add_list,int32_t(i));const auto stored=state.entries.find(int32_t(i));
            if(!pointer || p.entry!=*pointer || stored==state.entries.end() || stored->second.address!=p.entry || stored->second.role!=(i?Role::ordinary_right:Role::main))return false;
        }return true;
    }
private:
    struct List {uint64_t revision{},inflight{},main_calls{},pip_calls{},missing_frames{};bool valid{};std::optional<FrameSyncClock::Frame> frame;std::map<int32_t,Entry> entries;uintptr_t producer_list{};uint64_t producer_revision{};};
    mutable std::mutex m_mutex;std::map<uintptr_t,List> m_lists;uint64_t m_revision{};std::atomic_bool m_enabled{};
};
