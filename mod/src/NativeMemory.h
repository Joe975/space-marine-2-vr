#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Fault-safe reads of native game memory. Each read is one ReadProcessMemory system call: it fails
// cleanly on unmapped or protected memory and never raises an exception. That call costs about a
// microsecond, and on 22 Oct the render thread spent about 8% of its time in it (own-queue A/B), mostly
// from hundreds of small field reads per frame. Read a contiguous native object once with read_block
// and take fields from the copy with Block::get. The result is the same as long as the whole object
// is readable, which is the case for every block read here (one native object).
namespace native_memory {
inline std::atomic<uint64_t>& calls() { static std::atomic<uint64_t> value{}; return value; } // Tests count system calls.
inline bool read_bytes(uintptr_t address,void* out,size_t size) {
    if(!address || !size || address>UINTPTR_MAX-size) return false;
    calls().fetch_add(1,std::memory_order_relaxed);
    SIZE_T bytes{};
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),out,size,&bytes) && bytes==size;
}
template<class T> bool read(uintptr_t address,T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return read_bytes(address,&value,sizeof(T));
}
// A copy of bytes [address, address+N) taken in one system call.
template<size_t N> struct Block {
    std::array<uint8_t,N> bytes{};
    bool read_from(uintptr_t address) { return read_bytes(address,bytes.data(),N); }
    template<class T> void get(size_t offset,T& value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(sizeof(T)<=N);
        if(offset>N-sizeof(T)) { std::memset(&value,0,sizeof(T)); return; } // Outside the block: never read past it.
        std::memcpy(&value,bytes.data()+offset,sizeof(T));
    }
    template<class T> T get(size_t offset) const { T value{}; get(offset,value); return value; }
};
}
