#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

// Fixed-size observation tables keyed by a game object (bodies, camera trackers, weapon controls,
// UI movies). Before 26 Oct a full table dropped every new object for the rest of the session: the
// objects of earlier levels kept their rows, so after enough level loads the new player's camera
// tracker had no row, EngineBodyProbe saw no player actor, owned readiness refused every job
// (actor_missing) and the headset stayed on the flat panel (26 Oct playtest, 11:19: trackers=32,
// overflow climbing). A full table now gives the row least recently seen to the new object, if that
// row is at least STALE old (one not seen for a second belongs to a gone object). Live rows are
// never evicted: with every row fresh the new object is dropped and counted, as before.
// Row needs `calls` (uint64_t) and `received` (steady_clock::time_point).
namespace bounded_table {
inline constexpr auto STALE=std::chrono::seconds(1);
enum class Result {updated,added,evicted,dropped};
template<class Row,class Match>
Result upsert(std::vector<Row>& rows,Row sample,Match match,size_t capacity,std::chrono::steady_clock::duration stale=STALE){
    const auto found=std::find_if(rows.begin(),rows.end(),match);
    if(found!=rows.end()){sample.calls=found->calls+1;*found=sample;return Result::updated;}
    sample.calls=1;
    if(rows.size()<capacity){rows.push_back(sample);return Result::added;}
    const auto oldest=std::min_element(rows.begin(),rows.end(),[](const Row& a,const Row& b){return a.received<b.received;});
    if(oldest!=rows.end() && sample.received>=oldest->received && sample.received-oldest->received>=stale){*oldest=sample;return Result::evicted;}
    return Result::dropped;
}
}
