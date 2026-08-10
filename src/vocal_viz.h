#pragma once
/*  CLI.MUSIC.COM — vocal_viz.h
 *
 *  Stereo cross-correlation visualizer.
 *  Maps inter-channel delay (lag) to 21 spatial columns:
 *
 *  Col:  0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15   16   17   18   19   20
 *  ms:  80   60   40   20   10    5    4    3    2    1    0    1    2    3    4    5   10   20   40   60   80
 *        ←──────────── L leads (panned-left content) ──────────┤ C ├── R leads (panned-right) ───────────────→
 *
 *  At lag=0 (col 10) L and R are in-phase → mostly vocals.
 *  At |lag|>0 the energy reflects stereo-panned / delayed content (instruments, reverb, effects).
 *
 *  Algorithm: windowed normalised cross-correlation.
 *    xcorr(τ) = Σ L[t] * R[t+τ]  over WINDOW samples, then |·| and smooth.
 *  Zero extra dependencies — uses only the stereo PCM already decoded.
 */
#include <array>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <span>

class VocalVisualizer {
public:
    static constexpr int N_SLOTS = 21;

    // Delay in ms for each column (symmetric around centre col 10)
    static constexpr float DELAY_MS[N_SLOTS] = {
        80, 60, 40, 20, 10, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 10, 20, 40, 60, 80
    };

    // Push interleaved stereo float PCM (same data the main visualizer receives)
    void push(const float* pcm, int frames, int channels, int sr) {
        std::lock_guard lk(mtx_);
        if (sr > 0) sr_ = sr;
        for (int i = 0; i < frames; ++i) {
            float l = pcm[i * channels];
            float r = (channels > 1) ? pcm[i * channels + 1] : l;
            L_[w_] = l;
            R_[w_] = r;
            w_ = (w_ + 1) & (BUF - 1);
        }
    }

    // Returns N_SLOTS smoothed energy values (0..1) for the 21 columns.
    // Call once per frame from the draw thread (lock-free copy of ring then compute).
    std::array<float, N_SLOTS> compute() {
        // Snapshot ring buffers under lock
        float Lsnap[BUF], Rsnap[BUF];
        int   wsnap, sr;
        {
            std::lock_guard lk(mtx_);
            std::copy(L_, L_ + BUF, Lsnap);
            std::copy(R_, R_ + BUF, Rsnap);
            wsnap = w_;
            sr    = sr_;
        }

        // For each slot compute |xcorr(τ)| over WINDOW samples ending at wsnap
        std::array<float, N_SLOTS> out{};
        float peak = 1e-9f;

        for (int s = 0; s < N_SLOTS; ++s) {
            // Lag in samples: centre column (s==10) → 0, left side → positive lag on R,
            // right side → positive lag on L (so R leads L).
            // We use signed lag: negative = L leads R (content panned left in mix),
            //                    positive = R leads L (content panned right).
            int ms_val = (int)DELAY_MS[s];
            int lag    = (int)((float)ms_val * sr / 1000.0f);
            if (s < 10) lag = -lag;   // left side: L leads

            float acc = 0.0f, el = 0.0f, er = 0.0f;
            for (int t = 0; t < WINDOW; ++t) {
                // L sample at time t (reading backwards from write head)
                int li = (wsnap - WINDOW + t + BUF) & (BUF - 1);
                // R sample shifted by lag
                int ri = (li + lag + BUF) & (BUF - 1);
                float lv = Lsnap[li];
                float rv = Rsnap[ri];
                acc += lv * rv;
                el  += lv * lv;
                er  += rv * rv;
            }
            // Normalise: pearson-like, clamp to [0,1]
            float denom = std::sqrt(el * er) + 1e-9f;
            float norm  = std::fabs(acc / denom);
            out[s] = norm;
            if (norm > peak) peak = norm;
        }

        // Scale so peak always fills the display, then smooth
        for (int s = 0; s < N_SLOTS; ++s) {
            float target = out[s] / peak;
            float a = (target > smooth_[s]) ? 0.35f : 0.12f;
            smooth_[s] = smooth_[s] * (1.0f - a) + target * a;
            out[s] = smooth_[s];
        }
        return out;
    }

private:
    static constexpr int BUF    = 8192;   // must be power of 2, covers ~185ms @44100
    static constexpr int WINDOW = 4096;   // correlation window (~93ms), covers max 80ms lag

    float L_[BUF]{}, R_[BUF]{};
    int   w_  = 0;
    int   sr_ = 44100;
    std::array<float, N_SLOTS> smooth_{};
    mutable std::mutex mtx_;
};

constexpr float VocalVisualizer::DELAY_MS[VocalVisualizer::N_SLOTS];
