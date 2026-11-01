// CPU only: SDK containers use these local malloc/free stubs. No RenderDoc DLL is linked.
#define REPLAY_EXTRACT_CPU_TEST
#define RENDERDOC_EXPORTS
#include "../scripts/native_replay_extract.cpp"
#include <cstdlib>
extern "C" void RENDERDOC_CC RENDERDOC_FreeArrayMem(void* p){std::free(p);}
extern "C" void* RENDERDOC_CC RENDERDOC_AllocArrayMem(uint64_t n){return std::malloc(size_t(n));}
void RENDERDOC_OutOfMemory(uint64_t){throw std::bad_alloc();}
ResourceId resource(uint64_t n){ResourceId r;std::memcpy(&r,&n,8);return r;}
template<class F> void refuses(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"expected production refusal");}
struct Fake {
    D3D12Pipe::State state{};SDFile structured;rdcarray<ActionDescription> actions;rdcarray<ResourceDescription> resources;rdcarray<TextureDescription> textures;rdcarray<BufferDescription> buffers;rdcarray<DescriptorAccess> accesses;rdcarray<Descriptor> values;
    size_t reads{};ResourceId last_resource;uint64_t last_offset{},last_size{};uint32_t event{};bool short_read{};std::vector<byte> payload;size_t descriptor_reads{},texture_reads{};ResourceId last_texture;Subresource last_sub;std::vector<std::string> calls;
    bytebuf GetBufferData(ResourceId r,uint64_t offset,uint64_t n){++reads;last_resource=r;last_offset=offset;last_size=n;bytebuf b;b.resize(size_t(n-(short_read?1:0)));for(size_t i=0;i<b.size();++i)b[i]=payload.empty()?0x5a:payload.at(size_t(offset+i));return b;}
    const auto& GetRootActions(){return actions;}
    const auto& GetStructuredFile(){return structured;}
    const auto& GetResources(){return resources;}
    const auto& GetTextures(){return textures;}
    bytebuf GetTextureData(ResourceId r,const Subresource& sub){++texture_reads;calls.push_back("texture");last_texture=r;last_sub=sub;bytebuf b;b.resize(8);for(size_t i=0;i<8;++i)b[i]=byte(i+1);return b;}
    const auto& GetBuffers(){return buffers;}
    const auto& GetDescriptorAccess(){return accesses;}
    auto GetDescriptors(ResourceId store,const rdcarray<DescriptorRange>& ranges){++descriptor_reads;check(store==resource(44) && ranges.size()==1 && ranges[0].offset==24,"actual descriptor locator preserved");return values;}
    void SetFrameEvent(uint32_t eid,bool force){check(force,"exact event force");event=eid;calls.push_back("seek");}
    const D3D12Pipe::State* GetD3D12PipelineState(){calls.push_back("state");return &state;}
    rdcstr DisassembleShader(ResourceId,const ShaderReflection*,const char*){return {};}
    MeshFormat GetPostVSData(uint32_t,uint32_t,MeshDataStage){calls.push_back("postvs");return {};}
};
#ifndef REPLAY_EXTRACT_PATHS_NO_MAIN
int main(){const auto parent=std::filesystem::temp_directory_path();const auto dir=parent/("sm2vr-replay-cpu-"+std::to_string(GetCurrentProcessId()));
    try{check(!std::filesystem::exists(dir),"fresh CPU test directory");
        {Output output{dir};Options options;options.event=7;options.bytes=16;Fake fake;Extract<Fake> e{fake,output,options};e.buffers[resource(1)]=64;
        ActionDescription action;action.eventId=7;action.customName="named draw";action.numIndices=12;ActionDescription child;child.eventId=8;child.customName="child";action.children.push_back(child);fake.actions.push_back(action);
        TextureDescription texture{};texture.resourceId=resource(90);texture.width=1280;texture.height=1440;texture.depth=1;texture.arraysize=2;texture.mips=1;texture.msSamp=4;fake.textures.push_back(texture);
        auto inventory=e.inventory();check(inventory["actions"].size()==2 && inventory["actions"][0]["event"]==7 && inventory["actions"][1]["parent_event"]==7 && inventory["textures"][0]["width"]==1280 && inventory["textures"][0]["samples"]==4 && inventory["textures"][0]["array_size"]==2,"actual inventory action hierarchy and texture metadata");
        auto b=e.buffer(resource(1),8,32,"buffer");check(b["exported_bytes"]==16 && b["prefix_only"]==true && fake.last_offset==8 && fake.last_size==16,"actual bounded prefix read");
        const auto reads=fake.reads;check(e.buffer(resource(2),0,16,"unknown")["bytes_status"]=="not_in_buffer_inventory_length_unavailable" && fake.reads==reads,"unknown source no read");
        check(e.buffer(resource(1),60,8,"invalid")["bytes_status"]=="unsupported_or_invalid_view_size" && fake.reads==reads,"invalid source span no read");
        check(e.buffer(resource(1),64,0,"empty")["bytes_status"]=="empty_view" && fake.reads==reads,"zero length never means whole read");
        check(e.buffer(resource(1),60,UINT32_MAX,"oversized")["bytes_status"]=="unsupported_or_invalid_view_size","direct literal oversized span refused");
        fake.short_read=true;refuses([&]{e.buffer(resource(1),0,4,"short");});fake.short_read=false;
        DescriptorAccess a;a.descriptorStore=resource(44);a.byteOffset=24;a.byteSize=32;fake.accesses.push_back(a);Descriptor d;d.resource=resource(1);d.byteOffset=12;d.byteSize=8;fake.values.push_back(d);
        auto rows=e.descriptors();check(rows[0]["binding"]["resource"]=="0x1" && fake.last_resource==resource(1) && fake.last_offset==12 && fake.last_size==8,"actual descriptor resource and range");
        fake.values[0].resource=resource(2);check(e.descriptors()[0]["binding"]["bytes_status"]=="not_in_buffer_inventory_length_unavailable","wrong descriptor source unavailable");fake.accesses.clear();
        fake.state.rasterizer.state.frontCCW=true;fake.state.rasterizer.viewports.push_back(Viewport(1,2,800,600,0.1f,0.9f));fake.state.rasterizer.scissors.push_back(Scissor(3,4,500,400));
        auto& ds=fake.state.outputMerger.depthStencilState;ds.depthEnable=true;ds.depthWrites=false;ds.stencilEnable=true;ds.frontFace.reference=7;ds.frontFace.writeMask=0x31;
        fake.state.outputMerger.depthTarget.resource=resource(99);fake.state.outputMerger.depthTarget.firstMip=2;fake.state.outputMerger.renderTargets.push_back(d);
        fake.state.inputAssembly.indexBuffer.resourceId=resource(1);fake.state.inputAssembly.indexBuffer.byteSize=UINT32_MAX;D3D12Pipe::VertexBuffer vb;vb.resourceId=resource(1);vb.byteSize=UINT32_MAX;fake.state.inputAssembly.vertexBuffers.push_back(vb);
        e.found=true;auto event=e.event();check(event["ia"]["index_buffer"]["bytes_status"]=="unsupported_or_invalid_view_size" && event["ia"]["vertex_buffers"][0]["bytes_status"]=="unsupported_or_invalid_view_size","direct D3D12 IA literal sizes do not inherit common BoundVBuffer sentinel");check(fake.event==7 && event["event"]==7 && event["output_state"]["depth_stencil"]["depth_test"]==true && event["output_state"]["depth_stencil"]["front"]["reference"]==7 && event["output_state"]["depth_target"]["resource"]=="0x63" && event["output_state"]["depth_target"]["first_mip"]==2 && event["output_state"]["raster"]["viewports"][0]["width"]==800 && event["output_state"]["raster"]["scissors"][0]["x"]==3,"actual event output/depth/raster serialization");
        check(event["output_preview"]["status"]=="not_requested" && fake.texture_reads==0,"default event does not read output texture");
        auto& tex=fake.textures[0];tex.dimension=2;tex.type=TextureType::Texture2DArray;tex.width=4;tex.height=2;tex.depth=1;tex.arraysize=3;tex.mips=2;tex.msSamp=1;tex.format.type=ResourceFormatType::Regular;tex.format.compCount=4;tex.format.compByteWidth=1;tex.format.compType=CompType::UNorm;
        auto& target=fake.state.outputMerger.renderTargets[0];target.resource=resource(90);target.format=tex.format;target.firstMip=1;target.numMips=1;target.firstSlice=2;target.numSlices=1;e.options.preview=true;e.options.preview_slot=7;
        check(e.event()["output_preview"]["reason"]=="invalid_colour_slot" && fake.texture_reads==0,"actual event invalid preview slot never reads texture");
        e.options.preview_slot=0;tex.format.compType=CompType::Float;check(e.event()["output_preview"]["reason"]=="unsupported_format_requires_regular_rgba8_unorm" && fake.texture_reads==0,"actual event unsupported preview format never reads texture");tex.format.compType=CompType::UNorm;
        e.options.postvs=true;fake.calls.clear();const auto preview=e.event()["output_preview"];check(preview["status"]=="exported" && fake.texture_reads==1 && fake.last_texture==resource(90) && fake.last_sub==Subresource(1,2,0),"actual event preview exact resource/subresource");check(fake.calls==std::vector<std::string>({"seek","state","texture","postvs"}),"actual event preview follows exact seek before postVS");
        e.found=false;refuses([&]{e.event();});check(fake.event==7,"missing event never seeks");
        output.budget.files=MAX_FILES;const auto previous=fake.reads;refuses([&]{e.buffer(resource(1),0,4,"capped");});check(fake.reads==previous,"file cap before API read");output.budget.files=0;output.budget.total=MAX_TOTAL;refuses([&]{e.buffer(resource(1),0,4,"bytes_capped");});check(fake.reads==previous,"byte cap before API read");}
        const auto resolved=std::filesystem::weakly_canonical(dir);check(resolved.parent_path()==std::filesystem::weakly_canonical(parent) && resolved.filename().string().rfind("sm2vr-replay-cpu-",0)==0,"CPU cleanup target bounded");std::filesystem::remove_all(resolved);
        std::cout<<"replay extractor production CPU paths PASS\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

#endif
