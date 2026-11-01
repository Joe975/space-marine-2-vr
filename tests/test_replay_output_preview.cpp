// CPU only: no RenderDoc DLL or replay/GPU initialisation.
#define RENDERDOC_EXPORTS
#include <Windows.h>
#include "ReplayOutputPreview.h"
#include <cstdlib>
#include <iostream>
extern "C" void RENDERDOC_CC RENDERDOC_FreeArrayMem(void* p){std::free(p);}
extern "C" void* RENDERDOC_CC RENDERDOC_AllocArrayMem(uint64_t n){return std::malloc(size_t(n));}
void RENDERDOC_OutOfMemory(uint64_t){throw std::bad_alloc();}
using namespace replay_extract;
ResourceId resource(uint64_t n){ResourceId r;std::memcpy(&r,&n,8);return r;}
struct Fake {
    rdcarray<TextureDescription> textures;size_t reads{};ResourceId last_resource;Subresource last_sub;int size_delta{};
    const auto& GetTextures(){return textures;}
    bytebuf GetTextureData(ResourceId r,const Subresource& sub){++reads;last_resource=r;last_sub=sub;bytebuf data;data.resize(size_t(8+size_delta));for(size_t i=0;i<data.size();++i)data[i]=byte(i+1);return data;}
};
std::vector<byte> read_file(const fs::path& p){std::ifstream f{p,std::ios::binary};return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
int main(){const auto parent=fs::temp_directory_path();const auto dir=parent/("sm2vr-preview-cpu-"+std::to_string(GetCurrentProcessId()));
    try{check(!fs::exists(dir),"fresh preview fixture directory");fs::create_directory(dir);size_t test{};
        auto run=[&](auto test_case){Output out{dir/("case"+std::to_string(test++))};Deadline deadline;Fake fake;TextureDescription t{};t.resourceId=resource(90);t.dimension=2;t.type=TextureType::Texture2DArray;t.width=4;t.height=2;t.depth=1;t.arraysize=3;t.mips=2;t.msSamp=1;t.format.type=ResourceFormatType::Regular;t.format.compCount=4;t.format.compByteWidth=1;t.format.compType=CompType::UNorm;fake.textures.push_back(t);D3D12Pipe::State state;Descriptor d;d.resource=resource(90);d.view=resource(91);d.format=t.format;d.firstMip=1;d.numMips=1;d.firstSlice=2;d.numSlices=1;state.outputMerger.renderTargets.push_back(d);return test_case(fake,state,out,deadline);};
        run([](auto& f,auto& s,auto& o,auto& d){const auto j=event_output_preview(f,o,d,7,s,0);check(j["status"]=="exported" && f.reads==1 && f.last_resource==resource(90) && f.last_sub==Subresource(1,2,0),"actual target ResourceId/subresource forwarding");check(j["view"]=="0x5b" && j["width"]==2 && j["height"]==1 && j["preview_alpha_ignored"]==true && j["draw_contribution_verified"]==false,"preview source metadata remains qualified");check(read_file(o.path/j["raw_file"].template get<std::string>())==std::vector<byte>({1,2,3,4,5,6,7,8}),"original raw alpha/channels preserved");auto ppm=read_file(o.path/j["preview_file"].template get<std::string>());check(std::vector<byte>(ppm.end()-6,ppm.end())==std::vector<byte>({1,2,3,5,6,7}),"RGBA preview exact channels alpha ignored");const auto again=event_output_preview(f,o,d,7,s,0);check(again["reason"]=="preview_output_exists" && f.reads==1,"existing preview refused before read");return 0;});
        run([](auto& f,auto& s,auto& o,auto& d){f.textures[0].format.SetBGRAOrder(true);s.outputMerger.renderTargets[0].format=f.textures[0].format;const auto j=event_output_preview(f,o,d,7,s,0);auto ppm=read_file(o.path/j["preview_file"].template get<std::string>());check(j["raw_channel_order"]=="BGRA" && std::vector<byte>(ppm.end()-6,ppm.end())==std::vector<byte>({3,2,1,7,6,5}),"BGRA preview exact red/blue conversion");return 0;});
        auto refuses_before_read=[&](auto change,const char* reason){run([&](auto& f,auto& s,auto& o,auto& d){change(f,s,o);const auto j=event_output_preview(f,o,d,7,s,0);check(j["status"]=="unavailable" && j["reason"]==reason && f.reads==0 && o.budget.files==0,"unsupported source refused before API read/write");return 0;});};
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets.clear();},"invalid_colour_slot");
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets[0].resource=ResourceId::Null();},"unbound_colour_target");
        refuses_before_read([](auto& f,auto&,auto&){f.textures.clear();},"texture_not_in_inventory");
        refuses_before_read([](auto& f,auto&,auto&){const auto t=f.textures[0];f.textures.push_back(t);},"ambiguous_texture_inventory");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].depth=2;},"unsupported_texture_dimension_or_type");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].type=TextureType::Texture3D;},"unsupported_texture_dimension_or_type");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].cubemap=true;},"unsupported_texture_dimension_or_type");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].msSamp=4;},"unsupported_multisample");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].format.compByteWidth=2;f.textures[0].format.compType=CompType::Float;},"unsupported_format_requires_regular_rgba8_unorm");
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets[0].format.compType=CompType::Typeless;},"unsupported_format_requires_regular_rgba8_unorm");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].format.type=ResourceFormatType::BC1;},"unsupported_format_requires_regular_rgba8_unorm");
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets[0].format.SetBGRAOrder(true);},"view_texture_channel_order_mismatch");
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets[0].firstMip=2;},"invalid_subresource_range");
        refuses_before_read([](auto&,auto& s,auto&){s.outputMerger.renderTargets[0].firstSlice=3;},"invalid_subresource_range");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].mips=33;},"invalid_subresource_range");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].width=0;},"invalid_subresource_range");
        refuses_before_read([](auto& f,auto&,auto&){f.textures[0].width=UINT32_MAX;f.textures[0].height=UINT32_MAX;},"preview_pixel_limit");
        run([](auto& f,auto& s,auto& o,auto& d){o.budget.total=MAX_TOTAL-1;check(event_output_preview(f,o,d,7,s,0)["reason"]=="preview_output_budget" && f.reads==0,"total output cap before read");o.budget.total=0;o.budget.files=MAX_FILES-1;check(event_output_preview(f,o,d,7,s,0)["reason"]=="preview_output_budget" && f.reads==0,"both output files budgeted before read");return 0;});
        for(int delta:{-1,1})run([&](auto& f,auto& s,auto& o,auto& d){f.size_delta=delta;bool threw=false;try{event_output_preview(f,o,d,7,s,0);}catch(const std::exception&){threw=true;}check(threw && f.reads==1 && o.budget.files==0,"short/oversized texture read never exported");return 0;});
        const auto resolved=fs::weakly_canonical(dir);check(resolved.parent_path()==fs::weakly_canonical(parent) && resolved.filename().string().rfind("sm2vr-preview-cpu-",0)==0,"preview fixture cleanup bounded");fs::remove_all(resolved);std::cout<<"replay output preview CPU paths PASS\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
