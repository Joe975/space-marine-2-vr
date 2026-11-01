#pragma once
#include <mutex>

// Publication and its freshness clock must share the same critical section.
// A clock sampled before lock acquisition can predate an already-published row.
template<class Mutex,class Clock,class Read>
decltype(auto) locked_clock_read(Mutex& mutex,Clock clock,Read read) {
    std::lock_guard lock{mutex};
    return read(clock());
}
