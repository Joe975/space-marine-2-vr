#pragma once
#include "NativeTextureContract.h"
#include "HistoryDescriptorProbe.h"

namespace native_global_history {
struct Snapshot {
    native_texture_contract::Snapshot pool;
    std::array<uintptr_t,2> native{},resource{};
    std::array<bool,2> readable{};
    std::array<HistoryDescriptorProbe::Observation,2> descriptors;
};
inline Snapshot capture(uintptr_t base,uintptr_t current,uintptr_t previous,uint32_t counter){
    Snapshot value;value.native={current,previous};std::array<uintptr_t,9> wrappers{};wrappers[0]=current;wrappers[1]=previous;
    value.pool=native_texture_contract::capture_global_history(base,wrappers,counter);
    for(size_t i=0;i<2;++i){const auto native=value.native[i];
        value.readable[i]=native && native<=UINTPTR_MAX-0x280 && native_texture_contract::read(native+0x278,value.resource[i]);
        value.descriptors[i]=HistoryDescriptorProbe::get().observe(value.readable[i]?value.resource[i]:0);
    }return value;
}
inline void write(std::ostream& out,const Snapshot& value){
    out<<"{\"pool\":";native_texture_contract::write(out,value.pool);out<<",\"inputs\":[";
    for(size_t i=0;i<2;++i){if(i)out<<',';out<<"{\"role\":\""<<(i?"previous":"current")<<"\",\"native\":"<<value.native[i]<<",\"resource\":"<<value.resource[i]<<",\"readable\":"<<(value.readable[i]?"true":"false")<<",\"descriptors\":";HistoryDescriptorProbe::write(out,value.descriptors[i]);out<<'}';}out<<"]}";
}
}
