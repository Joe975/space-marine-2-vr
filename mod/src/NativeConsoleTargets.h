#pragma once
#include <array>
#include "GameBuild.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <ostream>

// Native CPU cache snapshots only. Targets can be intermediate/reused; neither
// a resource pointer nor a viewport establishes Scene/eye/frame ownership.
struct NativeConsoleTargets {
    using Reader=std::function<bool(uintptr_t,void*,size_t)>;
    struct State {
        bool fields_read{},fields_stable{},counts_bounded{},resources_read{},resources_stable{};
        std::array<uintptr_t,8> colors{},resources{},rtvs{};
        std::array<bool,8> resource_read{};
        uintptr_t depth{},depth_resource{},dsv{};
        bool depth_read{},depth_present{};
        std::array<int32_t,2> dimensions{};
        uint32_t subresource_words{};
        std::array<uint32_t,6> viewport_words{};
        std::array<int32_t,4> scissor{};
        int32_t prepared_count{},effective_count{};
    };
    struct Pair {bool supported{},identity_before{},identity_after{},same_call{};State before,after;};
    static bool supported(uintptr_t base,size_t image_size,const Reader& read){
        constexpr game_build::Code targets=game_build::hashed(0x2142ba0,16,"de4f31d8eca21ab43fdd1cbe56a9b7773ff586bf82bcd63953a4438e3e932cb3");
        constexpr game_build::Code bind=game_build::hashed(0x21426d0,16,"99cccf7e498fc4f187d5e78dbba6d224ecad6a873afa1fb345fa8b35fd92dffe");
        constexpr game_build::Code indexed=game_build::hashed(0x213d690,16,"736024b87904fa939bde5ab29175957ae70a04c3bda9d391e59387db3fa81c33");
        std::array<uint8_t,16> a{},b{},c{};
        return base && image_size>=0x2142bb0 && base<=UINTPTR_MAX-image_size && read(base+game_build::rva(0x2142ba0),a.data(),a.size()) && game_build::code(0x2142ba0,a,targets) && read(base+game_build::rva(0x21426d0),b.data(),b.size()) && game_build::code(0x21426d0,b,bind) && read(base+game_build::rva(0x213d690),c.data(),c.size()) && game_build::code(0x213d690,c,indexed);
    }
    static State capture(uintptr_t command,const Reader& read){
        State s;std::array<uint8_t,0x54> source{};std::array<uint8_t,0x94> cache{};
        if(!command || command>UINTPTR_MAX-0x119a4 || !read(command+0xfd88,source.data(),source.size()) || !read(command+0x11910,cache.data(),cache.size()))return s;
        std::memcpy(s.colors.data(),source.data(),64);std::memcpy(&s.depth,source.data()+0x40,8);std::memcpy(s.dimensions.data(),source.data()+0x48,8);std::memcpy(&s.subresource_words,source.data()+0x50,4);
        std::memcpy(s.viewport_words.data(),cache.data(),24);std::memcpy(s.scissor.data(),cache.data()+0x18,16);s.depth_present=cache[0x3c]!=0;std::memcpy(&s.dsv,cache.data()+0x40,8);std::memcpy(s.rtvs.data(),cache.data()+0x48,64);
        std::memcpy(&s.prepared_count,cache.data()+0x88,4);std::memcpy(&s.effective_count,cache.data()+0x90,4);s.fields_read=true;
        s.counts_bounded=s.prepared_count>=0 && s.prepared_count<=8 && s.effective_count>=-1 && s.effective_count<=8;
        // Invalid counts never authorize traversal; retain raw cache for diagnosis.
        if(!s.counts_bounded)return s;
        s.resources_read=true;
        for(size_t i=0;i<8;++i){s.resource_read[i]=!s.colors[i] || (s.colors[i]<=UINTPTR_MAX-0x280 && read(s.colors[i]+0x278,&s.resources[i],8));s.resources_read=s.resources_read&&s.resource_read[i];}
        s.depth_read=!s.depth || (s.depth<=UINTPTR_MAX-0x280 && read(s.depth+0x278,&s.depth_resource,8));s.resources_read=s.resources_read&&s.depth_read;
        std::array<uint8_t,0x54> source_again{};std::array<uint8_t,0x94> cache_again{};
        s.fields_stable=read(command+0xfd88,source_again.data(),source_again.size()) && source_again==source && read(command+0x11910,cache_again.data(),cache_again.size()) && cache_again==cache;
        s.resources_stable=s.resources_read;
        for(size_t i=0;i<8;++i)if(s.colors[i]){uintptr_t again{};s.resources_stable=read(s.colors[i]+0x278,&again,8) && again==s.resources[i] && s.resources_stable;}
        if(s.depth){uintptr_t again{};s.resources_stable=read(s.depth+0x278,&again,8) && again==s.depth_resource && s.resources_stable;}
        return s;
    }
    static void write_state(std::ostream& out,const State& s){
        out<<"{\"fields_read\":"<<(s.fields_read?"true":"false")<<",\"fields_stable\":"<<(s.fields_stable?"true":"false")<<",\"resources_stable\":"<<(s.resources_stable?"true":"false")<<",\"counts_bounded\":"<<(s.counts_bounded?"true":"false")<<",\"resources_read\":"<<(s.resources_read?"true":"false")<<",\"prepared_count\":"<<s.prepared_count<<",\"effective_count\":"<<s.effective_count<<",\"dimensions\":["<<s.dimensions[0]<<','<<s.dimensions[1]<<"],\"subresource_words\":"<<s.subresource_words<<",\"colors\":[";
        for(size_t i=0;i<8;++i){if(i)out<<',';out<<"{\"wrapper\":"<<s.colors[i]<<",\"resource\":"<<s.resources[i]<<",\"read\":"<<(s.resource_read[i]?"true":"false")<<",\"rtv\":"<<s.rtvs[i]<<'}';}
        out<<"],\"depth_wrapper\":"<<s.depth<<",\"depth_resource\":"<<s.depth_resource<<",\"depth_read\":"<<(s.depth_read?"true":"false")<<",\"depth_present\":"<<(s.depth_present?"true":"false")<<",\"dsv\":"<<s.dsv<<",\"viewport_words\":[";
        for(size_t i=0;i<6;++i){if(i)out<<',';out<<s.viewport_words[i];}out<<"],\"scissor\":[";for(size_t i=0;i<4;++i){if(i)out<<',';out<<s.scissor[i];}out<<"]}";
    }
    static void write(std::ostream& out,const Pair& p){out<<"{\"supported\":"<<(p.supported?"true":"false")<<",\"identity_before\":"<<(p.identity_before?"true":"false")<<",\"identity_after\":"<<(p.identity_after?"true":"false")<<",\"same_call\":"<<(p.same_call?"true":"false")<<",\"atomic\":false,\"owned_eye_verified\":false,\"gpu_binding_verified\":false,\"before\":";write_state(out,p.before);out<<",\"after\":";write_state(out,p.after);out<<'}';}
};
