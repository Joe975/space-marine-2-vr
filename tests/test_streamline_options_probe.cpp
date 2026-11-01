#include "StreamlineOptionsProbe.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <limits>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::atomic<bool> entered{},release_call{};
void blocked(){entered=true;while(!release_call)std::this_thread::yield();}
void overlapping(){if(!entered.exchange(true))while(!release_call)std::this_thread::yield();}
void reset_inside(){StreamlineOptionsProbe::get().reset();}
}
int main(int argc,char** argv){try{
    require(argc==2,"fixture required");auto& p=StreamlineOptionsProbe::get();const auto now=std::chrono::steady_clock::now();p.poll(now);require(!p.sample(0),"unarmed probe never assumes native settings");
    p.enable();p.poll(now);const auto dll=LoadLibraryA(argv[1]);require(dll,"fixture loads");
    const auto ready=reinterpret_cast<void(*)(bool)>(GetProcAddress(dll,"fixture_options_ready"));
    const auto result=reinterpret_cast<void(*)(sl::Result)>(GetProcAddress(dll,"fixture_options_result"));
    const auto calls=reinterpret_cast<unsigned(*)()>(GetProcAddress(dll,"fixture_options_calls"));
    const auto args=reinterpret_cast<bool(*)(const void*,const void*)>(GetProcAddress(dll,"fixture_options_args"));
    const auto inside=reinterpret_cast<void(*)(void(*)())>(GetProcAddress(dll,"fixture_options_inside"));
    const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(dll,"slGetFeatureFunction"));
    require(ready && result && calls && args && inside && resolve,"real API fixture exports");p.poll(now+std::chrono::seconds(2));require(!p.sample(0),"unready DLSS feature is not enabled by the observer");ready(true);p.poll(now+std::chrono::seconds(4));
    void* target{};require(resolve(sl::kFeatureDLSS,"slDLSSSetOptions",target)==sl::Result::eOk && target,"native feature resolved by exact name");const auto call=reinterpret_cast<PFun_slDLSSSetOptions*>(target);
    sl::ViewportHandle view{0u};sl::DLSSOptions values;values.mode=sl::DLSSMode::eMaxPerformance;values.outputWidth=3840;values.outputHeight=2160;
    const auto original=values;require(call(view,values)==sl::Result::eOk && calls()==1 && args(&view,&values) && !std::memcmp(&values,&original,sizeof(values)),"actual option arguments and original result forwarded once unchanged");
    auto first=p.sample(0);require(first && first->viewport==0 && first->values.outputWidth==3840 && first->values.mode==sl::DLSSMode::eMaxPerformance && p.current(*first),"only actual successful supported options become current");values.outputWidth=1920;require(first->values.outputWidth==3840,"options receipt owns native POD values");
    call(view,values);require(!p.current(*first) && p.sample(0)->serial>first->serial,"even identical context handles need the current successful options serial");
    result(sl::Result::eErrorMissingInputParameter);require(call(view,values)==sl::Result::eErrorMissingInputParameter && !p.sample(0),"failed native SetOptions invalidates stale settings without changing failure");result(sl::Result::eOk);
    for(auto version:{4u,0u}){values.structVersion=version;call(view,values);require(!p.sample(0),"unknown layouts stay unknown");}values.structVersion=3;
    values.next=&values;call(view,values);require(!p.sample(0),"unknown options extension cannot become a safe owned copy");values.next=nullptr;
    values.preExposure=std::numeric_limits<float>::quiet_NaN();call(view,values);require(!p.sample(0),"nonfinite exposure rejected");values.preExposure=1;
    view.structVersion=2;call(view,values);require(!p.sample(0),"unknown viewport layouts invalidate observations");view.structVersion=1;
    SYSTEM_INFO info{};GetSystemInfo(&info);auto* pages=static_cast<uint8_t*>(VirtualAlloc(nullptr,2*info.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));DWORD protection{};require(pages && VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&protection),"guard pages");
    for(unsigned version=1;version<=2;++version){values.structVersion=version;const auto start=reinterpret_cast<const uint8_t*>(&values);const auto size=version==1?reinterpret_cast<const uint8_t*>(&values.ultraQualityPreset)-start:reinterpret_cast<const uint8_t*>(&values.alphaUpscalingEnabled)-start;
        auto* short_values=reinterpret_cast<sl::DLSSOptions*>(pages+info.dwPageSize-size);std::memcpy(short_values,&values,size);call(view,*short_values);const auto observed=p.sample(0);require(observed && observed->values.structVersion==version,"older options read only their real historical prefix");
        std::ostringstream json;StreamlineOptionsProbe::write(json,observed);require(json.str().find("\"alpha_upscaling\":null")!=std::string::npos,"absent tail is unknown, never an observed default");
        short_values->structVersion=3;call(view,*short_values);require(!p.sample(0),"truncated claimed V3 options refused");
    }VirtualFree(pages,0,MEM_RELEASE);values.structVersion=3;
    call(view,values);first=p.sample(0);p.reset();require(!p.sample(0) && !p.current(*first),"device reset invalidates owned context settings epoch");call(view,values);require(p.sample(0)->epoch>first->epoch,"fresh actual submission after reset has new epoch");
    inside(reset_inside);call(view,values);inside(nullptr);require(!p.sample(0),"reset inside native API cannot publish settings from before the reset");
    inside(blocked);entered=false;release_call=false;std::thread worker{[&]{call(view,values);}};while(!entered)std::this_thread::yield();const bool during=!p.sample(0);release_call=true;worker.join();inside(nullptr);require(during && p.sample(0),"in-flight options are unknown until the actual API successfully completes");
    inside(overlapping);entered=false;release_call=false;std::thread overlap{[&]{call(view,values);}};while(!entered)std::this_thread::yield();call(view,values);release_call=true;overlap.join();inside(nullptr);require(!p.sample(0),"overlapping non-thread-safe native option submissions cannot publish a guessed winner");
    p.reset();for(uint32_t i=0;i<5;++i){sl::ViewportHandle other{i};call(other,values);}require(!p.sample(0) && !p.sample(4),"bounded viewport overflow poisons observations instead of silently evicting state");p.reset();call(view,values);require(p.sample(0).has_value(),"explicit reset recovers the bounded observer");
    std::ostringstream status;p.write_status(status);require(status.str().find("\"installed\":true")!=std::string::npos && status.str().find("\"rejected\":1")!=std::string::npos && status.str().find("\"known_viewports\":1")!=std::string::npos,"observer status distinguishes installed/called/rejected state from an absent receipt");
    std::cout<<"Actual native DLSS options observation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
