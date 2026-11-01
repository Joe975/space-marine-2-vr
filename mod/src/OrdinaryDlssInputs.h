#pragma once
#include <Windows.h>
#include <sl_core_api.h>
#include <array>
#include <set>

// Call-local clone of the supported actual evaluation input chain. These native
// resources are valid for this API call, not retained COM leases. Never cache it.
class OrdinaryDlssInputs {
public:
    OrdinaryDlssInputs()=default;
    OrdinaryDlssInputs(const OrdinaryDlssInputs&)=delete;
    OrdinaryDlssInputs& operator=(const OrdinaryDlssInputs&)=delete;
    bool build(const sl::BaseStructure** inputs,uint32_t count,uint32_t source_viewport,uint32_t private_viewport){
        m_count=0;m_nodes_count=0;m_tags_count=0;m_width=m_height=0;m_valid=false;std::set<uintptr_t> visited;std::set<uint32_t> types;unsigned views{};
        if(!count || count>16 || !inputs || source_viewport==private_viewport)return false;
        const auto base=reinterpret_cast<uintptr_t>(inputs);if(base>UINTPTR_MAX-count*sizeof(void*))return false;
        for(uint32_t i=0;i<count;++i){const sl::BaseStructure* next{};if(!read(reinterpret_cast<const sl::BaseStructure*const*>(base+i*sizeof(void*)),next) || !next)return false;sl::BaseStructure* previous{};
            while(next){const auto address=reinterpret_cast<uintptr_t>(next);if(m_nodes_count==32 || !visited.insert(address).second)return false;
                sl::BaseStructure header{{},0};if(!read(next,header) || header.structVersion!=1)return false;sl::BaseStructure* node{};const auto n=m_nodes_count++;
                if(header.structType==sl::ViewportHandle::s_structType){sl::ViewportHandle value;if(!read(reinterpret_cast<const sl::ViewportHandle*>(next),value) || value.structType!=header.structType || value.structVersion!=header.structVersion || value.next!=header.next || uint32_t(value)!=source_viewport || ++views!=1)return false;
                    m_views[n]=sl::ViewportHandle{private_viewport};node=&m_views[n];
                }else if(header.structType==sl::ResourceTag::s_structType){
                    if(m_tags_count==16 || !read(reinterpret_cast<const sl::ResourceTag*>(next),m_tags[n]))return false;auto& tag=m_tags[n];auto& resource=m_resources[n];
                    if(tag.structType!=header.structType || tag.structVersion!=1 || !types.insert(uint32_t(tag.type)).second ||
                        (tag.type!=sl::kBufferTypeDepth && tag.type!=sl::kBufferTypeMotionVectors && tag.type!=sl::kBufferTypeScalingInputColor && tag.type!=sl::kBufferTypeScalingOutputColor && tag.type!=sl::kBufferTypeExposure) ||
                        tag.lifecycle!=sl::ResourceLifecycle::eValidUntilEvaluate || !read(tag.resource,resource) || resource.structType!=sl::Resource::s_structType || resource.structVersion!=1 || resource.next || !resource.native || resource.type!=sl::ResourceType::eTex2d)return false;
                    if(tag.type==sl::kBufferTypeScalingInputColor){
                        if(!resource.width || !resource.height)return false;
                        if(tag.extent.width || tag.extent.height){if(!tag.extent.width || !tag.extent.height || uint64_t(tag.extent.left)+tag.extent.width>resource.width || uint64_t(tag.extent.top)+tag.extent.height>resource.height)return false;m_width=tag.extent.width;m_height=tag.extent.height;}
                        else {if(tag.extent.left || tag.extent.top)return false;m_width=resource.width;m_height=resource.height;}
                        if(m_width>32768 || m_height>32768)return false;
                    }
                    tag.resource=&resource;node=&tag;++m_tags_count;
                }else return false;
                // Read the next link from the same owned typed copy, rejecting a
                // changing header instead of splicing inconsistent observations.
                if(header.structType==sl::ResourceTag::s_structType && m_tags[n].next!=header.next)return false;
                node->next=nullptr;if(previous)previous->next=node;else m_inputs[i]=node;previous=node;next=header.next;
            }
        }
        if(views!=1 || !types.contains(sl::kBufferTypeDepth) || !types.contains(sl::kBufferTypeMotionVectors) || !types.contains(sl::kBufferTypeScalingInputColor) || !types.contains(sl::kBufferTypeScalingOutputColor))return false;
        m_count=count;m_valid=true;return true;
    }
    const sl::BaseStructure** data(){return m_valid?m_inputs.data():nullptr;}
    uint32_t count()const{return m_valid?m_count:0;}
    uint32_t render_width()const{return m_valid?m_width:0;}
    uint32_t render_height()const{return m_valid?m_height:0;}
private:
    template<class T>static bool read(const T* p,T& out){SIZE_T bytes{};return p && ReadProcessMemory(GetCurrentProcess(),p,&out,sizeof(T),&bytes) && bytes==sizeof(T);}
    std::array<const sl::BaseStructure*,16> m_inputs{};
    std::array<sl::ViewportHandle,32> m_views;
    std::array<sl::ResourceTag,32> m_tags;
    std::array<sl::Resource,32> m_resources;
    uint32_t m_count{},m_nodes_count{},m_tags_count{},m_width{},m_height{};bool m_valid{};
};
