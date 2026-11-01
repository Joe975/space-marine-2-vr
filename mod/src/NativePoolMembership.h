#pragma once
#include "NativeTextureContract.h"
#include "NativePoolReleaseBytes.h"
#include "GameBuild.h"
#include <algorithm>

// Mirrors only the guarded native release lookup. Never invokes release or
// changes pool/native references. An absent key is evidence, not a lifetime grant.
namespace native_pool_membership {
constexpr uintptr_t RELEASE_RVA=0x2079230;
constexpr size_t MAX_CHAIN=64;
struct Header {int32_t count{},capacity{};uintptr_t buckets{};bool operator==(const Header&)const=default;};
static_assert(sizeof(Header)==16);
struct Node {uintptr_t address{},next{},key{};int32_t record{-1};bool operator==(const Node&)const=default;};
enum class Refusal:uint32_t {none,address,header,chain,changed};
struct Result {
    uintptr_t pool{},key{};Header header;int32_t record_count{},bucket{-1};uintptr_t head{};
    std::vector<Node> nodes;bool known{},found{};int32_t record{-1};Refusal refusal{Refusal::address};
    bool operator==(const Result&)const=default;
};
inline bool matches(uint32_t stamp,const std::array<uint8_t,RELEASE_BYTES.size()>& bytes){return game_build::code(stamp,RELEASE_RVA,bytes,RELEASE_BYTES);}
inline uint32_t hash(uintptr_t key){
    uint64_t x=uint64_t(key);x=(x^(x>>33))*0xff51afd7ed558ccdULL;x=(x^(x>>33))*0xc4ceb9fe1a85ec53ULL;
    return uint32_t(x^(x>>33));
}
inline Result once(uintptr_t pool,uintptr_t key,int32_t records){
    using native_texture_contract::read;Result value;value.pool=pool;value.key=key;value.record_count=records;
    if(!pool || pool>UINTPTR_MAX-0x22e0 || !key || records<1 || records>4096)return value;
    value.refusal=Refusal::header;
    if(!read(pool+0x22d0,value.header) || value.header.count<0 || value.header.count>1048576)return value;
    if(value.header.count){
        if(value.header.capacity<1 || value.header.capacity>1048576 || !value.header.buckets || value.header.buckets>UINTPTR_MAX-size_t(value.header.capacity)*8)return value;
        value.bucket=int32_t(hash(key)%uint32_t(value.header.capacity));
        if(!read(value.header.buckets+size_t(value.bucket)*8,value.head))return value;
        value.refusal=Refusal::chain;
        auto cursor=value.head;
        while(cursor){
            if(value.nodes.size()>=MAX_CHAIN || cursor>UINTPTR_MAX-20 || std::ranges::find(value.nodes,cursor,&Node::address)!=value.nodes.end())return value;
            Node node;node.address=cursor;
            if(!read(cursor,node.next) || !read(cursor+8,node.key) || !read(cursor+16,node.record) || node.record< -1 || node.record>=records)return value;
            value.nodes.push_back(node);
            if(node.key==key){value.found=true;value.record=node.record;break;}
            cursor=node.next;
        }
    }
    value.known=true;value.refusal=Refusal::none;return value;
}
inline Result lookup(uintptr_t pool,uintptr_t key,int32_t records){
    auto first=once(pool,key,records);if(!first.known)return first;
    const auto second=once(pool,key,records);
    if(!second.known || first!=second){first.known=false;first.refusal=Refusal::changed;}
    return first;
}
inline void write(std::ostream& out,const Result& value){
    out<<"{\"pool\":"<<value.pool<<",\"key\":"<<value.key<<",\"record_count\":"<<value.record_count<<",\"known\":"<<(value.known?"true":"false")<<",\"found\":"<<(value.found?"true":"false")<<",\"record\":"<<value.record<<",\"refusal\":"<<uint32_t(value.refusal)<<",\"count\":"<<value.header.count<<",\"capacity\":"<<value.header.capacity<<",\"buckets\":"<<value.header.buckets<<",\"bucket\":"<<value.bucket<<",\"head\":"<<value.head<<",\"nodes\":[";
    for(size_t i=0;i<value.nodes.size();++i){if(i)out<<',';const auto& n=value.nodes[i];out<<"{\"address\":"<<n.address<<",\"next\":"<<n.next<<",\"key\":"<<n.key<<",\"record\":"<<n.record<<'}';}out<<"]}";
}
}
