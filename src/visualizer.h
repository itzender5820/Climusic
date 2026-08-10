#pragma once
/*  CLI.MUSIC.COM — visualizer.h  v3.0
 *  32-band FFT, peak hold.  Two render styles: BARS and SCOPE.
 *  Core logic ported unchanged from original; unused styles stripped.
 */
#include <vector>
#include <array>
#include <mutex>
#include <span>
#include "settings.h"

static constexpr int MAX_BANDS    = 32;
static constexpr int FFT_SIZE_VIZ = 2048;

// Braille sub-cell levels (0=empty … 4=full)
static constexpr wchar_t BRAILLE_LEVEL[5] = {
    L' ', L'\u28C0', L'\u28E4', L'\u28F6', L'\u28FF'
};

struct VisualizerFrame {
    std::vector<std::vector<uint32_t>> rows;
    int width = 0, height = 0;
    VizStyle style = VizStyle::BARS;
};

class Visualizer {
public:
    void init(int width, int height);
    void push_samples(std::span<const float> s, int channels, int sample_rate);
    void push_samples(const float* s, int frames, int channels, int sr);

    VisualizerFrame render(VizStyle style, int bands = 32);
    void set_peak_hold(bool on)  { peak_hold_ = on; }
    void set_density(int d)      { density_ = std::clamp(d, 1, 10); }

private:
    int  width_      = 80;
    int  height_     = 12;
    bool peak_hold_  = true;
    int  density_    = 5;

    std::array<float, FFT_SIZE_VIZ>     ring_{};
    int  ring_write_ = 0;
    int  sample_rate_ = 44100;

    std::array<float, FFT_SIZE_VIZ * 2> pcm_ring_{};
    int  pcm_write_  = 0;

    std::array<float, MAX_BANDS>        smooth_{};
    std::vector<float>                  peak_cols_;
    std::vector<int>                    peak_hold_timer_;

    mutable std::mutex mtx_;

    void compute_bands_locked(int bands);

    VisualizerFrame render_bars (const std::array<float,MAX_BANDS>& snap, int bands);
    VisualizerFrame render_scope();

    [[nodiscard]] std::vector<float> interpolate_cols(
        const std::array<float, MAX_BANDS>& snap,
        int bands,
        float scale = 1.0f) const;
};
