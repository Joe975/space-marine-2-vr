#pragma once
#include <Windows.h>
#include <sl.h>
#include <sl_dlss.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>

// Actual successful native option submissions, not inferred settings or SDK
// context ownership. No retained application pointers or feature enabling.
class StreamlineOptionsProbe {
public:
    struct Receipt {uint64_t epoch{},serial{};uint32_t viewport{},thread{};sl::DLSSOptions values;};
    static StreamlineOptionsProbe& get();
    void enable(){m_enabled=true;}
    void poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    void reset();
    sl::Result submit_observed(const sl::ViewportHandle& viewport,const sl::DLSSOptions& values){return m_installed?options(viewport,values):sl::Result::eErrorMissingInputParameter;}
    std::optional<Receipt> sample(uint32_t viewport)const;
    bool current(const Receipt&)const;
    static void write(std::ostream&,const std::optional<Receipt>&);
    void write_status(std::ostream&)const;
private:
    static sl::Result options(const sl::ViewportHandle&,const sl::DLSSOptions&);
    static bool copy(const sl::DLSSOptions&,sl::DLSSOptions&);
    std::atomic_bool m_enabled{},m_installed{};std::atomic<PFun_slDLSSSetOptions*> m_original{};
    mutable std::mutex m_mutex;std::mutex m_install;std::map<uint32_t,Receipt> m_options;
    uint64_t m_epoch{1},m_serial{},m_calls{},m_invalid{},m_rejected{};unsigned m_active{};bool m_overflow{};
    std::chrono::steady_clock::time_point m_next{};
};
