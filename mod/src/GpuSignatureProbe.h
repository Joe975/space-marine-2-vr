#pragma once
#include <d3d12.h>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <array>
#include <string_view>
#include "GpuIndirectEvidence.h"

// Process-lifetime creation observation, usable before Framework starts. Object
// metadata owns the evidence; the bounded map contains code trampolines only.
class GpuSignatureProbe {
public:
    struct Record {
        uint32_t magic{0x534d3253},version{1},thread{};uint64_t serial{},device{},signature{},callsite{},root{};
        GpuIndirectEvidence shape;
    };
    struct Status {bool early_installed{};uint64_t device_calls{},signature_calls{},annotated{},failed{};size_t implementations{};};
    static GpuSignatureProbe& get();
    static bool requested(std::string_view pip,std::string_view openvr,std::string_view openxr,std::string_view synthetic,std::string_view ordinary={},std::string_view ordinary_headset={}) {
        const auto off=[](auto value){return value.empty() || value=="0";};
        const bool desktop=(pip=="1" || ordinary=="1") && off(openvr) && off(openxr) && off(ordinary_headset);
        const bool single_runtime=(openvr=="1" && off(openxr)) || (openxr=="1" && off(openvr));
        const bool headset=ordinary=="1" && ordinary_headset=="1" && single_runtime && off(pip);
        return (desktop || headset) && off(synthetic);
    }
    bool install_device_creation();
    bool observe_device(ID3D12Device*);
    Status status() const;
    static std::optional<Record> read(ID3D12CommandSignature*);
    inline static constexpr GUID KEY{0x969b3ae1,0x7bf0,0x44cb,{0xa8,0xa5,0xd1,0x2d,0xb8,0x49,0x45,0x06}};
private:
    using CreateDevice=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    using Signature=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,REFIID,void**);
    static HRESULT WINAPI on_device(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    template<size_t Index> static HRESULT STDMETHODCALLTYPE on_signature(ID3D12Device*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,REFIID,void**);
    void annotate(ID3D12Device*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,void**,uintptr_t);
    mutable std::mutex m_mutex;std::map<void*,size_t> m_implementations;
    std::array<std::atomic<Signature>,16> m_originals{};
    std::atomic_bool m_early{};std::atomic<uint64_t> m_devices{},m_calls{},m_annotated{},m_failed{},m_serial{};
    inline static CreateDevice s_create_device{};
};
