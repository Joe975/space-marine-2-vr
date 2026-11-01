#include "EngineOutputSize.h"
#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <string>

namespace engine_output_size {

Hook& Hook::get()
{
    static auto* instance = new Hook;
    return *instance;
}

std::string Hook::status() const
{
    const auto s = snapshot();
    return "requested=" + std::string{s.requested ? "true" : "false"} + " installed=" + (s.installed ? "true" : "false") +
        " size=" + std::to_string(s.size.width) + "x" + std::to_string(s.size.height) + " mode=" + (s.mode == Mode::config ? "config" : "device") +
        " creates=" + std::to_string(s.creates) + " resizes=" + std::to_string(s.resizes) + " loads=" + std::to_string(s.loads) +
        " refused=" + std::to_string(s.refused) + " windowed=" + std::to_string(s.windowed) + " window_old_mode=" + std::to_string(s.window.old_mode) + " last_old=" + std::to_string(s.last.old_width) + "x" + std::to_string(s.last.old_height);
}

void Hook::record(uint8_t* config, uint64_t Snapshot::*counter)
{
    std::optional<Change> change;
    Size size{};
    {
        std::lock_guard lock{m_mutex};
        size = m_snapshot.size;
    }
    try { change = apply(config, size); } catch (...) {}
    bool log = false;
    {
        std::lock_guard lock{m_mutex};
        ++(m_snapshot.*counter);
        if (change) { m_snapshot.last = *change; } else { ++m_snapshot.refused; }
        log = m_snapshot.*counter <= 8;
    }
    if (log) {
        const char* what = counter == &Snapshot::creates ? "create" : counter == &Snapshot::resizes ? "resize" : "load";
        if (change) spdlog::info("EngineOutputSize: {} {}x{} -> {}x{} written={}", what, change->old_width, change->old_height, size.width, size.height, change->written);
        else spdlog::warn("EngineOutputSize: {} refused (config does not hold a plausible output size)", what);
    }
}

bool Hook::on_create(void* self, void* a2, void* queue, uint32_t format, void* window, void* a6, uint8_t* config)
{
    auto& hook = get();
    hook.record(config, &Snapshot::creates);
    return hook.m_create.load()(self, a2, queue, format, window, a6, config);
}

void Hook::on_resize(void* self, uint8_t* config, uint32_t format)
{
    auto& hook = get();
    hook.record(config, &Snapshot::resizes);
    hook.m_resize.load()(self, config, format);
}

void Hook::on_load(uint8_t* config)
{
    auto& hook = get();
    hook.m_load.load()(config);
    hook.record(config, &Snapshot::loads);
    // Only here, on a VideoConfig: the device's output follows the window size in windowed mode
    // and the desktop in borderless. (The create/resize argument is the device's own block,
    // where these offsets mean something else, so it is never touched there.)
    Size size{};
    uint64_t loads{};
    {
        std::lock_guard lock{hook.m_mutex};
        size = hook.m_snapshot.size;
        loads = hook.m_snapshot.loads;
    }
    std::optional<WindowChange> change;
    try { change = apply_windowed(config, size); } catch (...) {}
    {
        std::lock_guard lock{hook.m_mutex};
        if (change) { hook.m_snapshot.window = *change; ++hook.m_snapshot.windowed; } else { ++hook.m_snapshot.refused; }
    }
    if (loads <= 8) {
        if (change) spdlog::info("EngineOutputSize: load window mode {} {}x{} -> windowed {}x{} written={}", change->old_mode, change->old_width, change->old_height,
            size.width, size.height, change->written);
        else spdlog::warn("EngineOutputSize: load window refused (mode or window size not plausible)");
    }
}

void Hook::install_from_environment()
{
    char text[32]{}, mode_text[16]{};
    const auto length = GetEnvironmentVariableA("SM2VR_OUTPUT_SIZE", text, sizeof text);
    if (length == 0) return;
    const auto mode_length = GetEnvironmentVariableA("SM2VR_OUTPUT_SIZE_MODE", mode_text, sizeof mode_text);
    const auto size = length < sizeof text ? parse({text, length}) : std::nullopt;
    const auto mode = mode_length < sizeof mode_text ? parse_mode({mode_text, mode_length}) : std::nullopt;
    {
        std::lock_guard lock{m_mutex};
        m_snapshot.requested = true;
        if (size) m_snapshot.size = *size;
        if (mode) m_snapshot.mode = *mode;
    }
    if (!size || !mode) { spdlog::error("EngineOutputSize: invalid SM2VR_OUTPUT_SIZE='{}' or mode; not hooking", text); return; }

    const auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto stamp = nt->FileHeader.TimeDateStamp;
    void* const detours[3]{reinterpret_cast<void*>(&on_create), reinterpret_cast<void*>(&on_resize), reinterpret_cast<void*>(&on_load)};
    const Target targets[3]{Target::create, Target::resize, Target::load};
    const size_t count = *mode == Mode::config ? 3 : 2;
    uintptr_t rvas[3]{};
    // Verify every target before hooking any of them: all or nothing.
    for (size_t i = 0; i < count; ++i) {
        rvas[i] = rva(stamp, targets[i]);
        const auto length = code_length(targets[i]);
        if (!rvas[i] || rvas[i] + length > nt->OptionalHeader.SizeOfImage || !matches(stamp, targets[i], {base + rvas[i], length})) {
            spdlog::error("EngineOutputSize: build {:#x} {} code not verified; not hooking", stamp, name(targets[i]));
            return;
        }
    }
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) { spdlog::error("EngineOutputSize: MinHook init {}", MH_StatusToString(status)); return; }
    void* created[3]{};
    for (size_t i = 0; i < count; ++i) {
        auto* target = const_cast<uint8_t*>(base) + rvas[i];
        void* original{};
        status = MH_CreateHook(target, detours[i], &original);
        if (status != MH_OK) break;
        // Stored before enabling so a detour can never run without its original.
        if (i == 0) m_create.store(reinterpret_cast<CreateFn*>(original));
        else if (i == 1) m_resize.store(reinterpret_cast<ResizeFn*>(original));
        else m_load.store(reinterpret_cast<LoadFn*>(original));
        created[i] = target;
    }
    if (status == MH_OK) {
        for (size_t i = 0; i < count && status == MH_OK; ++i) status = MH_EnableHook(created[i]);
    }
    if (status != MH_OK) {
        for (auto* target : created) if (target) { MH_DisableHook(target); MH_RemoveHook(target); }
        spdlog::error("EngineOutputSize: hook failed: {}", MH_StatusToString(status));
        return;
    }
    {
        std::lock_guard lock{m_mutex};
        m_snapshot.installed = true;
    }
    spdlog::info("EngineOutputSize: installed size={}x{} mode={} build={:#x}", size->width, size->height, *mode == Mode::config ? "config" : "device", stamp);
}

} // namespace engine_output_size
