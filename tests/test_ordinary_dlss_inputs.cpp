#include "OrdinaryDlssInputs.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}}
int main(int argc,char** argv){try{
    require(argc==2,"fixture DLL required");const auto dll=LoadLibraryA(argv[1]);require(dll,"fixture loads");
    const auto evaluate=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(dll,"slEvaluateFeature"));const auto calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_history_calls"));
    const auto args=reinterpret_cast<bool(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(GetProcAddress(dll,"fixture_history_args"));require(evaluate && calls && args,"real SDK signature fixture");
    struct Frame:sl::FrameToken{operator uint32_t()const override{return 42;}} frame;
    sl::ViewportHandle view{0u};std::array<sl::Resource,4> resources;std::array<sl::ResourceTag,4> tags;const std::array<uint32_t,4> types{0,1,3,4};
    for(unsigned i=0;i<4;++i){resources[i]=sl::Resource{sl::ResourceType::eTex2d,reinterpret_cast<void*>(uintptr_t(0x1000+i)),128};resources[i].width=1920;resources[i].height=1080;tags[i]=sl::ResourceTag{&resources[i],types[i],sl::ResourceLifecycle::eValidUntilEvaluate};tags[i].extent={0,0,1920,1080};}
    view.next=&tags[0];for(unsigned i=0;i<3;++i)tags[i].next=&tags[i+1];const sl::BaseStructure* inputs[]{&view};
    std::array<uint8_t,sizeof(tags)> original;std::memcpy(original.data(),tags.data(),sizeof(tags));OrdinaryDlssInputs owned;
    require(owned.build(inputs,1,0,0x534d3201u) && owned.count()==1 && owned.render_width()==1920 && owned.render_height()==1080,"complete actual local input chain supplies observed render extents for a private viewport");
    auto* node=owned.data()[0];require(uint32_t(*static_cast<const sl::ViewportHandle*>(node))==0x534d3201u && node!=&view,"only the owned viewport is remapped");node=node->next;
    for(unsigned i=0;i<4;++i){const auto* tag=static_cast<const sl::ResourceTag*>(node);require(tag && tag!=&tags[i] && tag->type==types[i] && tag->resource!=&resources[i] && tag->resource->native==resources[i].native && tag->extent==tags[i].extent && tag->lifecycle==tags[i].lifecycle,"owned tag/resource metadata preserves the exact live native image and extent");node=node->next;}
    require(!node && uint32_t(view)==0 && !std::memcmp(original.data(),tags.data(),sizeof(tags)),"all original SDK inputs remain unchanged");
    const auto before=calls();auto* command=reinterpret_cast<sl::CommandBuffer*>(0x2222);require(evaluate(sl::kFeatureDLSS,frame,owned.data(),owned.count(),command)==sl::Result::eOk && calls()==before+1 && args(sl::kFeatureDLSS,reinterpret_cast<uintptr_t>(&frame),reinterpret_cast<uintptr_t>(owned.data()),1,reinterpret_cast<uintptr_t>(command)),"the real SDK API receives the owned chain and exact live frame/command once");
    tags[3].next=&view;require(!owned.build(inputs,1,0,1) && !owned.data() && !owned.count(),"cycles refuse and cannot expose a previous successful bundle");tags[3].next=nullptr;
    tags[3].type=3;require(!owned.build(inputs,1,0,1),"duplicate/missing required image tags refuse");tags[3].type=4;
    tags[0].lifecycle=sl::ResourceLifecycle::eOnlyValidNow;require(!owned.build(inputs,1,0,1),"a volatile pointer cannot be extended into a future evaluation");tags[0].lifecycle=sl::ResourceLifecycle::eValidUntilEvaluate;
    resources[0].next=&resources[0];require(!owned.build(inputs,1,0,1),"unknown resource extension refuses");resources[0].next=nullptr;
    sl::BaseStructure unknown{{0x1234,0,0,{}},1};unknown.next=&view;const sl::BaseStructure* extended[]{&unknown};require(!owned.build(extended,1,0,1),"unknown input extensions refuse before any native context change");
    sl::ViewportHandle extra{0u};tags[3].next=&extra;require(!owned.build(inputs,1,0,1),"ambiguous multiple viewport nodes refuse");tags[3].next=nullptr;
    require(!owned.build(nullptr,1,0,1) && !owned.build(inputs,17,0,1) && !owned.build(inputs,1,0,0) && !owned.build(inputs,1,2,1),"missing/overflow/unchanged/unexpected viewport inputs refuse");
    SYSTEM_INFO info{};GetSystemInfo(&info);auto* pages=static_cast<uint8_t*>(VirtualAlloc(nullptr,2*info.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));DWORD prior{};require(pages && VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&prior),"guard pages");
    sl::BaseStructure header{sl::ResourceTag::s_structType,1};auto* short_tag=pages+info.dwPageSize-sizeof(header);std::memcpy(short_tag,&header,sizeof(header));view.next=reinterpret_cast<sl::BaseStructure*>(short_tag);require(!owned.build(inputs,1,0,1),"truncated typed input is refused without dereferencing inaccessible memory");view.next=&tags[0];VirtualFree(pages,0,MEM_RELEASE);
    require(owned.build(inputs,1,0,1),"a fresh fully supported live call can construct a new owned chain");
    tags[2].extent={0,0,960,540};require(owned.build(inputs,1,0,1) && owned.render_width()==960 && owned.render_height()==540,"actual partial colour input extent determines raster jitter scale");tags[2].extent={0,1920,960,540};require(!owned.build(inputs,1,0,1),"out-of-resource colour extent refuses");tags[2].extent={};resources[2].width=0;require(!owned.build(inputs,1,0,1),"missing actual render dimensions cannot be replaced with guessed output size");
    std::cout<<"Ordinary DLSS call-local input cloning passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
