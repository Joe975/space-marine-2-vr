#include <Windows.h>
#include "ReplayExtractSupport.h"
#include "renderdoc_replay.h"
#include "ReplayOutputPreview.h"
#include <iostream>
#include <map>
#include <set>
#include <vector>
#ifndef REPLAY_EXTRACT_CPU_TEST
REPLAY_PROGRAM_MARKER();
#endif
namespace {
using namespace replay_extract;
std::string text(const rdcstr& s){return {s.c_str(),s.size()};}
std::string id(ResourceId r){static_assert(sizeof(r)==8,"opaque resource representation changed");uint64_t bits{};std::memcpy(&bits,&r,8);std::ostringstream s;s<<"0x"<<std::hex<<bits;return s.str();}
Json format(const ResourceFormat& f){return replay_extract::format(uint32_t(f.type),f.compCount,f.compByteWidth,uint32_t(f.compType),f.BGRAOrder(),f.SRGBCorrected());}
#ifndef REPLAY_EXTRACT_CPU_TEST
struct Globals {Globals(){RENDERDOC_InitialiseReplay(GlobalEnvironment{},{});}~Globals(){RENDERDOC_ShutdownReplay();}};
struct Session {
    Globals globals;Owned<ICaptureFile> capture{RENDERDOC_OpenCaptureFile()};Owned<IReplayController> replay;
    explicit Session(const fs::path& path){check(bool(capture),"OpenCaptureFile returned null");check(fs::is_regular_file(path) && fs::file_size(path)<=64ULL*1024*1024*1024,"capture missing or exceeds64GiB");
        const auto opened=capture->OpenFile(path.string().c_str(),"rdc",nullptr);if(!opened.OK())throw std::runtime_error("OpenFile failed code="+std::to_string(int(opened.code)));
        auto result=capture->OpenCapture(ReplayOptions{},nullptr);replay.reset(result.second);check(result.first.OK() && bool(replay),"OpenCapture failed");capture.reset();
        check(replay->GetAPIProperties().pipelineType==GraphicsAPI::D3D12,"only actual D3D12 capture supported");}
};
#endif
template<class Controller> struct Extract {
    Controller& c;Output& out;Options options;std::map<ResourceId,uint64_t> buffers;size_t action_count{},constant_count{};bool found{};Deadline deadline;
    std::vector<uint32_t> scan_events;uint64_t scan_non_draws{},scan_outside{};
    struct ScanLimits {uint64_t bytes=8*1024*1024;size_t sources=128,candidates=64;} scan_limits;
    std::string str(const rdcstr& s){return out.budget.text(text(s));}
    Json actions(const rdcarray<ActionDescription>& list,unsigned depth=0,uint32_t parent=0){
        check(depth<=128,"action hierarchy depth cap exceeded");Json rows=Json::array();
        for(const auto& a:list){deadline.check_time();check(++action_count<=100000,"action cap exceeded");if(a.eventId==options.event && options.event){check(!found,"ambiguous exact action EID");found=true;}
            if(options.command=="scan"){if(a.eventId>=options.first && a.eventId<=options.last){if((uint32_t(a.flags)&uint32_t(ActionFlags::Drawcall))!=0)scan_events.push_back(a.eventId);else ++scan_non_draws;}else ++scan_outside;}
            Json row={{"event",a.eventId},{"parent_event",parent},{"depth",depth},{"flags",uint32_t(a.flags)},{"name",str(a.GetName(c.GetStructuredFile()))},{"count",a.numIndices},{"instances",a.numInstances},{"index_offset",a.indexOffset},{"vertex_offset",a.vertexOffset},{"base_vertex",a.baseVertex},{"instance_offset",a.instanceOffset},{"depth_output",id(a.depthOut)}};
            row["outputs"]=Json::array();for(auto r:a.outputs)row["outputs"].push_back(id(r));rows.push_back(std::move(row));
            for(auto& child:actions(a.children,depth+1,a.eventId))rows.push_back(std::move(child));
        }return rows;
    }
    void load_buffers(){const auto& all=c.GetBuffers();check(all.size()<=100000,"buffer metadata cap exceeded");for(const auto& b:all)buffers.emplace(b.resourceId,b.length);}
    Json buffer(ResourceId r,uint64_t offset,uint64_t size,const std::string& stem,bool mesh_output=false){
        deadline.check_time();
        Json j={{"resource",id(r)},{"byte_offset",offset},{"view_byte_size",size}};
        if(r==ResourceId::Null()){j["bytes_status"]="unbound";return j;}const auto b=buffers.find(r);
        uint64_t n{};
        if(b==buffers.end()){
            if(!mesh_output){j["bytes_status"]="not_in_buffer_inventory_length_unavailable";return j;}
            j["provenance"]="GetPostVSData_mesh_view";j["resource_length_verified"]=false;
            if(!size || size==UINT64_MAX || offset>UINT64_MAX-size){j["bytes_status"]="mesh_range_unavailable";return j;}n=std::min(size,options.bytes);
        }else{
            j["resource_length"]=b->second;j["resource_length_verified"]=true;
            if(offset>b->second){j["bytes_status"]="invalid_view_offset";return j;}
            if(size>b->second-offset){j["bytes_status"]="unsupported_or_invalid_view_size";return j;}
            n=bounded_span(offset,size,b->second,options.bytes);
        }
        j["exported_bytes"]=n;j["prefix_only"]=n<size;
        if(!n){j["bytes_status"]="empty_view";return j;}
        check(n<=MAX_TOTAL-out.budget.total && out.budget.files<MAX_FILES,"buffer export budget exceeded before read");
        auto data=c.GetBufferData(r,offset,n);check(data.size()==n,"GetBufferData returned short/unavailable bytes");const auto name=stem+".bin";out.bytes(name,data.data(),data.size());j["file"]=name;j["bytes_status"]="exported";return j;
    }
    Json signatures(const rdcarray<SigParameter>& list){check(list.size()<=256,"signature count cap exceeded");Json j=Json::array();for(const auto& s:list)j.push_back({{"name",str(s.varName)},{"semantic",str(s.semanticName)},{"semantic_index",s.semanticIndex},{"register",s.regIndex},{"components",s.compCount},{"var_type",uint32_t(s.varType)},{"system_value",uint32_t(s.systemValue)}});return j;}
    Json constants(const rdcarray<ShaderConstant>& list,unsigned depth=0){check(depth<=8 && list.size()<=1024,"constant reflection bounds exceeded");Json j=Json::array();for(const auto& v:list){check(++constant_count<=4096,"constant reflection total exceeded");j.push_back({{"name",str(v.name)},{"byte_offset",v.byteOffset},{"type_name",str(v.type.name)},{"base_type",uint32_t(v.type.baseType)},{"rows",v.type.rows},{"columns",v.type.columns},{"elements",v.type.elements},{"array_byte_stride",v.type.arrayByteStride},{"matrix_byte_stride",v.type.matrixByteStride},{"row_major",v.type.RowMajor()},{"members",constants(v.type.members,depth+1)}});}return j;}
    template<class T> Json resource_reflection(const rdcarray<T>& list){check(list.size()<=512,"resource reflection cap exceeded");Json j=Json::array();for(const auto& r:list)j.push_back({{"name",str(r.name)},{"fixed_bind_number",r.fixedBindNumber},{"fixed_bind_space",r.fixedBindSetOrSpace},{"array_size",r.bindArraySize}});return j;}
    Json shader(const D3D12Pipe::Shader& stage,ResourceId pipeline,const std::string& name){
        Json j={{"shader",id(stage.resourceId)},{"stage_enum",uint32_t(stage.stage)}};
        if(stage.resourceId==ResourceId::Null()){j["status"]="unbound";return j;}const auto* r=stage.reflection;if(!r){j["status"]="reflection_unavailable";return j;}
        j["status"]="available";j["entry_point"]=str(r->entryPoint);j["encoding"]=uint32_t(r->encoding);j["inputs"]=signatures(r->inputSignature);j["outputs"]=signatures(r->outputSignature);
        check(r->constantBlocks.size()<=512,"constant block cap exceeded");j["constant_blocks"]=Json::array();for(const auto& b:r->constantBlocks)j["constant_blocks"].push_back({{"name",str(b.name)},{"fixed_bind_number",b.fixedBindNumber},{"fixed_bind_space",b.fixedBindSetOrSpace},{"byte_size",b.byteSize},{"variables",constants(b.variables)}});
        j["read_only_resources"]=resource_reflection(r->readOnlyResources);j["read_write_resources"]=resource_reflection(r->readWriteResources);
        check(r->rawBytes.size()<=8*1024*1024,"shader bytecode cap exceeded");if(r->rawBytes.empty())j["bytecode_status"]="unavailable";else{out.bytes(name+".bytecode",r->rawBytes.data(),r->rawBytes.size());j["bytecode_file"]=name+".bytecode";}
        deadline.check_time();const auto dis=c.DisassembleShader(pipeline,r,"");check(dis.size()<=4*1024*1024,"disassembly cap exceeded");if(dis.empty())j["disassembly_status"]="unavailable";else{out.bytes(name+".disassembly.txt",dis.c_str(),dis.size());j["disassembly_file"]=name+".disassembly.txt";j["disassembly_status"]="returned_text_not_independently_validated";}
        return j;
    }
    Json descriptors(){
        const auto& accesses=c.GetDescriptorAccess();check(accesses.size()<=128,"descriptor access cap exceeded");Json rows=Json::array();size_t index{};
        for(const auto& a:accesses){deadline.check_time();Json j={{"stage_enum",uint32_t(a.stage)},{"type_enum",uint32_t(a.type)},{"reflection_index",a.index},{"array_element",a.arrayElement},{"store",id(a.descriptorStore)},{"store_byte_offset",a.byteOffset},{"descriptor_bytes",a.byteSize},{"statically_unused",a.staticallyUnused}};
            if(a.descriptorStore==ResourceId::Null() || !a.byteSize){j["status"]="descriptor_location_unavailable";rows.push_back(j);continue;}
            check(uint64_t(a.byteOffset)+a.byteSize<=UINT32_MAX,"descriptor range overflow");rdcarray<DescriptorRange> ranges;ranges.push_back(DescriptorRange(a));const auto data=c.GetDescriptors(a.descriptorStore,ranges);
            check(data.size()==1,"exact descriptor unavailable");const auto& d=data[0];j["status"]="available";j["descriptor_type_enum"]=uint32_t(d.type);j["view"]=id(d.view);j["secondary"]=id(d.secondary);j["format"]=format(d.format);j["element_bytes"]=d.elementByteSize;
            j["binding"]=buffer(d.resource,d.byteOffset,d.byteSize,"descriptor_"+std::to_string(index++));rows.push_back(std::move(j));
        }return rows;
    }
    Json target(const Descriptor& d){return {{"resource",id(d.resource)},{"view",id(d.view)},{"type_enum",uint32_t(d.type)},{"format",format(d.format)},{"first_mip",d.firstMip},{"mip_count",d.numMips},{"first_slice",d.firstSlice},{"slice_count",d.numSlices},{"byte_offset",d.byteOffset},{"byte_size",d.byteSize}};}
    Json stencil(const StencilFace& f){return {{"compare_enum",uint32_t(f.function)},{"reference",f.reference},{"compare_mask",f.compareMask},{"write_mask",f.writeMask},{"fail_enum",uint32_t(f.failOperation)},{"depth_fail_enum",uint32_t(f.depthFailOperation)},{"pass_enum",uint32_t(f.passOperation)}};}
    template<class State> Json output_state(const State& p){
        const auto& r=p.rasterizer;const auto& om=p.outputMerger;const auto& d=om.depthStencilState;
        check(r.viewports.size()<=16 && r.scissors.size()<=16 && om.renderTargets.size()<=8,"raster/output metadata cap exceeded");
        Json j={{"raster",{{"fill_enum",uint32_t(r.state.fillMode)},{"cull_enum",uint32_t(r.state.cullMode)},{"front_ccw",r.state.frontCCW},{"depth_clip",r.state.depthClip},{"depth_bias",r.state.depthBias},{"depth_bias_clamp",r.state.depthBiasClamp},{"slope_scaled_depth_bias",r.state.slopeScaledDepthBias},{"sample_mask",r.sampleMask},{"viewports",Json::array()},{"scissors",Json::array()}}},
            {"depth_stencil",{{"depth_test",d.depthEnable},{"depth_write",d.depthWrites},{"depth_compare_enum",uint32_t(d.depthFunction)},{"depth_bounds_enable",d.depthBoundsEnable},{"min_depth_bounds",d.minDepthBounds},{"max_depth_bounds",d.maxDepthBounds},{"stencil_test",d.stencilEnable},{"front",stencil(d.frontFace)},{"back",stencil(d.backFace)},{"depth_read_only",om.depthReadOnly},{"stencil_read_only",om.stencilReadOnly}}},
            {"colour_targets",Json::array()},{"depth_target",target(om.depthTarget)}};
        for(const auto& v:r.viewports)j["raster"]["viewports"].push_back({{"enabled",v.enabled},{"x",v.x},{"y",v.y},{"width",v.width},{"height",v.height},{"min_depth",v.minDepth},{"max_depth",v.maxDepth}});
        for(const auto& v:r.scissors)j["raster"]["scissors"].push_back({{"enabled",v.enabled},{"x",v.x},{"y",v.y},{"width",v.width},{"height",v.height}});
        for(const auto& t:om.renderTargets)j["colour_targets"].push_back(target(t));return j;
    }
    Json event(){check(found,"exact positive action EID not found");deadline.check_time();c.SetFrameEvent(options.event,true);const auto* p=c.GetD3D12PipelineState();check(p!=nullptr,"D3D12 state unavailable");
        Json preview=options.preview?event_output_preview(c,out,deadline,options.event,*p,options.preview_slot):Json{{"status","not_requested"}};
        Json j={{"event",options.event},{"output_preview",std::move(preview)},{"pipeline",id(p->pipelineResourceId)},{"vs",shader(p->vertexShader,p->pipelineResourceId,"vs")},{"ps",shader(p->pixelShader,p->pipelineResourceId,"ps")}};
        j["output_state"]=output_state(*p);
        check(p->rootSignature.parameters.size()<=64,"root parameter cap exceeded");j["root_signature"]={{"resource",id(p->rootSignature.resourceId)},{"parameters",Json::array()}};
        for(const auto& r:p->rootSignature.parameters){check(r.constants.size()<=256 && r.tableRanges.size()<=64,"root data/range cap exceeded");Json v={{"visibility_mask",uint32_t(r.visibility)},{"register",r.reg},{"space",r.space},{"heap",id(r.heap)},{"heap_byte_offset",r.heapByteOffset},{"root_descriptor",{{"type_enum",uint32_t(r.descriptor.type)},{"resource",id(r.descriptor.resource)},{"byte_offset",r.descriptor.byteOffset},{"byte_size",r.descriptor.byteSize}}},{"constant_bytes",Json::array()},{"table_ranges",Json::array()}};
            for(auto b:r.constants)v["constant_bytes"].push_back(b);for(const auto& a:r.tableRanges)v["table_ranges"].push_back({{"category_enum",uint32_t(a.category)},{"space",a.space},{"base_register",a.baseRegister},{"count",a.count},{"table_byte_offset",a.tableByteOffset}});j["root_signature"]["parameters"].push_back(v);}
        const auto& ia=p->inputAssembly;check(ia.layouts.size()<=64 && ia.vertexBuffers.size()<=32,"IA metadata cap exceeded");j["ia"]={{"topology_enum",uint32_t(ia.topology)},{"strip_cut",ia.indexStripCutValue},{"index_stride",ia.indexBuffer.byteStride},{"index_buffer",buffer(ia.indexBuffer.resourceId,ia.indexBuffer.byteOffset,ia.indexBuffer.byteSize,"ia_index")}};
        j["ia"]["attributes"]=Json::array();for(const auto& a:ia.layouts)j["ia"]["attributes"].push_back({{"semantic",str(a.semanticName)},{"semantic_index",a.semanticIndex},{"slot",a.inputSlot},{"byte_offset",a.byteOffset},{"format",format(a.format)},{"per_instance",a.perInstance},{"instance_step",a.instanceDataStepRate}});
        j["ia"]["vertex_buffers"]=Json::array();size_t i{};for(const auto& b:ia.vertexBuffers){auto v=buffer(b.resourceId,b.byteOffset,b.byteSize,"ia_vertex_"+std::to_string(i));v["slot"]=i++;v["stride"]=b.byteStride;j["ia"]["vertex_buffers"].push_back(v);}
        j["descriptor_accesses"]=descriptors();
        if(options.postvs){const auto mesh=c.GetPostVSData(0,0,MeshDataStage::VSOut);j["postvs"]={{"instance",0},{"view",0},{"stage","VSOut"},{"count",mesh.numIndices},{"vertex_stride",mesh.vertexByteStride},{"index_stride",mesh.indexByteStride},{"base_vertex",mesh.baseVertex},{"format",format(mesh.format)},{"unproject",mesh.unproject}};
            j["postvs"]["vertices"]=buffer(mesh.vertexResourceId,mesh.vertexByteOffset,mesh.vertexByteSize,"postvs_vertices",true);j["postvs"]["indices"]=buffer(mesh.indexResourceId,mesh.indexByteOffset,mesh.indexByteSize,"postvs_indices",true);}
        else j["postvs"]={{"status","not_requested"}};return j;
    }
    const D3D12Pipe::Shader* stage(const D3D12Pipe::State& p,ShaderStage st){switch(st){case ShaderStage::Vertex:return &p.vertexShader;case ShaderStage::Hull:return &p.hullShader;case ShaderStage::Domain:return &p.domainShader;case ShaderStage::Geometry:return &p.geometryShader;case ShaderStage::Pixel:return &p.pixelShader;case ShaderStage::Compute:return &p.computeShader;case ShaderStage::Amplification:return &p.ampShader;case ShaderStage::Mesh:return &p.meshShader;default:return nullptr;}}
    Json block(const D3D12Pipe::State& p,const DescriptorAccess& access){
        Json j={{"status","unavailable"}};const auto* shader=stage(p,access.stage);if(!shader)return j;j["shader"]=id(shader->resourceId);
        if(!shader->reflection || access.index>=shader->reflection->constantBlocks.size())return j;const auto& b=shader->reflection->constantBlocks[access.index];
        j["status"]="reported_reflection_index";j["name"]=str(b.name);j["register"]=b.fixedBindNumber;j["space"]=b.fixedBindSetOrSpace;j["byte_size"]=b.byteSize;j["variables"]=constants(b.variables);return j;
    }
    Json scan(){
        check(options.first && options.first<=options.last && options.max_events && options.max_events<=256,"invalid production scan range/event bounds");
        check(scan_limits.bytes && scan_limits.bytes<=8*1024*1024 && scan_limits.sources && scan_limits.sources<=128 && scan_limits.candidates && scan_limits.candidates<=64,"invalid production scan limits");
        Json result={{"candidate_only",true},{"identity_proven",false},{"contribution_verified",false},{"first_eid",options.first},{"last_eid",options.last},{"max_events",options.max_events},{"source_cap_per_event",scan_limits.sources},{"total_read_byte_cap",scan_limits.bytes},{"candidate_cap",scan_limits.candidates},{"weak_fallback_enabled",options.weak},{"native_register_mapping_assumed",false},
            {"word_encoding","little_endian_u32"},{"pattern_source","docs/validation/object-constant-passive-getter-settled-live.json; exact D6149/R6148 HAND469 CPU object blocks"},{"pattern",{{"word0",27},{"word2_allowed",Json::array({0,1})},{"word8",469},{"offset_domain","relative to actual bound CBV/root-constant byte range"},{"draw_entry_ids_1483_1484_reused",false}}},{"candidates",Json::array()},{"refusal_counts",Json::object()}};
        uint64_t bytes{},visited{},sources{},nonmatches{},unavailable{},skipped{},source_in_event{};std::string incomplete;const D3D12Pipe::State* active_state{};
        auto refuse=[&](const char* reason){++unavailable;auto& value=result["refusal_counts"][reason];value=value.is_null()?1:value.get<uint64_t>()+1;};
        auto match=[&](const byte* data,size_t n,Json source,uint32_t eid){
            auto u32=[&](size_t offset){return uint32_t(data[offset])|(uint32_t(data[offset+1])<<8)|(uint32_t(data[offset+2])<<16)|(uint32_t(data[offset+3])<<24);};
            const bool strong=n>=36 && u32(0)==27 && u32(8)<=1 && u32(32)==469;Json raw=Json::array();if(options.weak)for(size_t off=0;off+4<=n;off+=4)if(u32(off)==469)raw.push_back(off);
            if(!strong && raw.empty()){++nonmatches;return;}if(result["candidates"].size()>=scan_limits.candidates){incomplete="candidate_limit";return;}
            if(source["source_kind"]=="reported_constant_buffer_descriptor"){DescriptorAccess a;a.stage=ShaderStage(source["stage_enum"].get<uint32_t>());a.index=source["reflection_index"].get<uint32_t>();source["reflection"]=block(*active_state,a);}
            source["pipeline"]=id(active_state->pipelineResourceId);source["root_signature"]=id(active_state->rootSignature.resourceId);source["vs"]=id(active_state->vertexShader.resourceId);source["ps"]=id(active_state->pixelShader.resourceId);
            const auto index=result["candidates"].size();source["event"]=eid;source["match_strength"]=strong?"supported_native_object_pattern_candidate":"weak_raw_469_candidate";source["identity_proven"]=false;source["contribution_verified"]=false;source["sampled_bytes"]=n;source["raw_469_byte_offsets"]=raw;
            if(strong)source["matched_words"]={{"word0",u32(0)},{"word1_raw_not_filter",u32(4)},{"word2",u32(8)},{"word8",u32(32)}};
            const auto name="scan_candidate_"+std::to_string(index)+".bin";out.bytes(name,data,n);source["bytes_file"]=name;result["candidates"].push_back(std::move(source));
        };
        auto reserve_source=[&](){if(result["candidates"].size()>=scan_limits.candidates){incomplete="candidate_limit";return false;}if(source_in_event>=scan_limits.sources){incomplete="source_limit";return false;}++source_in_event;++sources;return true;};
        auto read=[&](const Descriptor& d,Json source,uint32_t eid){
            source["descriptor_type_enum"]=uint32_t(d.type);source["resource"]=id(d.resource);source["view"]=id(d.view);source["byte_offset"]=d.byteOffset;source["view_byte_size"]=d.byteSize;
            if(d.resource==ResourceId::Null()){refuse("unbound");return;}const auto it=buffers.find(d.resource);if(it==buffers.end()){refuse("length_unavailable");return;}
            if(d.byteOffset>it->second || d.byteSize>it->second-d.byteOffset){refuse("invalid_literal_range");return;}
            const auto n=std::min(d.byteSize,options.bytes);if(!n){refuse("empty_range");return;}
            if(n>scan_limits.bytes-bytes){incomplete="byte_limit";return;}if(n>MAX_TOTAL-out.budget.total || out.budget.files>=MAX_FILES){incomplete="output_limit";return;}check(n<=MAX_BUFFER,"scan read prefix cap exceeded");bytes+=n;
            auto data=c.GetBufferData(d.resource,d.byteOffset,n);if(data.size()!=n){refuse("short_read");return;}
            source["resource_length"]=it->second;source["prefix_only"]=n<d.byteSize;match(data.data(),data.size(),std::move(source),eid);
        };
        std::set<uint32_t> unique;
        for(auto eid:scan_events){deadline.check_time();check(eid>=options.first && eid<=options.last && unique.insert(eid).second,"scan schedule contains invalid/duplicate EID");if(visited>=options.max_events){incomplete="event_limit";break;}++visited;source_in_event=0;c.SetFrameEvent(eid,true);const auto* p=c.GetD3D12PipelineState();if(!p){refuse("state_unavailable");continue;}active_state=p;
            const auto& accesses=c.GetDescriptorAccess();if(accesses.size()>128){incomplete="descriptor_metadata_limit";break;}
            for(const auto& a:accesses){deadline.check_time();if(a.type!=DescriptorType::ConstantBuffer){++skipped;continue;}if(!reserve_source())break;
                if(a.descriptorStore==ResourceId::Null() || !a.byteSize || uint64_t(a.byteOffset)+a.byteSize>UINT32_MAX){refuse("descriptor_location_unavailable");continue;}
                rdcarray<DescriptorRange> ranges;ranges.push_back(DescriptorRange(a));const auto values=c.GetDescriptors(a.descriptorStore,ranges);if(values.size()!=1 || values[0].type!=DescriptorType::ConstantBuffer){refuse("descriptor_type_or_count");continue;}
                Json source={{"source_kind","reported_constant_buffer_descriptor"},{"stage_enum",uint32_t(a.stage)},{"reflection_index",a.index},{"array_element",a.arrayElement},{"descriptor_store",id(a.descriptorStore)},{"descriptor_byte_offset",a.byteOffset},{"descriptor_bytes",a.byteSize},{"statically_unused",a.staticallyUnused}};read(values[0],std::move(source),eid);if(!incomplete.empty())break;
            }
            if(!incomplete.empty())break;
            if(p->rootSignature.parameters.size()>64){incomplete="root_metadata_limit";break;}size_t root_index{};
            for(const auto& r:p->rootSignature.parameters){deadline.check_time();const auto index=root_index++;if(r.constants.empty() && r.descriptor.type!=DescriptorType::ConstantBuffer){++skipped;continue;}if(!reserve_source())break;
                Json source={{"source_kind",r.constants.empty()?"root_constant_buffer_descriptor":"root_inline_constants"},{"root_signature",id(p->rootSignature.resourceId)},{"root_parameter",index},{"visibility_mask",uint32_t(r.visibility)},{"register",r.reg},{"space",r.space}};
                if(!r.constants.empty()){if(r.constants.size()>256){refuse("root_constants_size");continue;}const auto n=std::min(uint64_t(r.constants.size()),options.bytes);if(n>scan_limits.bytes-bytes){incomplete="byte_limit";break;}if(n>MAX_TOTAL-out.budget.total || out.budget.files>=MAX_FILES){incomplete="output_limit";break;}bytes+=n;source["prefix_only"]=n<r.constants.size();match(r.constants.data(),size_t(n),std::move(source),eid);}else read(r.descriptor,std::move(source),eid);if(!incomplete.empty())break;
            }
            if(!incomplete.empty())break;
        }
        result["coverage_complete"]=incomplete.empty() && unavailable==0;result["coverage_domain"]="selected constant-source prefixes only; not whole buffers or GPU contribution";result["completion"]=incomplete.empty()?(unavailable?"requested_draw_range_visited_sources_unavailable":"requested_draw_range_visited"):incomplete;result["scheduled_draws"]=scan_events.size();result["visited_draws"]=visited;result["unvisited_draws"]=scan_events.size()-visited;result["non_draw_actions_in_range"]=scan_non_draws;result["actions_outside_range"]=scan_outside;result["source_views_considered"]=sources;result["non_constant_sources_skipped"]=skipped;result["source_reads_refused"]=unavailable;result["sampled_nonmatches"]=nonmatches;result["read_bytes"]=bytes;result["absence_of_match_is_not_identity_proof"]=true;return result;
    }
    Json inventory(){Json j;j["actions"]=actions(c.GetRootActions());const auto& resources=c.GetResources();check(resources.size()<=100000,"resource metadata cap exceeded");j["resources"]=Json::array();for(const auto& r:resources)j["resources"].push_back({{"id",id(r.resourceId)},{"type_enum",uint32_t(r.type)},{"name",str(r.name)},{"auto_name",r.autogeneratedName}});
        const auto& textures=c.GetTextures();check(textures.size()<=100000,"texture metadata cap exceeded");j["textures"]=Json::array();for(const auto& t:textures)j["textures"].push_back({{"id",id(t.resourceId)},{"width",t.width},{"height",t.height},{"depth",t.depth},{"array_size",t.arraysize},{"mips",t.mips},{"samples",t.msSamp},{"format",format(t.format)}});
        j["buffers"]=Json::array();for(const auto& b:buffers)j["buffers"].push_back({{"id",id(b.first)},{"length",b.second}});return j;}
};
}
#ifndef REPLAY_EXTRACT_CPU_TEST
int main(int argc,char** argv){
    try{const auto options=parse(argc,argv);Output output{options.output};try{Session session{options.capture};Extract<IReplayController> extract{*session.replay,output,options};extract.load_buffers();
        Json result={{"model","renderdoc_d3d12_extract_v1"},{"capture",options.capture.string()},{"command",options.command},{"status","complete"},{"native_register_mapping_assumed",false}};
        if(options.command=="inventory")result["inventory"]=extract.inventory();else{extract.actions(session.replay->GetRootActions());if(options.command=="scan")result["scan"]=extract.scan();else result["event_state"]=extract.event();}
        output.json("result.json",result);return 0;
    }catch(const std::exception& e){output.json("error.json",{{"status","failed"},{"error",e.what()}});throw;}}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

#endif
