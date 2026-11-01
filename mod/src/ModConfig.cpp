#include "Mods.hpp"
#include "Sm2Probe.h"

#include <mods/VR.hpp>
#include <mods/VRConfig.hpp>

void VR::on_xinput_get_state(uint32_t*, uint32_t, XINPUT_STATE*) {}

// Sm2Probe owns the opt-in token-aware OpenVR session. The generic framework VR Present
// adapter is not registered: its parity/copy path must not submit the same images a second time.
Mods::Mods()
{
    m_mods.emplace_back(VRConfig::get());
    m_mods.emplace_back(Sm2Probe::get());
}
