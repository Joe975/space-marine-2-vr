#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <ostream>

// Passive owned POD only. Offset labels deliberately do not infer ownership or
// semantics of opaque shading/previous-camera fields. No native writes/leases.
struct NativeCameraPacket {
    using Block=std::array<uint8_t,0xa60>;
    using Reader=std::function<bool(uintptr_t,Block&)>;
    inline static constexpr std::array<size_t,26> OFFSETS{0x34,0x3dc,0x45c,0x49c,0x4dc,0x51c,0x55c,0x59c,0x61c,0x65c,0x6b4,0x6f4,0x734,0x7b4,0x7f4,0x41c,0x5dc,0x774,0x834,0x874,0x8b4,0x8f4,0x934,0x974,0x9b4,0x9f4};
    inline static constexpr std::array<const char*,26> LABELS{"0x34","0x3dc","0x45c","0x49c","0x4dc","0x51c","0x55c","0x59c","0x61c","0x65c","0x6b4","0x6f4","0x734","0x7b4","0x7f4","0x41c","0x5dc","0x774","0x834","0x874","0x8b4","0x8f4","0x934","0x974","0x9b4","0x9f4"};
    uintptr_t entry{};
    std::array<std::array<uint32_t,16>,26> matrices{};
    std::array<uint32_t,2> jitter_words{};
    uint32_t jitter_index{};
    std::array<uint32_t,6> motion_words{};
    std::array<uint8_t,2> motion_flags{};
    bool operator==(const NativeCameraPacket&)const=default;
    static std::optional<NativeCameraPacket> capture(uintptr_t address,const Reader& reader={}) {
        if(!address || address>UINTPTR_MAX-sizeof(Block))return std::nullopt;
        const auto read=[&](Block& value){if(reader)return reader(address,value);SIZE_T bytes{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),value.data(),value.size(),&bytes) && bytes==value.size();};
        Block before{},after{};try{if(!read(before) || !read(after) || before!=after)return std::nullopt;}catch(...){return std::nullopt;}
        NativeCameraPacket packet;packet.entry=address;
        for(size_t i=0;i<OFFSETS.size();++i)std::memcpy(packet.matrices[i].data(),before.data()+OFFSETS[i],64);
        std::memcpy(packet.jitter_words.data(),before.data()+0x6a8,8);std::memcpy(&packet.jitter_index,before.data()+0x6b0,4);
        std::memcpy(packet.motion_flags.data(),before.data()+0xa34,2);std::memcpy(packet.motion_words.data(),before.data()+0xa38,24);
        return packet;
    }
    void write(std::ostream& out)const {
        out<<"{\"model\":\"native_camera_packet_v2\",\"entry\":"<<entry<<",\"stable_double_read\":true,\"resource_leases\":false,\"matrix_words\":{";
        for(size_t i=0;i<matrices.size();++i){if(i)out<<',';out<<'"'<<LABELS[i]<<"\":[";for(size_t n=0;n<16;++n){if(n)out<<',';out<<matrices[i][n];}out<<']';}
        out<<"},\"jitter_words\":["<<jitter_words[0]<<','<<jitter_words[1]<<"],\"jitter_index\":"<<jitter_index<<",\"motion_flags\":["<<unsigned(motion_flags[0])<<','<<unsigned(motion_flags[1])<<"],\"motion_words\":[";
        for(size_t i=0;i<motion_words.size();++i){if(i)out<<',';out<<motion_words[i];}out<<"]}";
    }
};
