#pragma once
#include "EyeTextureCapture.h"
#include "VrPhoto.h"
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// Saves one eye image as a PNG without touching the frame (docs/VIEWER_MODE.md). The frame path only
// adds one copy to the eye work it records anyway (EyeTextureCapture::tap); a thread of the save's own
// makes the readback buffer, waits for that copy, converts, encodes and writes. Nothing here ever
// waits on the GPU or the disk on a frame thread. All calls but save_png are thread-safe and quick.
class EyePhoto {
public:
    using Clock=vr_photo::Clock;
    struct Result {
        vr_photo::Outcome outcome{vr_photo::Outcome::saved};
        std::filesystem::path path;   // The file, or the folder that couldn't be written.
        uint32_t width{},height{};    // The picture: the view's true shape at the eye's pixel count.
        uint32_t eye_width{},eye_height{}; // The eye texture it came from.
        uint64_t bytes{};             // Size on disk.
        HRESULT hr{S_OK};             // What failed, for the log.
        double wait_ms{},copy_ms{},encode_ms{}; // Click to the copy being recorded; the GPU copy; convert, encode and write.
    };
    // folder: where pictures go; empty: default_folder(), looked up on the save thread.
    explicit EyePhoto(std::filesystem::path folder={}):m_folder(std::move(folder)) {}
    void folder(std::filesystem::path value) {std::lock_guard lock{m_mutex};m_folder=std::move(value);}
    EyePhoto(const EyePhoto&)=delete;
    EyePhoto& operator=(const EyePhoto&)=delete;
    ~EyePhoto();
    // A click. False: a picture is in flight, so this click takes nothing (poll() won't report it).
    bool request(const vr_photo::Stamp&,Clock::time_point now);
    // Cheap: true while a frame should call frame().
    bool wanted() const;
    // A frame with a usable eye: the first call starts the save, later ones hand over the tap for this
    // frame's capture once the buffer exists, until tapped() takes it. view_aspect: the width over the
    // height of what the eye sees (vr_photo::view_aspect); the picture takes that shape.
    std::optional<EyeTextureCapture::Tap> frame(ID3D12Device*,ID3D12Resource* eye,float view_aspect,Clock::time_point now);
    void tapped(const EyeTextureCapture::Tapped&,Clock::time_point now);
    // The picture's end, once: saved or why not. Also ends a click that found no eye image.
    std::optional<Result> poll(Clock::time_point now);
    // ~/Pictures/Space Marine 2 VR, wherever Windows keeps Pictures (OneDrive, a moved folder). Empty if it can't say.
    static std::filesystem::path default_folder();
    // RGB bytes in BGR order, width*3 a row, resampled to the picture's size (none: as they are).
    // Writes the whole file or nothing: path holds no partial PNG.
    static HRESULT save_png(const std::filesystem::path&,const uint8_t* bgr,uint32_t width,uint32_t height,vr_photo::Size picture={});
private:
    struct Job;
    static void run(const std::shared_ptr<Job>&);
    mutable std::mutex m_mutex;
    std::filesystem::path m_folder;
    vr_photo::Flight m_flight;
    vr_photo::Stamp m_stamp;
    std::shared_ptr<Job> m_job;
};
