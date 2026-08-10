/*  CLI.MUSIC.COM — visualizer.cpp  v3.0
 *
 *  Core logic identical to original v2.0.
 *  Removed: MIRROR, WAVE, FIRE, DOTS, SPECTRUM, RAIN.
 *  Kept:    BARS (braille bars + peak-hold) and SCOPE (oscilloscope).
 *
 *  Band fix:  compute_bands_locked() is called from push_samples() with
 *             MAX_BANDS (32) every time — not from render().  The `bands`
 *             param in render() only controls how many of those 32 values
 *             are mapped to screen columns (16 = coarser, 32 = fine detail).
 *
 *  Smoothness: interpolate_cols() uses cosine interpolation so bar edges
 *              blend instead of stepping sharply between frequency buckets.
 */
#include "visualizer.h"
#include "kissfft/kiss_fft.h"
#include <cmath>
#include <algorithm>
#include <numeric>

// ─── init ─────────────────────────────────────────────────────────────────────
void Visualizer::init(int w, int h) {
    std::lock_guard lk(mtx_);
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == width_ && h == height_ && !ring_.empty()) return;
    width_  = w;
    height_ = h;
    ring_.fill(0.0f);
    pcm_ring_.fill(0.0f);
    ring_write_ = pcm_write_ = 0;
    smooth_.fill(0.0f);
    peak_cols_.assign(w, 0.0f);
    peak_hold_timer_.assign(w, 0);
}

// ─── push_samples ─────────────────────────────────────────────────────────────
void Visualizer::push_samples(std::span<const float> s, int channels, int sr) {
    std::lock_guard lk(mtx_);
    sample_rate_ = sr;
    int frames = (int)s.size() / std::max(1, channels);
    for (int i = 0; i < frames; ++i) {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c) m += s[i * channels + c];
        m /= (float)channels;
        ring_[ring_write_]    = m;
        pcm_ring_[pcm_write_] = m;
        ring_write_  = (ring_write_  + 1) % FFT_SIZE_VIZ;
        pcm_write_   = (pcm_write_   + 1) % (FFT_SIZE_VIZ * 2);
    }
    compute_bands_locked(MAX_BANDS);   // always compute all 32 bands
}

void Visualizer::push_samples(const float* s, int frames, int channels, int sr) {
    push_samples(std::span<const float>(s, frames * channels), channels, sr);
}

// ─── FFT + band energy (always 32 bands, mirrored) ───────────────────────────
void Visualizer::compute_bands_locked(int bands) {
    bands = std::clamp(bands, 16, MAX_BANDS);

    std::vector<kiss_fft_cpx> in(FFT_SIZE_VIZ), out(FFT_SIZE_VIZ);
    for (int i = 0; i < FFT_SIZE_VIZ; ++i) {
        int idx = (ring_write_ + i) % FFT_SIZE_VIZ;
        float w = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (FFT_SIZE_VIZ - 1)));
        in[i] = {ring_[idx] * w, 0.0f};
    }

    auto* cfg = kiss_fft_alloc(FFT_SIZE_VIZ, 0);
    kiss_fft(cfg, in.data(), out.data());
    kiss_fft_free(cfg);

    const float NORM = (float)FFT_SIZE_VIZ * (float)FFT_SIZE_VIZ;
    const float fr   = (float)sample_rate_ / (float)FFT_SIZE_VIZ;

    // 16 log-spaced frequency edges → 16 half-bands mirrored to 32 total
    static constexpr float EDGES[17] = {
        30,   50,   80,   120,  180,  250,  350,  500,
        700, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 12000
    };
    constexpr int HALF = MAX_BANDS / 2;   // 16

    float right[HALF] = {};
    for (int b = 0; b < HALF; ++b) {
        int lo = std::max(1, (int)(EDGES[b]     / fr));
        int hi = std::max(lo + 1, (int)(EDGES[b + 1] / fr));
        hi = std::min(hi, FFT_SIZE_VIZ / 2);
        float e = 0.0f;
        for (int k = lo; k < hi; ++k) e += std::norm(out[k]);
        e /= (float)(hi - lo);
        float db = 10.0f * std::log10(e / NORM + 1e-12f);
        right[b] = std::clamp(db + 70.0f, 0.0f, 70.0f);
    }

    // Mirror: symmetric around centre — left side mirrors right side
    float full[MAX_BANDS];
    for (int i = 0; i < HALF; ++i) full[i]       = right[HALF - 1 - i];
    for (int i = 0; i < HALF; ++i) full[HALF + i] = right[i];

    // Exponential smoothing  density 1=fluid … 10=dense
    float t    = (density_ - 1) / 9.0f;
    float UP   = 0.55f - t * 0.47f;    // attack:  0.55 → 0.08
    float DOWN = 0.30f - t * 0.27f;    // release: 0.30 → 0.03
    for (int b = 0; b < MAX_BANDS; ++b) {
        float a = (full[b] > smooth_[b]) ? UP : DOWN;
        smooth_[b] = smooth_[b] * (1.0f - a) + full[b] * a;
    }
}

