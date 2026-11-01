#pragma once
#include "EngineBodyProbe.h"
#include "EngineChainswordRev.h"
#include "VrControllerInput.h"

inline EngineChainswordRev::Request read_chainsword_rev_request(
    const EngineBodyProbe& body,const VrControllerInput& input) {
    // Each source evaluates freshness while holding its publication lock. A
    // shared earlier timestamp can turn a newly published hold into a release.
    const auto actor=body.active_actor();
    return {actor,input.snapshot().chainsword_rev};
}
