#ifndef KISS_FFT_H
#define KISS_FFT_H

#include <complex>
#include <vector>
#include <cmath>
#include <cassert>

typedef std::complex<float> kiss_fft_cpx;

struct kiss_fft_state;
typedef struct kiss_fft_state* kiss_fft_cfg;

struct kiss_fft_state {
    int nfft;
    int inverse;
    std::vector<int> factors;
    std::vector<kiss_fft_cpx> twiddles;
};

static void kf_factor(int n, std::vector<int>& facbuf) {
    int p = 4;
    double floor_sqrt = std::floor(std::sqrt((double)n));
    do {
        while (n % p) {
            switch (p) {
                case 4: p = 2; break;
                case 2: p = 3; break;
                default: p += 2; break;
            }
            if (p > floor_sqrt) p = n;
        }
        n /= p;
        facbuf.push_back(p);
        facbuf.push_back(n);
    } while (n > 1);
}

static kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft) {
    kiss_fft_cfg st = new kiss_fft_state();
    st->nfft = nfft;
    st->inverse = inverse_fft;
    st->twiddles.resize(nfft);
    for (int i = 0; i < nfft; ++i) {
        double phase = -2.0 * M_PI * i / nfft;
        if (inverse_fft) phase = -phase;
        st->twiddles[i] = kiss_fft_cpx((float)std::cos(phase), (float)std::sin(phase));
    }
    kf_factor(nfft, st->factors);
    return st;
}

static void kiss_fft_free(kiss_fft_cfg cfg) {
    delete cfg;
}

// Simple DFT fallback (O(N^2)) for non-power-of-2, or recursive FFT
static void kiss_fft_work(kiss_fft_cfg st, const kiss_fft_cpx* fin, kiss_fft_cpx* fout, int in_stride, int factors_idx, int n) {
    if (n == 1) {
        fout[0] = fin[0];
        return;
    }
    // Simple DFT
    int N = n;
    for (int k = 0; k < N; ++k) {
        fout[k] = kiss_fft_cpx(0, 0);
        for (int t = 0; t < N; ++t) {
            double phase = -2.0 * M_PI * k * t / N;
            if (st->inverse) phase = -phase;
            kiss_fft_cpx twiddle((float)std::cos(phase), (float)std::sin(phase));
            fout[k] += fin[t * in_stride] * twiddle;
        }
    }
}

static void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx* fin, kiss_fft_cpx* fout) {
    int N = cfg->nfft;
    // Cooley-Tukey FFT if power of 2, else DFT
    bool pow2 = (N & (N - 1)) == 0;
    if (!pow2 || N <= 1) {
        kiss_fft_work(cfg, fin, fout, 1, 0, N);
        return;
    }
    // In-place Cooley-Tukey
    std::vector<kiss_fft_cpx> buf(fin, fin + N);
    // Bit-reversal permutation
    int bits = 0;
    for (int tmp = N; tmp > 1; tmp >>= 1) bits++;
    for (int i = 0; i < N; ++i) {
        int rev = 0;
        for (int b = 0; b < bits; ++b) {
            rev = (rev << 1) | ((i >> b) & 1);
        }
        if (rev > i) std::swap(buf[i], buf[rev]);
    }
    for (int len = 2; len <= N; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        if (cfg->inverse) ang = -ang;
        kiss_fft_cpx wlen((float)std::cos(ang), (float)std::sin(ang));
        for (int i = 0; i < N; i += len) {
            kiss_fft_cpx w(1, 0);
            for (int j = 0; j < len / 2; ++j) {
                kiss_fft_cpx u = buf[i + j];
                kiss_fft_cpx v = buf[i + j + len / 2] * w;
                buf[i + j] = u + v;
                buf[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    for (int i = 0; i < N; ++i) fout[i] = buf[i];
}

#endif // KISS_FFT_H