// ─── Column interpolation (cosine) ───────────────────────────────────────────
std::vector<float> Visualizer::interpolate_cols(
    const std::array<float, MAX_BANDS>& snap,
    int bands,
    float scale) const
{
    std::vector<float> cols(width_, 0.0f);
    // Centre x of each band bucket in screen-column space
    auto band_cx = [&](int b) {
        return (b + 0.5f) / (float)bands * (float)width_;
    };

    for (int col = 0; col < width_; ++col) {
        float cx = (float)col + 0.5f;
        // Find left bucket
        int bl = 0;
        for (int b = 0; b < bands; ++b) if (band_cx(b) <= cx) bl = b;
        int   br = std::min(bl + 1, bands - 1);
        float xl = band_cx(bl), xr = band_cx(br);
        float t  = (xr > xl) ? (cx - xl) / (xr - xl) : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        // Cosine blend — removes the sharp staircase between buckets
        float tc  = (1.0f - std::cos(t * (float)M_PI)) * 0.5f;
        float val = snap[bl] * (1.0f - tc) + snap[br] * tc;
        // Normalise to sub-cell height units (1 row = 4 sub-cells in braille)
        cols[col] = (val / 70.0f) * (float)(height_ * 4) * scale;
    }
    return cols;
}

// ─── Peak-hold helper ─────────────────────────────────────────────────────────
static void update_peaks(std::vector<float>& peaks,
                          std::vector<int>&   timers,
                          const std::vector<float>& cols,
                          int width)
{
    constexpr int   HOLD_FRAMES = 18;    // ~0.6 s at 30 fps
    constexpr float DROP_SPEED  = 0.5f;

    if ((int)peaks.size() != width) {
        peaks.assign(width, 0.0f);
        timers.assign(width, 0);
    }
    for (int c = 0; c < width; ++c) {
        if (cols[c] > peaks[c]) {
            peaks[c]  = cols[c];
            timers[c] = HOLD_FRAMES;
        } else if (timers[c] > 0) {
            --timers[c];
        } else {
            peaks[c] = std::max(0.0f, peaks[c] - DROP_SPEED);
        }
    }
}

// ─── Style: BARS ─────────────────────────────────────────────────────────────
VisualizerFrame Visualizer::render_bars(const std::array<float,MAX_BANDS>& snap, int bands) {
    VisualizerFrame f;
    f.width = width_; f.height = height_; f.style = VizStyle::BARS;
    f.rows.assign(height_, std::vector<uint32_t>(width_, ' '));

    auto cols = interpolate_cols(snap, bands);
    if (peak_hold_) update_peaks(peak_cols_, peak_hold_timer_, cols, width_);

    for (int col = 0; col < width_; ++col) {
        int tot = (int)std::round(cols[col]);
        for (int row = height_ - 1; row >= 0 && tot > 0; --row) {
            int u = std::min(4, tot); tot -= u;
            f.rows[row][col] = (uint32_t)BRAILLE_LEVEL[u];
        }
        // Peak-hold dot
        if (peak_hold_ && peak_cols_[col] > 0.5f) {
            int pr = height_ - 1 - (int)(peak_cols_[col] / 4);
            pr = std::clamp(pr, 0, height_ - 1);
            if (f.rows[pr][col] == ' ')
                f.rows[pr][col] = 0x2022u;   // •
        }
    }
    return f;
}

// ─── Style: SCOPE (oscilloscope waveform with connecting lines) ───────────────
VisualizerFrame Visualizer::render_scope() {
    VisualizerFrame f;
    f.width = width_; f.height = height_; f.style = VizStyle::SCOPE;
    f.rows.assign(height_, std::vector<uint32_t>(width_, ' '));

    const int PCM_LEN = FFT_SIZE_VIZ * 2;
    std::array<float, FFT_SIZE_VIZ * 2> snap;
    int w0 = pcm_write_;
    for (int i = 0; i < PCM_LEN; ++i)
        snap[i] = pcm_ring_[(w0 + i) % PCM_LEN];

    for (int col = 0; col < width_; ++col) {
        int src = (int)((float)col / (float)width_ * PCM_LEN);
        src = std::clamp(src, 0, PCM_LEN - 1);
        float v = snap[src];   // -1..+1

        float row_f = ((1.0f - v) * 0.5f) * (float)(height_ * 4);
        row_f = std::clamp(row_f, 0.0f, (float)(height_ * 4 - 1));
        int row = (int)(row_f / 4);
        int sub = 3 - ((int)row_f % 4);
        row = std::clamp(row, 0, height_ - 1);
        f.rows[row][col] = (uint32_t)BRAILLE_LEVEL[std::max(1, sub)];

        // Fill the vertical gap between consecutive samples so the line
        // looks continuous rather than a scatter of isolated dots
        if (col > 0) {
            int src_prev = std::max(0, src - PCM_LEN / width_);
            float vp = snap[src_prev];
            float rf_prev = std::clamp(
                ((1.0f - vp) * 0.5f) * (float)(height_ * 4),
                0.0f, (float)(height_ * 4 - 1));
            int row_prev = std::clamp((int)(rf_prev / 4), 0, height_ - 1);
            if (row_prev != row) {
                int lo = std::min(row, row_prev);
                int hi = std::max(row, row_prev);
                for (int r = lo; r <= hi && r < height_; ++r)
                    if (f.rows[r][col] == ' ')
                        f.rows[r][col] = (uint32_t)BRAILLE_LEVEL[1];
            }
        }
    }
    return f;
}

// ─── Public render dispatch ───────────────────────────────────────────────────
VisualizerFrame Visualizer::render(VizStyle style, int bands) {
    bands = std::clamp(bands, 16, MAX_BANDS);
    std::array<float, MAX_BANDS> snap;
    {
        std::lock_guard lk(mtx_);
        snap = smooth_;
        if ((int)peak_cols_.size() != width_) {
            peak_cols_.assign(width_, 0.0f);
            peak_hold_timer_.assign(width_, 0);
        }
    }

    switch (style) {
        case VizStyle::SCOPE: {
            std::lock_guard lk(mtx_);
            return render_scope();
        }
        default:
            return render_bars(snap, bands);
    }
}
