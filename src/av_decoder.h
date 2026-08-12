#pragma once
/*  MUSICO VERSE 2.0 — av_decoder.h
 *  FFmpeg-backed multi-format audio decoder.
 */
#include <string>
#include <vector>
#include <atomic>
#include <span>
#include <cstdint>

struct TrackMeta {
    std::string title;
    std::string artist;
    std::string album;
    std::string format;
    int         duration_sec  = 0;
    int         year          = 0;
    uint32_t    sample_rate   = 44100;
    uint32_t    channels      = 2;
    uint32_t    bps           = 32;
};

class AvDecoder {
public:
    AvDecoder();
    ~AvDecoder();

    // Open any container FFmpeg supports (flac, mp3, m4a, opus, ogg, wav …).
    // Returns false and leaves decoder closed on failure.
    [[nodiscard]] bool open(const std::string& path);
    void close();

    // Decode up to max_frames of interleaved float stereo PCM into out[].
    // Returns frames written; 0 = EOF; -1 = error.
    int decode_next(std::span<float> out, int max_frames);
    int decode_next(float* out, int max_frames);

    bool   seek(double seconds);
    [[nodiscard]] double position() const;

    [[nodiscard]] const TrackMeta& meta()    const { return meta_; }
    [[nodiscard]] bool             is_open() const { return open_; }
    [[nodiscard]] bool             is_eof()  const { return eof_.load(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    TrackMeta         meta_;
    std::atomic<bool> eof_{false};
    bool              open_ = false;

    std::vector<float> leftover_;
    int                leftover_pos_ = 0;
    double             last_pos_sec_ = 0.0;
};
