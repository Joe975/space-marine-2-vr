#pragma once
#include "NativeRenderProbe.h"
#include "GameBuild.h"
#include "OrdinaryCameraMath.h"
#include "OrdinaryTrackedCamera.h"
#include <atomic>
#include <mutex>

// The headset's per-eye camera packet (SM2VR_ORDINARY_NATIVE_PACKET, every headset launch; began as a
// four-frame desktop experiment). Supported camera fields only;
// independent native GPU resources, opaque fields and runtime remain unverified.
class NativeOrdinaryPacket {
public:
    using Block=NativeCameraPacket::Block;using Math=ordinary_camera_math::Result;
    struct Globals {uintptr_t list{};uint32_t counter{},width{},height{};float scale{},depth{};bool operator==(const Globals&)const=default;};
    struct Sdk {uint64_t epoch{},ordinal{};uint32_t token{};bool operator==(const Sdk&)const=default;};
    using Context=std::function<std::optional<Globals>()>;using Accepted=std::function<std::optional<Sdk>(uint64_t)>;
    using Writer=std::function<bool(uintptr_t,const Block&)>;
    struct TrackedSource {ordinary_tracked_camera::Pair prepared;uintptr_t producer_list{};uint64_t producer_revision{};};
    using TrackedProvider=std::function<std::optional<TrackedSource>(uint64_t)>;
    struct Receipt {uint64_t dispatch{},render_id{};uintptr_t renderer{};Globals context;unsigned refusal{};bool reference{},applied{},accepted{},continued{},rollback{},rollback_failed{},source_unchanged{},recoverable{};std::optional<NativeCameraPacket> left_source,original,patched,left_patched;std::optional<Sdk> sdk;std::optional<TrackedSource> tracked;};
    void continuous(bool value){m_continuous=value;}
    // Tracked refusals a level transition or a transient camera mismatch can cause, all before
    // any write: 12 the entries no longer hold the producer's tracked pose (provenance, flags,
    // world/view/projection), 13 the tracked math refuses the inputs, 4 the left eye's current
    // fields differ from the game's own camera (23 Oct, first frame of a level load). In
    // continuous tracked mode such a receipt is marked recoverable and the packet stays armed:
    // the job renders native, nothing was written, and the next job is fully re-checked as a
    // cut. Every other refusal (structure, context, write, rollback, completion, SDK) still stops.
    static bool transient(unsigned refusal){return refusal==4 || refusal==12 || refusal==13;}
    bool tracked(TrackedProvider provider){if(m_armed || !provider)return false;m_tracked=std::move(provider);return true;}
    static NativeOrdinaryPacket& get(){static auto* value=new NativeOrdinaryPacket;return *value;}
    bool arm(Context context,Accepted accepted,Writer writer={}){std::lock_guard lock{m_mutex};if(m_armed || !context || !accepted)return false;m_context=std::move(context);m_accepted=std::move(accepted);m_writer=writer?std::move(writer):Writer{write};m_armed.store(true,std::memory_order_release);return true;}
    bool arm_in_game(HMODULE module,Accepted accepted){
        const auto base=reinterpret_cast<uintptr_t>(module);IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if(!read(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 || !read(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
            !game_build::supported(nt.FileHeader.TimeDateStamp) || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage<0x4ff0c10)return false;
        return arm([base]()->std::optional<Globals>{Globals g;uintptr_t dimensions{};
            if(!read(base+game_build::rva(0x4f7c6c8),g.list) || !read(base+game_build::rva(0x4ff0c08),dimensions) || !g.list || g.list>UINTPTR_MAX-0x29c7c || !dimensions || dimensions>UINTPTR_MAX-0x4c ||
                !read(g.list+0x29c78,g.counter) || !read(g.list+0x34,g.scale) || !read(g.list+0x38,g.depth) || !read(dimensions+0x44,g.width) || !read(dimensions+0x48,g.height))return {};return g;},std::move(accepted));
    }
    void prepare(NativeRenderProbe::Snapshot& snapshot){
        if(!m_armed.load(std::memory_order_acquire) || !snapshot.ordinary_pair)return;
        std::lock_guard lock{m_mutex};if(m_stopped || (!m_continuous && m_receipts.size()>=4) || snapshot.sequence<=m_last_dispatch)return;
        if(m_pending){m_receipts.back().refusal=11;m_pending.reset();m_previous.reset();m_stopped=true;return;}
        if(m_continuous && m_receipts.size()==4)m_receipts.erase(m_receipts.begin());
        if(m_tracked){prepare_tracked(snapshot);return;}
        m_last_dispatch=snapshot.sequence;Receipt r;r.dispatch=snapshot.sequence;r.renderer=snapshot.renderer;r.render_id=snapshot.ordinary_pair->left.render_id;
        const auto fail=[&](unsigned reason){r.refusal=reason;m_receipts.push_back(r);m_stopped=true;};
        const auto pair=*snapshot.ordinary_pair;Block left{},right{};bool touched{};
        const auto rollback=[&]()noexcept{r.rollback=true;try{r.rollback_failed=!m_writer(pair.right.entry,right);Block restored{};r.rollback_failed=r.rollback_failed || !stable(pair.right.entry,restored) || restored!=right;}catch(...){r.rollback_failed=true;}touched=r.rollback_failed;};
        try{
            if(m_pending || snapshot.before.size()!=2 || !NativeCameraEntries::get().current(pair) || pair.left.slot!=0 || pair.right.slot!=1 || pair.left.render_list!=snapshot.list || pair.right.render_list!=snapshot.list || pair.left.entry!=snapshot.list+0x168 || pair.right.entry!=pair.left.entry+0xa60 || !snapshot.renderer){fail(1);return;}
            const auto first=m_context();const auto second=m_context();if(!first || first!=second || first->list!=snapshot.list){fail(2);return;}r.context=*first;
            if(!stable(pair.left.entry,left) || !stable(pair.right.entry,right) || field<uint64_t>(left,0)!=snapshot.before[0].flags || field<uint64_t>(right,0)!=snapshot.before[1].flags || (field<uint64_t>(left,0)&0x50000044) || field<uint64_t>(left,0)!=field<uint64_t>(right,0)){fail(3);return;}
            const auto lworld=field<vr_camera::Matrix>(left,0x34),rworld=field<vr_camera::Matrix>(right,0x34);
            for(size_t i=0;i<16;++i)if(std::abs(lworld[i]-snapshot.before[0].world[i])>1e-5f || std::abs(rworld[i]-snapshot.before[1].world[i])>1e-5f){fail(3);return;}
            for(size_t i=0;i<12;++i)if(std::abs(lworld[i]-rworld[i])>1e-5f){fail(3);return;}
            for(size_t i=0;i<3;++i)if(std::abs(rworld[12+i]-lworld[12+i]-.064f*lworld[i])>3e-4f){fail(3);return;}
            if(field<uint8_t>(left,0xa34)>1 || field<uint8_t>(left,0xa35)>1){fail(4);return;}
            auto input=make_input(left,r.context);const bool continuous=m_previous && m_previous->renderer==snapshot.renderer && m_previous->render_id+1==r.render_id && uint32_t(m_previous->counter+1)==r.context.counter;
            if(continuous)input.previous=m_previous->left;
            else {ordinary_camera_math::Previous previous;previous.projection=field<vr_camera::Matrix>(left,0x834);previous.view=field<vr_camera::Matrix>(left,0x8f4);previous.world=vr_camera::inverse(previous.view);
                auto jitter=vr_camera::IDENTITY;jitter[12]=-2*input.jitter[0]/float(input.width);jitter[13]=2*input.jitter[1]/float(input.height);
                previous.combined=vr_camera::multiply(field<vr_camera::Matrix>(left,0x934),jitter);previous.raster_combined=field<vr_camera::Matrix>(left,0x974);input.previous=previous;}
            const auto reference=ordinary_camera_math::build(input);if(!reference){fail(4);return;}
            for(auto offset:Math::OFFSETS)if(!same(reference->at(offset),field<vr_camera::Matrix>(left,offset),offset==0x9b4 || offset==0x9f4?1e-3f:3e-4f)){fail(4);return;}
            r.reference=true;
            if(!same(field<vr_camera::Matrix>(right,0x45c),input.projection,1e-5f)){fail(5);return;}
            auto right_input=input;right_input.world=rworld;right_input.view=field<vr_camera::Matrix>(right,0x3dc);right_input.previous=continuous?std::optional{m_previous->right}:std::nullopt;
            right_input.cut=field<uint8_t>(left,0xa35)!=0;const auto result=ordinary_camera_math::build(right_input);if(!result){fail(5);return;}
            Block patched=right;for(auto offset:Math::OFFSETS)put(patched,offset,result->at(offset));put(patched,0x6a8,result->jitter);put(patched,0x6b0,result->jitter_index);
            put(patched,0xa38,result->delta);put(patched,0xa44,result->angles);put(patched,0xa34,uint8_t(result->moved));put(patched,0xa35,uint8_t(result->cut));
            r.left_source=NativeCameraPacket::capture(pair.left.entry);r.original=NativeCameraPacket::capture(pair.right.entry);if(!r.left_source || !r.original || !writable(pair.right.entry,patched.size()) || !same_current(snapshot,pair,*first,left,right)){fail(6);return;}
            touched=true;if(!m_writer(pair.right.entry,patched)){rollback();fail(7);return;}
            Block after{},source{};r.source_unchanged=stable(pair.left.entry,source) && source==left;
            if(!stable(pair.right.entry,after) || after!=patched || !r.source_unchanged || m_context()!=first || !NativeCameraEntries::get().current(pair)){rollback();fail(7);return;}
            r.patched=NativeCameraPacket::capture(pair.right.entry);const auto views=NativeRenderProbe::read_views(snapshot.list);
            if(!r.patched || !views || views->size()!=2){rollback();fail(7);return;}
            snapshot.before=*views;r.applied=true;r.continued=continuous && !right_input.cut;
            m_pending=Pending{r,*reference,*result};m_receipts.push_back(r);
        }catch(...){if(touched)rollback();m_pending.reset();fail(10);}
    }
    void complete(const NativeRenderProbe::Snapshot& snapshot){
        if(!m_armed.load(std::memory_order_acquire))return;std::lock_guard lock{m_mutex};if(!m_pending || m_pending->receipt.dispatch!=snapshot.sequence)return;
        auto& receipt=m_receipts.back();const auto pending=*m_pending;m_pending.reset();
        try{
            if(!snapshot.ordinary_pair || !NativeCameraEntries::get().current(*snapshot.ordinary_pair) || snapshot.scene_calls!=2 || snapshot.scene_refused || snapshot.scene_dropped || snapshot.scenes.size()!=2 || m_context()!=std::optional{receipt.context}){receipt.refusal=8;m_stopped=true;return;}
            for(size_t i=0;i<2;++i){const auto& s=snapshot.scenes[i];const auto& eye=i?snapshot.ordinary_pair->right:snapshot.ordinary_pair->left;
                if(!s.valid() || s.slot!=i || s.entry!=eye.entry || s.renderer!=receipt.renderer || s.list!=receipt.context.list || s.camera_revision_before!=eye.revision || s.camera_revision_after!=eye.revision ||
                    s.render_counter_before!=receipt.context.counter || s.render_counter_after!=receipt.context.counter || !s.packet_before || s.packet_before!=s.packet_after || s.packet_before!=(i?receipt.patched:(receipt.left_patched?receipt.left_patched:receipt.left_source))){receipt.refusal=8;m_stopped=true;return;}}
            const auto sdk=m_accepted(snapshot.sequence);if(!sdk || !sdk->epoch || !sdk->ordinal || (receipt.continued && (!m_previous || sdk->epoch!=m_previous->sdk.epoch || sdk->ordinal!=m_previous->sdk.ordinal+1 || sdk->token!=uint32_t(m_previous->sdk.token+1)))){receipt.refusal=9;m_stopped=true;return;}
            receipt.sdk=*sdk;receipt.accepted=true;m_previous=Previous{pending.left.previous(),pending.right.previous(),receipt.renderer,receipt.render_id,receipt.context.counter,*sdk};
            m_tracked_history=pending.tracked_history;
        }catch(...){receipt.refusal=10;m_stopped=true;}
    }
    std::optional<Receipt> receipt(uint64_t dispatch)const{std::lock_guard lock{m_mutex};for(const auto& r:m_receipts)if(r.dispatch==dispatch)return r;return {};}
    // The job's receipt is a recoverable refusal: it renders native, is never shown, and must not
    // enter the GPU pair retention (whose completion would refuse it and stop the producer).
    bool recoverable(uint64_t dispatch)const{std::lock_guard lock{m_mutex};for(const auto& r:m_receipts)if(r.dispatch==dispatch)return r.recoverable && r.refusal && !r.applied;return false;}
    static bool recoverable_job(uint64_t dispatch){return get().recoverable(dispatch);} // StreamlineHistoryProbe::skip_private
    // Cold restart of owned stereo (OrdinaryColdRestart): lift a stop as at arming. The next job
    // has no predecessor, so it is fully checked as a cut. Refused while a job is pending (its
    // write still has to be rolled back by its completion).
    bool restart(){std::lock_guard lock{m_mutex};if(!m_armed.load() || m_pending)return false;m_stopped=false;m_previous.reset();++m_restarts;return true;}
    bool stopped()const{std::lock_guard lock{m_mutex};return m_stopped;}
    uint64_t restarts()const{std::lock_guard lock{m_mutex};return m_restarts;}
    void write_receipt(std::ostream& out,uint64_t dispatch)const{
        const auto r=receipt(dispatch);if(!r){out<<"null";return;}const auto boolean=[&](const char* name,bool value){out<<",\""<<name<<"\":"<<(value?"true":"false");};
        out<<"{\"model\":\"ordinary_native_packet_transaction_v1\",\"dispatch\":"<<r->dispatch<<",\"render_id\":"<<r->render_id<<",\"renderer\":"<<r->renderer<<",\"list\":"<<r->context.list<<",\"counter\":"<<r->context.counter<<",\"width\":"<<r->context.width<<",\"height\":"<<r->context.height<<",\"auxiliary_scale\":"<<r->context.scale<<",\"depth_remap\":"<<r->context.depth<<",\"refusal\":"<<r->refusal;
        boolean("tracked",r->tracked.has_value());boolean("left_rebuilt",r->left_patched.has_value());boolean("reference_pass",r->reference);boolean("applied",r->applied);boolean("accepted",r->accepted);boolean("continued",r->continued);boolean("rollback",r->rollback);boolean("rollback_failed",r->rollback_failed);boolean("source_unchanged",r->source_unchanged);boolean("recoverable",r->recoverable);
        out<<",\"left_source\":";if(r->left_source)r->left_source->write(out);else out<<"null";out<<",\"original\":";if(r->original)r->original->write(out);else out<<"null";out<<",\"patched\":";if(r->patched)r->patched->write(out);else out<<"null";
        out<<",\"unsupported_offsets\":[\"0x5dc\",\"0x874\",\"0x8b4\"],\"native_gpu_histories_verified\":false,\"gpu_pair_verified\":false,\"runtime_submission\":false}";
    }
private:
    struct Previous {ordinary_camera_math::Previous left,right;uintptr_t renderer{};uint64_t render_id{};uint32_t counter{};Sdk sdk;};
    struct Pending {Receipt receipt;Math left,right;std::array<std::optional<ordinary_camera_math::TrackedHistory>,2> tracked_history;};
    void prepare_tracked(NativeRenderProbe::Snapshot& snapshot){
        m_last_dispatch=snapshot.sequence;Receipt r;r.dispatch=snapshot.sequence;r.renderer=snapshot.renderer;
        const auto pair=*snapshot.ordinary_pair;r.render_id=pair.left.render_id;
        std::array<Block,2> originals{},patched{};std::array<uintptr_t,2> addresses{pair.left.entry,pair.right.entry};bool touched{};
        const auto fail=[&](unsigned reason){r.refusal=reason;r.recoverable=m_continuous && !touched && transient(reason);m_receipts.push_back(r);if(!r.recoverable)m_stopped=true;};
        const auto rollback=[&]()noexcept{r.rollback=true;for(size_t i=0;i<2;++i)try{Block restored{};if(!m_writer(addresses[i],originals[i]) || !stable(addresses[i],restored) || restored!=originals[i])r.rollback_failed=true;}catch(...){r.rollback_failed=true;}};
        try{
            if(snapshot.before.size()!=2 || !NativeCameraEntries::get().current(pair) || pair.left.slot!=0 || pair.right.slot!=1 || pair.left.render_list!=snapshot.list || pair.right.render_list!=snapshot.list || addresses[0]!=snapshot.list+0x168 || addresses[1]!=addresses[0]+0xa60 || !snapshot.renderer){fail(1);return;}
            const auto globals=m_context();if(!globals || m_context()!=globals || globals->list!=snapshot.list){fail(2);return;}r.context=*globals;
            const auto source=m_tracked(r.render_id);
            if(!source || source->producer_list!=pair.producer_list || source->producer_revision!=pair.producer_revision || source->prepared.render_id!=r.render_id || source->prepared.game_token!=pair.left.token){fail(12);return;}
            r.tracked=source;
            const bool continued=m_previous && m_previous->renderer==r.renderer && m_previous->render_id+1==r.render_id && uint32_t(m_previous->counter+1)==r.context.counter;
            std::array<ordinary_camera_math::TrackedResult,2> results;
            for(size_t i=0;i<2;++i){const auto& eye=source->prepared.eyes[i];
                if(!stable(addresses[i],originals[i]) || !eye.tracking.fov || eye.tracking.eye!=int(i) || eye.tracking.render_token!=r.render_id ||
                   field<uint64_t>(originals[i],0)!=snapshot.before[i].flags || (snapshot.before[i].flags&0x50000044) || snapshot.before[i].flags!=snapshot.before[0].flags ||
                   field<vr_camera::Matrix>(originals[i],0x34)!=eye.world || field<vr_camera::Matrix>(originals[i],0x3dc)!=eye.view || snapshot.before[i].world!=eye.world ||
                   !same(field<vr_camera::Matrix>(originals[i],i?0x45c:0x49c),eye.projection,1e-5f)){fail(12);return;}
                ordinary_camera_math::TrackedInput input;input.native=make_input(originals[0],r.context);
                input.native.world=eye.world;input.native.view=eye.view;input.native.projection=eye.projection;
                input.lens.fov=*eye.tracking.fov;std::memcpy(&input.lens.near_z,eye.source.data()+0x80,4);std::memcpy(&input.lens.far_z,eye.source.data()+0x84,4);
                if(continued)input.previous=m_tracked_history[i];
                input.native.cut=!continued || field<uint8_t>(originals[0],0xa35)!=0;
                if(input.previous && (input.previous->lens!=input.lens || input.previous->width!=input.native.width || input.previous->height!=input.native.height))input.native.cut=true;
                const auto result=ordinary_camera_math::build_tracked(input);if(!result){fail(13);return;}results[i]=*result;
                // Validate independently calculated current left fields against the native builder.
                // Previous fields are intentionally replaced with this eye's accepted predecessor.
                if(!i)for(auto offset:Math::OFFSETS)if(offset<=0x774 && !same(result->packet.at(offset),field<vr_camera::Matrix>(originals[0],offset),3e-4f)){fail(4);return;}
                patched[i]=originals[i];const auto& math=result->packet;
                for(auto offset:Math::OFFSETS)put(patched[i],offset,math.at(offset));put(patched[i],0x6a8,math.jitter);put(patched[i],0x6b0,math.jitter_index);
                put(patched[i],0xa38,math.delta);put(patched[i],0xa44,math.angles);put(patched[i],0xa34,uint8_t(math.moved));put(patched[i],0xa35,uint8_t(math.cut));
            }
            r.reference=true;r.left_source=NativeCameraPacket::capture(addresses[0]);r.original=NativeCameraPacket::capture(addresses[1]);
            if(!r.left_source || !r.original || !writable(addresses[0],sizeof(Block)) || !writable(addresses[1],sizeof(Block)) || !same_current(snapshot,pair,*globals,originals[0],originals[1])){fail(6);return;}
            touched=true;
            for(size_t i=0;i<2;++i){Block after{};if(!m_writer(addresses[i],patched[i]) || !stable(addresses[i],after) || after!=patched[i]){rollback();fail(7);return;}}
            if(m_context()!=globals || !NativeCameraEntries::get().current(pair)){rollback();fail(7);return;}
            r.left_patched=NativeCameraPacket::capture(addresses[0]);r.patched=NativeCameraPacket::capture(addresses[1]);const auto views=NativeRenderProbe::read_views(snapshot.list);
            if(!r.left_patched || !r.patched || !views || views->size()!=2){rollback();fail(7);return;}
            r.source_unchanged=false;r.applied=true;r.continued=continued && !results[0].packet.cut && !results[1].packet.cut;snapshot.before=*views;
            m_pending=Pending{r,results[0].packet,results[1].packet,{results[0].history(),results[1].history()}};m_receipts.push_back(r);
        }catch(...){if(touched)rollback();m_pending.reset();fail(10);}
    }
    template<class T>static bool read(uintptr_t address,T& out){SIZE_T n{};return address && address<=UINTPTR_MAX-sizeof(T) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&out,sizeof(out),&n) && n==sizeof(out);}
    template<class T>static T field(const Block& b,size_t offset){T value{};std::memcpy(&value,b.data()+offset,sizeof(value));return value;}
    template<class T>static void put(Block& b,size_t offset,const T& value){std::memcpy(b.data()+offset,&value,sizeof(value));}
    static bool stable(uintptr_t address,Block& out){Block after{};return read(address,out) && read(address,after) && out==after;}
    static bool same(const vr_camera::Matrix& a,const vr_camera::Matrix& b,float tolerance){for(size_t i=0;i<16;++i)if(!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i]-b[i])>tolerance)return false;return true;}
    static bool writable(uintptr_t address,size_t size){if(!address || address>UINTPTR_MAX-size)return false;const auto end=address+size;
        while(address<end){MEMORY_BASIC_INFORMATION memory{};if(!VirtualQuery(reinterpret_cast<void*>(address),&memory,sizeof(memory)) || memory.State!=MEM_COMMIT || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(memory.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
            const auto next=reinterpret_cast<uintptr_t>(memory.BaseAddress)+memory.RegionSize;if(next<=address)return false;address=next;}return true;}
    static bool write(uintptr_t address,const Block& value){SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value.data(),value.size(),&n) && n==value.size();}
    static ordinary_camera_math::Input make_input(const Block& block,const Globals& g){ordinary_camera_math::Input input;input.world=field<vr_camera::Matrix>(block,0x34);input.view=field<vr_camera::Matrix>(block,0x3dc);input.projection=field<vr_camera::Matrix>(block,0x49c);input.jitter=field<std::array<float,2>>(block,0x6a8);input.jitter_index=field<uint32_t>(block,0x6b0);input.width=g.width;input.height=g.height;input.auxiliary_scale=g.scale;input.depth_remap=g.depth;return input;}
    bool same_current(const NativeRenderProbe::Snapshot& s,const NativeCameraEntries::OrdinaryPair& pair,const Globals& g,const Block& left,const Block& right){Block a{},b{};return m_context()==std::optional{g} && NativeCameraEntries::get().current(pair) && pair.left.render_list==s.list && stable(pair.left.entry,a) && stable(pair.right.entry,b) && a==left && b==right;}
    mutable std::mutex m_mutex;std::atomic_bool m_armed{};Context m_context;Accepted m_accepted;Writer m_writer;bool m_stopped{};uint64_t m_last_dispatch{},m_restarts{};std::vector<Receipt> m_receipts;std::optional<Pending> m_pending;std::optional<Previous> m_previous;
    bool m_continuous{};TrackedProvider m_tracked;std::array<std::optional<ordinary_camera_math::TrackedHistory>,2> m_tracked_history;
};
