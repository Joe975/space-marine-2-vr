#pragma once
#include "ReplayExtractSupport.h"
#include "renderdoc_replay.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace replay_extract {
inline std::string preview_resource_id(ResourceId resource){uint64_t bits{};static_assert(sizeof(resource)==sizeof(bits),"ResourceId representation changed");std::memcpy(&bits,&resource,sizeof(bits));std::ostringstream out;out<<"0x"<<std::hex<<bits;return out.str();}
inline Json preview_format(const ResourceFormat& f){return format(uint32_t(f.type),f.compCount,f.compByteWidth,uint32_t(f.compType),f.BGRAOrder(),f.SRGBCorrected());}
inline bool preview_rgba8(const ResourceFormat& f){return f.type==ResourceFormatType::Regular && f.compCount==4 && f.compByteWidth==1 && (f.compType==CompType::UNorm || f.compType==CompType::UNormSRGB);}

// The caller has already sought this exact action. This helper never changes the
// event, guesses an eye, interprets a native pointer, or invokes pixel history.
template<class Controller> Json event_output_preview(Controller& controller,Output& output,Deadline& deadline,uint32_t event,const D3D12Pipe::State& state,uint32_t slot){
    Json result={{"status","unavailable"},{"event",event},{"colour_slot",slot},{"api","GetTextureData"},{"pixel_history",false},{"owned_eye_verified",false},{"draw_contribution_verified",false}};
    const auto unavailable=[&](const char* reason){result["reason"]=reason;return result;};
    if(!event)return unavailable("invalid_event");
    const auto& targets=state.outputMerger.renderTargets;if(targets.size()>8 || slot>=targets.size())return unavailable("invalid_colour_slot");
    const auto descriptor=targets[slot];result["resource"]=preview_resource_id(descriptor.resource);result["view"]=preview_resource_id(descriptor.view);result["view_format"]=preview_format(descriptor.format);
    result["view_first_mip"]=descriptor.firstMip;result["view_mip_count"]=descriptor.numMips;result["view_first_slice"]=descriptor.firstSlice;result["view_slice_count"]=descriptor.numSlices;
    if(descriptor.resource==ResourceId::Null())return unavailable("unbound_colour_target");
    deadline.check_time();const auto& textures=controller.GetTextures();if(textures.size()>100000)return unavailable("texture_inventory_limit");
    const TextureDescription* found{};for(const auto& t:textures)if(t.resourceId==descriptor.resource){if(found)return unavailable("ambiguous_texture_inventory");found=&t;}
    if(!found)return unavailable("texture_not_in_inventory");const auto texture=*found;
    result["texture_format"]=preview_format(texture.format);result["texture_type_enum"]=uint32_t(texture.type);result["texture_dimension"]=texture.dimension;result["texture_width"]=texture.width;result["texture_height"]=texture.height;result["texture_depth"]=texture.depth;result["texture_mips"]=texture.mips;result["texture_array_size"]=texture.arraysize;result["texture_samples"]=texture.msSamp;
    if(texture.dimension!=2 || texture.depth!=1 || texture.cubemap || (texture.type!=TextureType::Texture2D && texture.type!=TextureType::Texture2DArray))return unavailable("unsupported_texture_dimension_or_type");
    if(texture.msSamp!=1)return unavailable("unsupported_multisample");
    if(!preview_rgba8(texture.format) || !preview_rgba8(descriptor.format))return unavailable("unsupported_format_requires_regular_rgba8_unorm");
    if(texture.format.BGRAOrder()!=descriptor.format.BGRAOrder())return unavailable("view_texture_channel_order_mismatch");
    if(!texture.width || !texture.height || !texture.mips || texture.mips>32 || !texture.arraysize || !descriptor.numMips || !descriptor.numSlices || descriptor.firstMip>=texture.mips || descriptor.numMips>texture.mips-descriptor.firstMip || descriptor.firstSlice>=texture.arraysize || descriptor.numSlices>texture.arraysize-descriptor.firstSlice)return unavailable("invalid_subresource_range");
    const auto width=std::max(1U,texture.width>>descriptor.firstMip),height=std::max(1U,texture.height>>descriptor.firstMip);const uint64_t pixels=uint64_t(width)*height;
    if(pixels>4ULL*1024*1024)return unavailable("preview_pixel_limit");const auto raw_size=pixels*4;
    const auto header="P6\n"+std::to_string(width)+" "+std::to_string(height)+"\n255\n";const auto ppm_size=uint64_t(header.size())+pixels*3;
    const auto stem="preview_colour_"+std::to_string(slot);const auto raw_name=stem+".raw.bin",ppm_name=stem+".ppm";
    if(output.budget.files>MAX_FILES-2 || output.budget.total>MAX_TOTAL || raw_size+ppm_size>MAX_TOTAL-output.budget.total)return unavailable("preview_output_budget");
    if(fs::exists(output.path/raw_name) || fs::exists(output.path/ppm_name))return unavailable("preview_output_exists");
    result["width"]=width;result["height"]=height;result["mip"]=descriptor.firstMip;result["slice"]=descriptor.firstSlice;result["sample"]=0;result["selected_first_view_subresource_only"]=true;result["expected_raw_bytes"]=raw_size;
    deadline.check_time();const auto data=controller.GetTextureData(descriptor.resource,Subresource(descriptor.firstMip,descriptor.firstSlice,0));deadline.check_time();
    check(data.size()==raw_size,"GetTextureData returned unexpected RGBA8 subresource size");
    std::vector<byte> ppm;ppm.reserve(size_t(ppm_size));ppm.insert(ppm.end(),header.begin(),header.end());const auto red=texture.format.BGRAOrder()?2U:0U,blue=texture.format.BGRAOrder()?0U:2U;
    for(size_t i=0;i<data.size();i+=4){ppm.push_back(data[i+red]);ppm.push_back(data[i+1]);ppm.push_back(data[i+blue]);}
    deadline.check_time();output.bytes(raw_name,data.data(),data.size());output.bytes(ppm_name,ppm.data(),ppm.size());
    result["status"]="exported";result["raw_file"]=raw_name;result["preview_file"]=ppm_name;result["raw_bytes"]=data.size();result["raw_channel_order"]=texture.format.BGRAOrder()?"BGRA":"RGBA";result["preview_alpha_ignored"]=true;result["raw_alpha_retained"]=true;result["colour_transform"]="none; raw encoded RGB components";return result;
}
}
