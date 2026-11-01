#pragma once
// A debug/runtime implementation can forward one engine API call through
// another hooked command interface. Observe the engine's logical binding/writer
// once. Never suppress lifecycle, queue, barrier or extended-access evidence.
struct GpuApiForwardScope {
    inline static thread_local unsigned depth{};
    static bool active(){return depth!=0;}
    GpuApiForwardScope(){++depth;}
    ~GpuApiForwardScope(){--depth;}
    GpuApiForwardScope(const GpuApiForwardScope&)=delete;
};
