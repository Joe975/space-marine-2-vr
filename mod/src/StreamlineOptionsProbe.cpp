#include "StreamlineOptionsProbe.h"
#include "StreamlineModule.h"
#include <MinHook.h>
#include <cmath>

namespace {
template<class T>bool read(const T* p,T& value){SIZE_T bytes{};return p && ReadProcessMemory(GetCurrentProcess(),p,&value,sizeof(value),&bytes) && bytes==sizeof(value);}
}
StreamlineOptionsProbe& StreamlineOptionsProbe::get(){static auto* value=new StreamlineOptionsProbe;return *value;}
void StreamlineOptionsProbe::poll(std::chrono::steady_clock::time_point now){
    std::lock_guard lock{m_install};if(!m_enabled || m_installed || now<m_next)return;m_next=now+std::chrono::seconds(1);
    const auto module=streamline_module();if(!module)return;
    const auto resolve=reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(module,"slGetFeatureFunction"));void* target{};
    if(!resolve || resolve(sl::kFeatureDLSS,"slDLSSSetOptions",target)!=sl::Result::eOk || !target)return;
    HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(target),&pinned))return;
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return;void* original{};
    if(MH_CreateHook(target,reinterpret_cast<void*>(&options),&original)!=MH_OK)return;m_original=reinterpret_cast<PFun_slDLSSSetOptions*>(original);
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);m_original=nullptr;return;}m_installed=true;
}
bool StreamlineOptionsProbe::copy(const sl::DLSSOptions& source,sl::DLSSOptions& value){
    sl::BaseStructure header{{},0};if(!read(static_cast<const sl::BaseStructure*>(&source),header) || header.structType!=sl::DLSSOptions::s_structType || header.next || header.structVersion<1 || header.structVersion>3)return false;
    // Official SDK 2.0.1 V1 ends before ultraQualityPreset; SDK 2.1.0 V2
    // adds ultraQualityPreset/useAutoExposure. V3 adds alphaUpscalingEnabled.
    const auto begin=reinterpret_cast<const uint8_t*>(&value);
    const size_t size=header.structVersion==1?reinterpret_cast<const uint8_t*>(&value.ultraQualityPreset)-begin:
        header.structVersion==2?reinterpret_cast<const uint8_t*>(&value.alphaUpscalingEnabled)-begin:sizeof(value);
    SIZE_T bytes{};if(!ReadProcessMemory(GetCurrentProcess(),&source,&value,size,&bytes) || bytes!=size || value.structType!=header.structType || value.structVersion!=header.structVersion || value.next)return false;
    const auto valid=[](float f){return std::isfinite(f) && f!=sl::INVALID_FLOAT;};
    if(uint32_t(value.mode)>=uint32_t(sl::DLSSMode::eCount) || !value.outputWidth || value.outputWidth==sl::INVALID_UINT || !value.outputHeight || value.outputHeight==sl::INVALID_UINT ||
        !valid(value.sharpness) || value.sharpness<0 || value.sharpness>1 || !valid(value.preExposure) || value.preExposure<=0 || !valid(value.exposureScale) || value.exposureScale<=0)return false;
    for(auto b:{value.colorBuffersHDR,value.indicatorInvertAxisX,value.indicatorInvertAxisY,value.useAutoExposure,value.alphaUpscalingEnabled})if(b!=sl::eFalse && b!=sl::eTrue)return false;
    for(auto p:{value.dlaaPreset,value.qualityPreset,value.balancedPreset,value.performancePreset,value.ultraPerformancePreset,value.ultraQualityPreset})if(uint32_t(p)>=uint32_t(sl::DLSSPreset::eCount))return false;
    return true;
}
sl::Result StreamlineOptionsProbe::options(const sl::ViewportHandle& viewport,const sl::DLSSOptions& values){
    auto& self=get();sl::ViewportHandle view;Receipt receipt;bool accepted=false;uint64_t epoch{},serial{};
    try{accepted=read(&viewport,view) && view.structType==sl::ViewportHandle::s_structType && view.structVersion==1 && !view.next && copy(values,receipt.values);
        if(accepted){receipt.viewport=uint32_t(view);receipt.thread=GetCurrentThreadId();}
    }catch(...){accepted=false;}
    {std::lock_guard lock{self.m_mutex};++self.m_calls;if(!accepted)++self.m_invalid;if(self.m_active){++self.m_epoch;self.m_options.clear();}++self.m_active;epoch=self.m_epoch;serial=++self.m_serial;
        if(accepted)self.m_options.erase(receipt.viewport);else self.m_options.clear();}
    const auto result=self.m_original.load()(viewport,values);
    try{std::lock_guard lock{self.m_mutex};--self.m_active;if(result!=sl::Result::eOk)++self.m_rejected;
        const bool stable=epoch==self.m_epoch && serial==self.m_serial && !self.m_active;++self.m_serial;
        if(accepted && stable && result==sl::Result::eOk && !self.m_overflow){
            if(self.m_options.size()==4){self.m_options.clear();self.m_overflow=true;}
            else{receipt.epoch=self.m_epoch;receipt.serial=self.m_serial;self.m_options.emplace(receipt.viewport,receipt);}
        }
    }catch(...){}return result;
}
void StreamlineOptionsProbe::reset(){std::lock_guard lock{m_mutex};++m_epoch;++m_serial;m_options.clear();m_overflow=false;}
std::optional<StreamlineOptionsProbe::Receipt> StreamlineOptionsProbe::sample(uint32_t viewport)const{
    std::lock_guard lock{m_mutex};if(!m_enabled || !m_installed || m_active || m_overflow)return std::nullopt;const auto found=m_options.find(viewport);return found==m_options.end()?std::nullopt:std::optional{found->second};
}
bool StreamlineOptionsProbe::current(const Receipt& r)const{const auto value=sample(r.viewport);return value && value->epoch==r.epoch && value->serial==r.serial;}
void StreamlineOptionsProbe::write_status(std::ostream& out)const{std::lock_guard lock{m_mutex};out<<"{\"installed\":"<<(m_installed?"true":"false")<<",\"calls\":"<<m_calls<<",\"invalid\":"<<m_invalid<<",\"rejected\":"<<m_rejected<<",\"active\":"<<m_active<<",\"overflow\":"<<(m_overflow?"true":"false")<<",\"epoch\":"<<m_epoch<<",\"serial\":"<<m_serial<<",\"known_viewports\":"<<m_options.size()<<'}';}
void StreamlineOptionsProbe::write(std::ostream& out,const std::optional<Receipt>& receipt){
    if(!receipt){out<<"null";return;}const auto& r=*receipt;const auto& v=r.values;
    out<<"{\"epoch\":"<<r.epoch<<",\"serial\":"<<r.serial<<",\"viewport\":"<<r.viewport<<",\"thread\":"<<r.thread<<",\"version\":"<<v.structVersion<<",\"mode\":"<<uint32_t(v.mode)<<",\"width\":"<<v.outputWidth<<",\"height\":"<<v.outputHeight<<",\"sharpness\":"<<v.sharpness<<",\"pre_exposure\":"<<v.preExposure<<",\"exposure_scale\":"<<v.exposureScale<<",\"hdr\":"<<uint32_t(v.colorBuffersHDR)<<",\"invert_x\":"<<uint32_t(v.indicatorInvertAxisX)<<",\"invert_y\":"<<uint32_t(v.indicatorInvertAxisY)<<",\"presets\":[";
    unsigned i{};for(auto p:{v.dlaaPreset,v.qualityPreset,v.balancedPreset,v.performancePreset,v.ultraPerformancePreset}){if(i++)out<<',';out<<uint32_t(p);}out<<"],\"ultra_quality_preset\":";if(v.structVersion>=2)out<<uint32_t(v.ultraQualityPreset);else out<<"null";
    out<<",\"auto_exposure\":";if(v.structVersion>=2)out<<uint32_t(v.useAutoExposure);else out<<"null";out<<",\"alpha_upscaling\":";if(v.structVersion>=3)out<<uint32_t(v.alphaUpscalingEnabled);else out<<"null";out<<'}';
}
