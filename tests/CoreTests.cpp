// =============================================================================
//  Pillow RZ EQ v0.7 — DSP 코어 단독 테스트 (JUCE 불필요)
//
//    빌드:  g++ -std=c++17 -O2 -I../Source CoreTests.cpp -o CoreTests
//    실행:  ./CoreTests            (모든 테스트)
//           ./CoreTests --dump out (그래프용 CSV 저장)
// =============================================================================
#include "dsp/RZ062Core.h"

#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string>

#ifdef _WIN32
 #define NOMINMAX
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>   // 콘솔 UTF-8 출력용
#endif

using namespace pillow::rz;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        ++g_checks;                                               \
        if (! (cond)) {                                           \
            ++g_failures;                                         \
            std::printf ("  FAIL (%s:%d): ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                            \
            std::printf ("\n");                                   \
        }                                                         \
    } while (0)

//------------------------------------------------------------------------------
// 헬퍼
//------------------------------------------------------------------------------
static std::vector<double> logFreqs (double lo, double hi, int n)
{
    std::vector<double> f ((size_t) n);
    for (int i = 0; i < n; ++i)
        f[(size_t) i] = lo * std::pow (hi / lo, (double) i / (n - 1));
    return f;
}

static double dftDb (const std::vector<float>& h, double f, double fs)
{
    const double w = 2.0 * kPi * f / fs;
    const std::complex<double> rot (std::cos (w), -std::sin (w));
    std::complex<double> ph (1.0, 0.0), acc (0.0, 0.0);
    for (float v : h) { acc += (double) v * ph; ph *= rot; }
    return 20.0 * std::log10 (std::max (std::abs (acc), 1.0e-12));
}

// 스테레오 엔진에 작은 임펄스(amp)를 넣고 응답 측정. inL/inR 로 어느 채널에 넣을지 지정
static std::vector<float> impulseResponse (const Params& p, double fs, int len, float inL, float inR,
                                           int outChannel, float amp = 1.0e-3f)
{
    Engine e;
    e.prepare (fs, 2, 4096);
    e.setParams (p);
    e.reset();
    std::vector<float> L ((size_t) len, 0.0f), R ((size_t) len, 0.0f);
    L[0] = inL * amp;
    R[0] = inR * amp;
    float* ptr[2] = { L.data(), R.data() };
    e.process (ptr, 2, len);
    auto& out = outChannel == 0 ? L : R;
    for (auto& v : out) v /= amp;
    return out;
}

static void harmonics (const std::vector<float>& y, double fs, double f0, int start, int len,
                       double* amps, int nh, double* dcOut)
{
    double dc = 0.0;
    for (int i = start; i < start + len; ++i) dc += y[(size_t) i];
    *dcOut = dc / len;
    for (int h = 1; h <= nh; ++h)
    {
        std::complex<double> acc (0.0, 0.0);
        const double w = 2.0 * kPi * f0 * h / fs;
        for (int i = start; i < start + len; ++i)
            acc += (double) y[(size_t) i] * std::complex<double> (std::cos (w * i), -std::sin (w * i));
        amps[h - 1] = 2.0 * std::abs (acc) / len;
    }
}

static double thdPercent (const double* h, int nh)
{
    double s = 0.0;
    for (int k = 1; k < nh; ++k) s += h[k] * h[k];
    return 100.0 * std::sqrt (s) / h[0];
}

static std::vector<float> sine (double fs, double f0, double level, int len)
{
    std::vector<float> x ((size_t) len);
    for (int i = 0; i < len; ++i)
        x[(size_t) i] = (float) (level * std::sin (2.0 * kPi * f0 * i / fs));
    return x;
}

static float logRandom (std::mt19937& rng, double lo, double hi)
{
    std::uniform_real_distribution<double> u (0.0, 1.0);
    return (float) (lo * std::pow (hi / lo, u (rng)));
}

static BandParams makeBand (BandType t, double f, double g, double q = 1.0, Placement pl = Placement::Stereo,
                            int slope = kDefaultSlope)
{
    BandParams b;
    b.used = true;  b.on = true;  b.type = t;  b.place = pl;  b.slope = slope;
    b.freq = (float) f;  b.gain = (float) g;  b.q = (float) q;
    return b;
}

// 무작위 밴드. typeMask: 허용할 모양 (비트 = BandType)
static BandParams randomBand (std::mt19937& rng, unsigned typeMask = 0x7Fu, bool anyPlacement = false,
                              double maxGain = 18.0)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    BandType t;
    do { t = (BandType) (int) (u (rng) * (float) kNumBandTypes * 0.9999f); } while (! (typeMask & (1u << (int) t)));
    const auto pl = anyPlacement ? (Placement) (int) (u (rng) * (float) kNumPlacements * 0.9999f) : Placement::Stereo;
    // 함수 인자의 계산 순서는 컴파일러마다 다름 (GCC · MSVC는 오른쪽부터, Clang은 왼쪽부터) -> 뽑는 순서를 고정
    const int    slope = (int) (u (rng) * 7.999f);
    const float  q     = logRandom (rng, 0.2, 8.0);
    const double gain  = -maxGain + 2.0 * maxGain * u (rng);
    const float  freq  = logRandom (rng, 30.0, 16000.0);
    return makeBand (t, freq, gain, q, pl, slope);
}

// 모든 기능을 무작위로 (스트레스·블록 크기 테스트용)
static Params randomParams (std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    Params p;
    p.eqOn     = u (rng) < 0.9f;
    p.driveOn  = u (rng) < 0.6f;
    p.drive    = u (rng) * kDriveMaxDb;
    p.focus    = -1.0f + 2.0f * u (rng);
    p.mix      = u (rng) < 0.3f ? u (rng) : 1.0f;
    p.xfLow    = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.xfMid    = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.xfHigh   = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.outputDb = -6.0f + 12.0f * u (rng);
    p.xfAll    = u (rng) < 0.6f;
    for (auto& b : p.bands)
        if (u (rng) < 0.35f)
        {
            b = randomBand (rng, 0x7Fu, true, 12.0);
            b.on = u (rng) < 0.85f;
            b.tube     = u (rng) < 0.3f;
            b.drive    = 48.0f * u (rng);
            b.focus    = -1.0f + 2.0f * u (rng);
            b.tubeMix  = u (rng) < 0.3f ? u (rng) : 1.0f;
            b.xfmr     = u (rng) < 0.3f;
            b.xfAmount = u (rng);
        }
    return p;
}

//------------------------------------------------------------------------------
static void testNeutral()
{
    std::printf ("[1] 밴드 없음 / 게인 0 밴드 / 꺼진 밴드 -> 투명\n");
    std::mt19937 rng (1);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> L0 (8192), R0 (8192);
    for (size_t i = 0; i < L0.size(); ++i) { L0[i] = d (rng); R0[i] = d (rng); }

    auto run = [&] (const Params& p, int nch, double fs)
    {
        Engine e;
        e.prepare (fs, nch, 2048);
        e.setParams (p);
        e.reset();
        auto L = L0, R = R0;
        float* ptr[2] = { L.data(), R.data() };
        e.process (ptr, nch, (int) L.size());
        double err = 0.0;
        for (size_t i = 0; i < L.size(); ++i)
            err = std::max ({ err, (double) std::abs (L[i] - L0[i]), nch > 1 ? (double) std::abs (R[i] - R0[i]) : 0.0 });
        return err;
    };

    for (double fs : { 48000.0, 192000.0 })
    {
        Params none;
        CHECK (run (none, 2, fs) == 0.0 && run (none, 1, fs) == 0.0, "no bands must be bit-exact");

        Params zero;   // 게인 0 벨·셸프·틸트 (Q 여러 가지, L/R/스테레오) -> 비트 단위 동일
        zero.bands[0] = makeBand (BandType::Bell, 1000, 0, 7.0);
        zero.bands[1] = makeBand (BandType::LowShelf, 120, 0, 2.5, Placement::Left);
        zero.bands[2] = makeBand (BandType::HighShelf, 6000, 0, 0.4, Placement::Right);
        zero.bands[3] = makeBand (BandType::Tilt, 650, 0);
        CHECK (run (zero, 2, fs) == 0.0 && run (zero, 1, fs) == 0.0, "zero-gain bands must be bit-exact");

        Params ms = zero;   // Mid/Side 밴드가 있으면 M/S 변환 반올림만
        ms.bands[4] = makeBand (BandType::Bell, 3000, 0, 1.0, Placement::Mid);
        ms.bands[5] = makeBand (BandType::LowShelf, 200, 0, 1.0, Placement::Side);
        CHECK (run (ms, 2, fs) < 1.0e-6, "M/S zero-gain error %.3g", run (ms, 2, fs));

        Params off;   // 꺼진 밴드 · EQ off
        off.bands[0] = makeBand (BandType::LowCut, 500, 0, 1.0, Placement::Stereo, 5);
        off.bands[0].on = false;
        off.bands[1] = makeBand (BandType::Bell, 2000, 12, 2.0);
        off.eqOn = false;
        CHECK (run (off, 2, fs) == 0.0, "disabled bands / EQ off must be bit-exact");
    }
}

//------------------------------------------------------------------------------
static ResponseModel single (const BandParams& b, double fs)
{
    Params p;
    p.bands[0] = b;
    return ResponseModel (p, fs);
}

static void testBandShapes()
{
    std::printf ("[2] 밴드 모양 (HQ 레이트 = 거의 아날로그)\n");
    const double fs = processingRateFor (48000.0);

    {   // 벨: 중심 이득 정확
        double worst = 0.0;
        for (double f : { 40.0, 500.0, 3000.0, 12000.0 })
            for (double g : { -18.0, -6.0, 6.0, 18.0 })
                for (double q : { 0.3, 1.0, 8.0 })
                    worst = std::max (worst, std::abs (single (makeBand (BandType::Bell, f, g, q), fs).magnitudeDb (f) - g));
        std::printf ("    벨: 중심 이득 오차 최대 %.4f dB\n", worst);
        CHECK (worst < 0.01, "bell center %.4f", worst);
    }
    {   // 셸프: 끝 = 게인, 중심 주파수 = 게인의 절반, Q 1은 출렁임 없음
        double wEnd = 0.0, wMid = 0.0, over = 0.0;
        for (double f : { 100.0, 1000.0, 6000.0 })
            for (double g : { -12.0, 12.0 })
            {
                const auto ls = single (makeBand (BandType::LowShelf, f, g, 1.0), fs);
                const auto hs = single (makeBand (BandType::HighShelf, f, g, 1.0), fs);
                wEnd = std::max ({ wEnd, std::abs (ls.magnitudeDb (f / 20.0) - g), std::abs (hs.magnitudeDb (std::min (f * 10.0, 0.4 * fs)) - g) });
                wMid = std::max ({ wMid, std::abs (ls.magnitudeDb (f) - g / 2), std::abs (hs.magnitudeDb (f) - g / 2) });
                for (double ff : logFreqs (10.0, 40000.0, 400))
                    for (const auto* m : { &ls, &hs })
                    {
                        const double v = m->magnitudeDb (ff);
                        over = std::max ({ over, v - std::max (g, 0.0), std::min (g, 0.0) - v });   // 0 ~ g 범위 밖으로 넘침
                    }
            }
        std::printf ("    셸프: 끝 이득 오차 %.3f dB, 중심 = 절반 오차 %.3f dB, Q 1 출렁임 %.3f dB\n", wEnd, wMid, over);
        CHECK (wEnd < 0.1 && wMid < 0.05 && over < 0.02, "shelf shape end %.3f mid %.3f over %.3f", wEnd, wMid, over);
    }
    {   // 컷: Q 1 = 버터워스. 차단 주파수 -3.01 dB, 2옥타브 떨어진 곳 = 아날로그 버터워스
        double wFc = 0.0, wFar = 0.0;
        for (int s = 0; s < (int) kCutSlopes.size(); ++s)
            for (auto t : { BandType::LowCut, BandType::HighCut })
            {
                const auto m = single (makeBand (t, 1000, 0, 1.0, Placement::Stereo, s), fs);
                const int order = kCutSlopes[(size_t) s] / 6;
                const double farHz = t == BandType::LowCut ? 250.0 : 4000.0;   // ("far"는 MSVC에서 매크로)
                const double expected = -10.0 * std::log10 (1.0 + std::pow (4.0, 2.0 * order));
                wFc  = std::max (wFc, std::abs (m.magnitudeDb (1000.0) + 3.0103));
                wFar = std::max (wFar, std::abs (m.magnitudeDb (farHz) - expected));
            }
        const auto res = single (makeBand (BandType::LowCut, 1000, 0, 2.0), fs);
        double peak = -100.0;
        for (double f : logFreqs (200.0, 20000.0, 400)) peak = std::max (peak, res.magnitudeDb (f));
        std::printf ("    컷 6~96 dB/oct: 차단점 오차 %.4f dB, 2옥타브 지점 오차 %.3f dB, Q 2 공진 +%.2f dB\n", wFc, wFar, peak);
        CHECK (wFc < 0.02 && wFar < 0.3, "cut shape fc %.4f far %.3f", wFc, wFar);
        CHECK (peak > 3.0, "resonant cut expected (%.2f)", peak);
    }
    {   // 노치 / 틸트
        const auto n = single (makeBand (BandType::Notch, 2000, 0, 8.0), fs);
        const double depth = n.magnitudeDb (2000.0), side = n.magnitudeDb (4000.0);
        const auto t = single (makeBand (BandType::Tilt, 650, 4.0), fs);
        std::printf ("    노치 Q 8: 중심 %.1f dB, 1옥타브 위 %.2f dB / 틸트 +4: 축 %.3f, 20 Hz %.2f, 20 kHz %.2f dB\n",
                     depth, side, t.magnitudeDb (650.0), t.magnitudeDb (20.0), t.magnitudeDb (20000.0));
        CHECK (depth < -60.0 && std::abs (side) < 0.1, "notch depth %.1f side %.2f", depth, side);
        CHECK (std::abs (t.magnitudeDb (650.0)) < 0.01 && t.magnitudeDb (20.0) < -3.6 && t.magnitudeDb (20000.0) > 3.6, "tilt shape");
    }
}

//------------------------------------------------------------------------------
static void testAnalyticMatchesProcessing()
{
    std::printf ("[3] GUI 해석 곡선 == 실제 처리 응답 (모든 모양, 트랜스포머 셸프·Mix)\n");
    std::mt19937 rng (42);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    double worst = 0.0;
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int t = 0; t < 10; ++t)
        {
            Params p;
            const int nb = 1 + (int) (u (rng) * 5.99f);
            for (int i = 0; i < nb; ++i)
                p.bands[(size_t) (i * 2 + (t % 2))] = randomBand (rng);   // 빈 칸 섞어서
            p.xfHigh = u (rng) < 0.5f ? u (rng) : 0.0f;
            p.xfAll  = u (rng) < 0.7f;                                      // 트랜스포머(ALL) 셸프 켜기/끄기
            p.mix    = u (rng) < 0.4f ? 0.3f + 0.7f * u (rng) : 1.0f;
            const auto h = impulseResponse (p, fs, (int) (fs * 0.6), 1.0f, 1.0f, 0);
            const ResponseModel m (p, fs);
            for (double f : logFreqs (20.0, 20000.0, 50))
            {
                const double ref = m.magnitudeDb (f);
                if (ref > -40.0)
                    worst = std::max (worst, std::abs (dftDb (h, f, fs) - ref));
            }
        }
    std::printf ("    최대 오차 %.5f dB\n", worst);
    CHECK (worst < 0.01, "analytic vs measured worst %.5f dB", worst);
}

//------------------------------------------------------------------------------
static void testPlacements()
{
    std::printf ("[4] 적용 채널 (스테레오 · L · R · Mid · Side, 모노)\n");
    const double fs = 96000.0;
    const int len = (int) (fs * 0.5);
    auto with = [] (Placement pl)
    {
        Params p;
        p.bands[0] = makeBand (BandType::Bell, 1000, 9.0, 2.0, pl);
        p.bands[1] = makeBand (BandType::HighShelf, 6000, -6.0, 1.0);   // 스테레오 밴드도 같이
        return p;
    };
    double worst = 0.0, leak = 0.0;
    auto cmp = [&] (const std::vector<float>& h, const ResponseModel& m, Placement view)
    {
        for (double f : logFreqs (20.0, 20000.0, 40))
            worst = std::max (worst, std::abs (dftDb (h, f, fs) - m.magnitudeDb (f, view)));
    };
    auto peakAbs = [] (const std::vector<float>& h) { double a = 0.0; for (float v : h) a = std::max (a, (double) std::abs (v)); return a; };

    for (auto pl : { Placement::Left, Placement::Right })
    {
        const auto p = with (pl);
        const ResponseModel m (p, fs);
        const bool left = pl == Placement::Left;
        cmp (impulseResponse (p, fs, len, left ? 1.0f : 0.0f, left ? 0.0f : 1.0f, left ? 0 : 1), m, pl);              // 그 채널
        cmp (impulseResponse (p, fs, len, left ? 0.0f : 1.0f, left ? 1.0f : 0.0f, left ? 1 : 0), m, Placement::Stereo); // 다른 채널
        leak = std::max (leak, peakAbs (impulseResponse (p, fs, len, left ? 1.0f : 0.0f, left ? 0.0f : 1.0f, left ? 1 : 0)));
    }
    for (auto pl : { Placement::Mid, Placement::Side })
    {
        const auto p = with (pl);
        const ResponseModel m (p, fs);
        const bool mid = pl == Placement::Mid;
        // L = R 임펄스 = Mid만, L = -R 임펄스 = Side만
        cmp (impulseResponse (p, fs, len, 1.0f, mid ? 1.0f : -1.0f, 0), m, pl);
        cmp (impulseResponse (p, fs, len, 1.0f, mid ? -1.0f : 1.0f, 0), m, Placement::Stereo);
    }
    {   // 모노: Mid 밴드는 적용, Side 밴드는 없음
        for (auto pl : { Placement::Mid, Placement::Side })
        {
            const auto p = with (pl);
            const ResponseModel m (p, fs);
            Engine e;
            e.prepare (fs, 1, 4096);
            e.setParams (p);
            e.reset();
            std::vector<float> h ((size_t) len, 0.0f);
            h[0] = 1.0e-3f;
            float* ptr[1] = { h.data() };
            e.process (ptr, 1, len);
            for (auto& v : h) v /= 1.0e-3f;
            cmp (h, m, pl == Placement::Mid ? Placement::Mid : Placement::Stereo);
        }
    }
    std::printf ("    채널별 응답 오차 최대 %.5f dB, 다른 채널로 새는 양 %.1e\n", worst, leak);
    CHECK (worst < 0.01, "placement response error %.5f", worst);
    CHECK (leak < 1.0e-9, "L/R band leaks into other channel %.2e", leak);
}

//------------------------------------------------------------------------------
static double halfGainBandwidthOct (const ResponseModel& m, double fc)
{
    const double half = 0.5 * m.magnitudeDb (fc);
    auto edge = [&] (double dir)
    {
        double f = fc;
        for (int i = 0; i < 20000; ++i)
        {
            f *= std::pow (2.0, dir * 0.001);
            if (m.magnitudeDb (f) <= half) return f;
        }
        return f;
    };
    return std::log2 (edge (+1.0) / edge (-1.0));
}

static void testQBehaviour()
{
    std::printf ("[5] Q 동작\n");
    const double fs = processingRateFor (48000.0);
    double prevBw = 1e9;
    for (double q : { 0.3, 0.6, 1.0, 2.0, 4.0, 8.0, 16.0 })
    {
        const double bw = halfGainBandwidthOct (single (makeBand (BandType::Bell, 2800, 6.0, q), fs), 2800.0);
        std::printf ("    벨 2.8 kHz +6 dB, Q %4.1f -> 대역폭 %.2f oct\n", q, bw);
        CHECK (bw < prevBw, "bandwidth must shrink as Q grows (Q %.1f)", q);
        prevBw = bw;
    }
    double prevSlope = 0.0;
    for (double q : { 0.4, 0.7, 1.0, 1.5, 3.0 })
    {
        const auto m = single (makeBand (BandType::LowShelf, 160, 12.0, q), fs);
        double maxSlope = 0.0, maxGain = -100.0, minGain = 100.0;
        for (double f : logFreqs (10.0, 20000.0, 2000))
        {
            const double g = m.magnitudeDb (f);
            maxSlope = std::max (maxSlope, std::abs (m.magnitudeDb (f * 1.01) - g) / std::log2 (1.01));
            maxGain  = std::max (maxGain, g);
            minGain  = std::min (minGain, g);
        }
        std::printf ("    로우 셸프 +12, Q %.1f -> 최대 기울기 %.2f dB/oct, 최대 %.2f / 최소 %.2f dB\n", q, maxSlope, maxGain, minGain);
        CHECK (maxSlope > prevSlope, "shelf slope must grow with Q (Q %.1f)", q);
        if (q <= 1.0) CHECK (maxGain < 12.05 && minGain > -0.05, "no overshoot expected at Q %.1f", q);
        if (q >= 1.5) CHECK (maxGain > 12.3 && minGain < -0.3, "resonant shelf expected at Q %.1f", q);
        prevSlope = maxSlope;
    }
}

//------------------------------------------------------------------------------
static void testSampleRateConsistency()
{
    std::printf ("[6] 세션 샘플레이트 간 일관성 (20 Hz ~ 20 kHz)\n");
    const unsigned mask = (1u << (int) BandType::Bell) | (1u << (int) BandType::LowShelf) | (1u << (int) BandType::HighShelf)
                        | (1u << (int) BandType::Tilt) | (1u << (int) BandType::LowCut);

    // maxHz / qMax: 무작위 범위. 반환: HQ / ZERO LATENCY 최대 차이 (768 kHz 기준과 비교)
    auto run = [&] (unsigned seed, double maxHz, double qMax, double maxGain)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        double wHq = 0.0, wZero = 0.0;
        for (int t = 0; t < 60; ++t)
        {
            Params p;
            const int nb = 1 + (int) (u (rng) * 3.99f);
            for (int i = 0; i < nb; ++i)
            {
                auto b = randomBand (rng, mask, false, maxGain);
                b.freq = logRandom (rng, 30.0, maxHz);
                b.q    = logRandom (rng, 0.3, qMax);
                if (b.type == BandType::Tilt || b.type == BandType::LowCut)
                    b.freq = std::min (b.freq, 1500.0f);   // 1차 틸트·컷은 고역에 두면 쌍선형 특성이 남음 (아래 따로 확인)
                p.bands[(size_t) i] = b;
            }
            p.xfHigh = u (rng);
            const ResponseModel ref (p, 768000.0);
            for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                const ResponseModel hq (p, processingRateFor (sr)), zero (p, sr);
                for (double f : logFreqs (20.0, 20000.0, 100))
                {
                    const double r = ref.magnitudeDb (f);
                    if (r < -40.0) continue;
                    wHq   = std::max (wHq,   std::abs (hq.magnitudeDb (f) - r));
                    wZero = std::max (wZero, std::abs (zero.magnitudeDb (f) - r));
                }
            }
        }
        return std::pair<double, double> { wHq, wZero };
    };

    const auto [worstHq, worstZero] = run (7, 10000.0, 4.0, 15.0);     // 보통 쓰는 범위
    std::printf ("    무작위 60개 (30 Hz~10 kHz, Q 0.3~4, +-15 dB) vs 아날로그 기준 최대 차이: HQ %.3f dB / ZERO LATENCY %.3f dB\n", worstHq, worstZero);
    CHECK (worstHq < 0.15, "sample-rate deviation (HQ) %.3f dB", worstHq);
    CHECK (worstZero < 0.6, "sample-rate deviation (zero latency) %.3f dB", worstZero);

    const auto [xHq, xZero] = run (8, 16000.0, 8.0, 18.0);             // 극단 (참고용: 나이퀴스트 근처 한계)
    std::printf ("    극단 범위 (~16 kHz, Q ~8, +-18 dB): HQ %.3f dB / ZERO LATENCY %.3f dB (참고)\n", xHq, xZero);
    CHECK (xHq < 0.6, "sample-rate deviation (HQ, extreme) %.3f dB", xHq);

    // 하이 컷 · 노치는 세션 레이트에서도 차단점 / 중심이 정확 (나이퀴스트 근처 기울기는 쌍선형)
    double wCut = 0.0, wNotch = -1.0e9;
    for (double sr : { 44100.0, 48000.0 })
        for (double f : { 1000.0, 8000.0, 16000.0 })
        {
            wCut   = std::max (wCut, std::abs (single (makeBand (BandType::HighCut, f, 0, 1.0, Placement::Stereo, 3), sr).magnitudeDb (f) + 3.0103));
            wNotch = std::max (wNotch, single (makeBand (BandType::Notch, f, 0, 4.0), sr).magnitudeDb (f));
        }
    std::printf ("    44.1/48k 하이 컷 차단점 오차 %.4f dB, 노치 중심 %.1f dB\n", wCut, wNotch);
    CHECK (wCut < 0.01 && wNotch < -60.0, "session-rate cut/notch %.4f %.1f", wCut, wNotch);

    // 틸트 (1차): 축 10 Hz ~ 30 kHz, +-24 dB, 세션 레이트 44.1 ~ 192k 전체에서 아날로그 곡선과
    double wTilt = 0.0, wTiltNormal = 0.0, tiltAtPivot = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double pivot = 10.0; pivot <= 30000.0; pivot *= 1.25)
            for (double g = -24.0; g <= 24.0; g += 3.0)
            {
                const ResponseModel m = single (makeBand (BandType::Tilt, pivot, g, 1.0), sr);
                for (double f : logFreqs (20.0, std::min (20000.0, 0.499 * sr), 200))
                {
                    const double d = std::abs (m.magnitudeDb (f) - 10.0 * std::log10 (tiltAnalogMag2 (pivot, g, f)));
                    wTilt = std::max (wTilt, d);
                    if (pivot <= 3000.0 && std::abs (g) <= 15.0)
                        wTiltNormal = std::max (wTiltNormal, d);
                }
                if (pivot < 0.25 * sr)
                    tiltAtPivot = std::max (tiltAtPivot, std::abs (m.magnitudeDb (pivot)));
            }
    std::printf ("    틸트 세션 레이트: 아날로그 대비 최대 %.3f dB (축 3 kHz 이하 +-15 dB: %.3f dB), 축에서 %.4f dB\n",
                 wTilt, wTiltNormal, tiltAtPivot);
    CHECK (wTilt < 1.0 && wTiltNormal < 0.45 && tiltAtPivot < 1e-6, "session-rate tilt %.3f / %.3f / %.2g", wTilt, wTiltNormal, tiltAtPivot);
}

//------------------------------------------------------------------------------
// 300 Hz 사인 -> 6 kHz 고역통과 후 최대값으로 클릭 측정
struct ClickResult { double steady, transition; };

static ClickResult measureClick (const Params& before, const Params& after, double smoothingMs)
{
    const double fs = 48000.0;
    Engine e;
    e.prepare (fs, 2, 1024);
    e.setSmoothingMs (smoothingMs);
    e.setParams (before);
    e.reset();

    SvfCoeffs hp = makeHighpass (6000.0, 0.707, fs);
    SvfState hsL, hsR;

    const int total = (int) fs * 2, switchAt = (int) fs;
    auto L = sine (fs, 300.0, 0.5, total);
    auto R = sine (fs, 300.0, 0.35, total);

    double steady = 0.0, trans = 0.0;
    const int block = 64;
    for (int start = 0; start < total; start += block)
    {
        if (start == switchAt) e.setParams (after);
        float* ptr[2] = { L.data() + start, R.data() + start };
        e.process (ptr, 2, block);
        for (int i = start; i < start + block; ++i)
        {
            const double y = std::max (std::abs (svfTick (hp, hsL, L[(size_t) i])),
                                       std::abs (svfTick (hp, hsR, R[(size_t) i])));
            if (i > switchAt / 2 && i < switchAt) steady = std::max (steady, y);
            else if (i >= switchAt && i < switchAt + (int) (0.3 * fs)) trans = std::max (trans, y);
            else if (i > switchAt + (int) (0.6 * fs)) steady = std::max (steady, y);
        }
    }
    return { steady, trans };
}

static void testClicks()
{
    std::printf ("[7] 바꿀 때 클릭 (스무딩·페이드 on vs off)\n");
    struct Case { const char* name; Params a, b; };
    std::vector<Case> cases;
    auto add = [&] (const char* n, Params a, Params b) { cases.push_back ({ n, a, b }); };
    const auto bell = makeBand (BandType::Bell, 1000, 12, 2.0);
    {
        Params a; a.bands[0] = bell; Params b = a; b.bands[0].gain = -12;
        add ("Bell +12 -> -12", a, b);
    }
    {
        // (측정용 300 Hz 사인을 가로지르지 않는 범위: 지나가면 그 사인 자체가 +9 dB 커지는 게 정상이라)
        Params a; a.bands[0] = makeBand (BandType::Bell, 1000, 9, 6.0); Params b = a; b.bands[0].freq = 12000;
        add ("Bell 1 kHz -> 12 kHz (Q 6)", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::LowShelf, 200, 9, 0.4); Params b = a; b.bands[0].q = 3.0f;
        add ("Low shelf Q 0.4 -> 3", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 9, 1.0); Params b = a; b.bands[0].type = BandType::HighShelf;
        add ("Shape bell -> high shelf", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 9, 1.0); Params b = a; b.bands[0].place = Placement::Mid;
        add ("Placement stereo -> mid", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 9, 1.0); Params b = a; b.bands[0].on = false;
        add ("Band on -> off", a, b);
    }
    {
        Params a; Params b = a; b.bands[3] = makeBand (BandType::Bell, 300, 12, 1.0);
        add ("Add band (+12 at 300 Hz)", a, b);
    }
    {
        Params a; a.bands[3] = makeBand (BandType::Bell, 300, 12, 1.0); Params b = a; b.bands[3].used = false;
        add ("Delete band", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::LowCut, 200, 0, 1.0, Placement::Stereo, 1); Params b = a; b.bands[0].slope = 5;
        add ("Low cut 12 -> 48 dB/oct", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 9, 1.0); a.eqOn = false; Params b = a; b.eqOn = true;
        add ("EQ off -> on", a, b);
    }
    {
        Params a; a.drive = 18; Params b = a; b.driveOn = true;
        add ("Drive off -> on (18 dB)", a, b);
    }
    {
        Params a; a.bands[0] = bell; Params b = a; b.mix = 0.3f;
        add ("Mix 100 -> 30 %", a, b);
    }
    {
        Params a; Params b = a; b.xfLow = 1; b.xfMid = 1; b.xfHigh = 1;
        add ("Transformer 0 -> 100 %", a, b);
    }
    {
        Params a; Params b = a; b.outputDb = -12;
        add ("Output 0 -> -12 dB", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 0, 1.0); a.bands[0].drive = 30; Params b = a; b.bands[0].tube = true;
        add ("Band tube on (drive 30)", a, b);
    }
    {
        Params a; a.drive = 30; a.driveOn = true; a.bands[0] = makeBand (BandType::Bell, 300, 0, 1.0); a.bands[0].tube = true;
        Params b = a; b.driveOn = false;
        add ("Tube ALL off, band tube stays", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::LowShelf, 400, 6, 1.0); a.bands[0].tube = true; a.bands[0].drive = 30;
        Params b = a; b.bands[0].used = false;
        add ("Delete band with tube", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 6, 1.0); a.bands[0].tube = true; a.bands[0].drive = 30;
        Params b = a; b.bands[0].type = BandType::LowShelf;
        add ("Tube band shape change", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 3, 1.0); a.bands[0].tube = true; a.bands[0].drive = 12;
        Params b = a; b.bands[0].drive = 36;
        add ("Band drive 12 -> 36 dB", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 3, 1.0); a.bands[0].tube = true; a.bands[0].drive = 30; a.bands[0].tubeMix = 0;
        Params b = a; b.bands[0].tubeMix = 1;
        add ("Band tube mix 0 -> 100 %", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::Bell, 300, 0, 1.0); a.bands[0].tube = true; a.bands[0].focus = -1;
        a.bands[0].drive = 30; Params b = a; b.bands[0].focus = 1;
        add ("Band focus odd -> even", a, b);
    }
    {
        Params a; a.bands[0] = makeBand (BandType::LowShelf, 400, 0, 1.0); a.bands[0].xfAmount = 1;
        Params b = a; b.bands[0].xfmr = true;
        add ("Band transformer on (100 %)", a, b);
    }
    for (auto& c : cases)
    {
        const auto on  = measureClick (c.a, c.b, 15.0);
        const auto off = measureClick (c.a, c.b, 0.0);
        const double rOn = on.transition / on.steady, rOff = off.transition / off.steady;
        std::printf ("    %-30s 스무딩 on: x%.2f   off: x%.2f\n", c.name, rOn, rOff);
        CHECK (rOn < 3.0, "%s: click ratio %.2f", c.name, rOn);
    }
}

//------------------------------------------------------------------------------
static void testStress()
{
    std::printf ("[8] 무작위 자동화 스트레스 (모양·채널·추가/삭제 포함, NaN/발산 없음)\n");
    for (double fs : { 44100.0, 192000.0 })
        for (int nch : { 1, 2 })
        {
            Engine e;
            e.prepare (fs, nch, 256);
            std::mt19937 rng (99);
            std::normal_distribution<float> n (0.0f, 0.3f);
            const int total = (int) (fs * 4.0), block = 64;
            std::vector<float> L ((size_t) block), R ((size_t) block);
            double peak = 0.0;
            bool finite = true;
            for (int start = 0; start < total; start += block)
            {
                e.setParams (randomParams (rng));
                for (int i = 0; i < block; ++i) { L[(size_t) i] = n (rng); R[(size_t) i] = n (rng); }
                float* ptr[2] = { L.data(), R.data() };
                e.process (ptr, nch, block);
                for (int i = 0; i < block; ++i)
                {
                    finite = finite && std::isfinite (L[(size_t) i]) && std::isfinite (R[(size_t) i]);
                    peak = std::max ({ peak, (double) std::abs (L[(size_t) i]), (double) std::abs (R[(size_t) i]) });
                }
            }
            std::printf ("    fs %.0f, %d ch: 최대 피크 %.2f\n", fs, nch, peak);
            CHECK (finite, "non-finite output at %.0f/%d", fs, nch);
            CHECK (peak < 1000.0, "output diverged at %.0f/%d (%.2f)", fs, nch, peak);
        }
}

//------------------------------------------------------------------------------
static void testBlockSizeIndependence()
{
    std::printf ("[9] 블록 크기와 무관하게 같은 결과 (밴드 전환·튜브·트랜스포머 포함)\n");
    const double fs = 96000.0;
    const int total = 2048 * 24;
    std::mt19937 rng (3);
    std::normal_distribution<float> n (0.0f, 0.2f);
    std::vector<float> inL ((size_t) total), inR ((size_t) total);
    for (int i = 0; i < total; ++i) { inL[(size_t) i] = n (rng); inR[(size_t) i] = n (rng); }
    std::vector<Params> plist;
    for (int i = 0; i < total / 2048; ++i) plist.push_back (randomParams (rng));

    auto run = [&] (int block)
    {
        Engine e;
        e.prepare (fs, 2, 600);
        e.setParams (plist[0]);
        e.reset();
        auto L = inL, R = inR;
        for (int start = 0; start < total; start += block)
        {
            if (start % 2048 == 0) e.setParams (plist[(size_t) (start / 2048)]);
            float* ptr[2] = { L.data() + start, R.data() + start };
            e.process (ptr, 2, block);
        }
        L.insert (L.end(), R.begin(), R.end());
        return L;
    };
    const auto ref = run (2048);
    for (int b : { 1, 16, 64, 512 })
        CHECK (run (b) == ref, "block size %d differs from 2048", b);
}

//------------------------------------------------------------------------------
static void testEf804sCurve()
{
    std::printf ("[10] EF804S 모델\n");
    const auto& c = ef804sCurve();
    std::printf ("    RZ062 단: Rk %.0f ohm, 유휴 플레이트 %.1f V, 이득 %.2f배\n",
                 c.cathodeResistor(), c.idlePlateVoltage(), c.stageGain());
    CHECK (std::abs (c.cathodeResistor() - 8432.0) < 200.0, "Rk %.0f", c.cathodeResistor());
    CHECK (std::abs (c.idlePlateVoltage() - 180.0) < 1.5, "Vp0 %.1f", c.idlePlateVoltage());
    CHECK (std::abs (c.stageGain() - 14.39) < 0.3, "gain %.2f", c.stageGain());

    // 데이터시트: 3극관 결선 RC 증폭 250 V, 100 kOhm 부하, 약 1 mA -> 이득 약 31
    auto ia = [] (double vg, double rl) {
        double lo = 0.0, hi = 250.0 / rl;
        for (int it = 0; it < 64; ++it)
        {
            const double i = 0.5 * (lo + hi);
            (i - Ef804sCurve::plateCurrent (250.0 - i * rl, vg) > 0.0 ? hi : lo) = i;
        }
        return 0.5 * (lo + hi);
    };
    double lo = -15.0, hi = 0.0;
    for (int it = 0; it < 64; ++it) { const double b = 0.5 * (lo + hi); (ia (b, 100e3) > 1e-3 ? hi : lo) = b; }
    const double bias = 0.5 * (lo + hi), d = 1e-3;
    const double rcGain = (ia (bias + d, 100e3) - ia (bias - d, 100e3)) * 100e3 / (2.0 * d);
    std::printf ("    데이터시트 조건(250 V, 100 kOhm, 1 mA) 이득 %.1f (데이터시트 31)\n", rcGain);
    CHECK (std::abs (rcGain - 31.0) < 0.7, "datasheet RC gain %.1f", rcGain);

    // 룩업 테이블 정확도 (회로를 직접 푼 값과 비교)
    double worstAbs = 0.0, worstRel = 0.0;
    std::mt19937 rng (5);
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    for (int t = 0; t < 2000; ++t)
    {
        const double v = (t % 2 == 0) ? 200.0 * u (rng) : 1.0e-3 * std::pow (10.0, 3.0 * std::abs (u (rng))) * (u (rng) < 0 ? -1 : 1);
        const double direct = (c.idlePlateVoltage() - (Ef804sCurve::kBplus - Ef804sCurve::stageCurrent (v, c.cathodeResistor()) * Ef804sCurve::kRl)) / c.stageGain();
        const double err = std::abs (c (v) - direct);
        worstAbs = std::max (worstAbs, err);
        if (std::abs (v) < 1.0) worstRel = std::max (worstRel, err / std::abs (direct));
    }
    std::printf ("    룩업 오차: 최대 %.2e (절대), 작은 신호 %.2e (상대)\n", worstAbs, worstRel);
    CHECK (worstAbs < 1e-4, "LUT abs error %.2e", worstAbs);
    CHECK (worstRel < 1e-6, "LUT small-signal rel error %.2e", worstRel);

    // 오디오 루프용 홀수·짝수 표 (evalParts) == 원래 조회 (evalPair) — 표 안 · 밖 · 음수 · 아주 작은 값
    {
        double worst = 0.0;
        for (int t = 0; t < 20000; ++t)
        {
            const double mag = std::pow (10.0, -6.0 + 9.5 * (double) t / 20000.0);   // 1e-6 .. 3e3 V
            for (double v : { mag, -mag })
            {
                double dp, dn, pp, pn, dO, dE, pO, pE;
                c.evalPair (v, dp, dn, pp, pn);
                c.evalParts (v, dO, dE, pO, pE);
                const double scaleD = 1e-12 * (1.0 + std::abs (dp) + std::abs (dn));
                const double scaleP = 1e-12 * (1.0 + std::abs (pp) + std::abs (pn));
                worst = std::max ({ worst, std::abs (dO - 0.5 * (dp - dn)) / scaleD, std::abs (dE - 0.5 * (dp + dn)) / scaleD,
                                    std::abs (pO - 0.5 * (pp + pn)) / scaleP, std::abs (pE - 0.5 * (pp - pn)) / scaleP });
            }
        }
        std::printf ("    홀수·짝수 표 == 원래 조회: 최대 차이 %.2f (단위 1e-12)\n", worst);
        CHECK (worst < 1.0, "evalParts must match evalPair (%.2f)", worst);
    }

    // 그리드 진폭별 왜곡 (Python 계산과 같은 수치가 나와야 함)
    auto curveThd = [&] (double amp, double* h2db, double* h3db) {
        const int n = 1024;
        std::vector<float> y ((size_t) n);
        for (int i = 0; i < n; ++i) y[(size_t) i] = (float) c (amp * std::sin (2.0 * kPi * i / n));
        double h[10], dc;
        harmonics (y, (double) n, 1.0, 0, n, h, 10, &dc);
        *h2db = 20.0 * std::log10 (h[1] / h[0]);
        *h3db = 20.0 * std::log10 (h[2] / h[0]);
        return thdPercent (h, 10);
    };
    double h2, h3;
    const double t1 = curveThd (1.0, &h2, &h3);
    std::printf ("    그리드 1 Vpk: THD %.2f %% (H2 %.1f, H3 %.1f dB)\n", t1, h2, h3);
    CHECK (std::abs (t1 - 0.15) < 0.04 && h2 > h3, "1 V: THD %.2f, 2nd dominant", t1);
    const double t8 = curveThd (8.0, &h2, &h3);
    std::printf ("    그리드 8 Vpk: THD %.2f %% (H2 %.1f, H3 %.1f dB)\n", t8, h2, h3);
    CHECK (t8 > 2.0 && t8 < 3.5, "8 V: THD %.2f", t8);
    const double t32 = curveThd (32.0, &h2, &h3);
    std::printf ("    그리드 32 Vpk: THD %.2f %% (H2 %.1f, H3 %.1f dB)\n", t32, h2, h3);
    CHECK (h3 > h2, "32 V: odd harmonics dominate when clipping");
}

//------------------------------------------------------------------------------
struct ToneResult { double thd, h2db, h3db, fundDb, dc; };

static ToneResult tubeTone (bool on, double driveDb, double focus, double level, double f0 = 1000.0,
                            std::vector<float>* rawIn = nullptr, std::vector<float>* rawOut = nullptr)
{
    const double fsOS = 192000.0;
    const int total = (int) fsOS;   // 1초
    TubeStage t;
    t.prepare (fsOS);
    t.setTargets (on, driveDb, focus);
    t.reset();
    auto x = sine (fsOS, f0, level, total);
    if (rawIn) *rawIn = x;
    float* ptr = x.data();
    t.process (ptr, total);
    if (rawOut) *rawOut = x;
    double h[10], dc;
    harmonics (x, fsOS, f0, total / 2, total / 2, h, 10, &dc);
    return { thdPercent (h, 10), 20.0 * std::log10 (h[1] / h[0]), 20.0 * std::log10 (h[2] / h[0]),
             20.0 * std::log10 (h[0] / level), dc };
}

static void testTube()
{
    std::printf ("[11] EF804S 튜브 스테이지 (Drive / Focus / 보상)\n");
    {
        std::vector<float> in, out;
        tubeTone (false, 24.0, 0.0, 0.9, 1000.0, &in, &out);
        CHECK (in == out, "drive off must be bit-exact bypass");
    }

    std::printf ("    -6 dBFS 1 kHz 사인, Focus 0:\n");
    double prev = 0.0;
    for (double d : { 0.0, 12.0, 24.0, 30.0, 36.0, 48.0 })
    {
        const auto r = tubeTone (true, d, 0.0, 0.5);
        const auto ref = tubeTone (true, d, 0.0, 0.25);
        std::printf ("      Drive %4.0f dB: THD %6.2f %%  H2 %6.1f  H3 %6.1f dB  | -12 dBFS 기본파 %+.2f dB  DC %.1e\n",
                     d, r.thd, r.h2db, r.h3db, ref.fundDb, r.dc);
        CHECK (r.thd > prev, "THD must grow with drive (%.0f dB)", d);
        CHECK (std::abs (ref.fundDb) < 0.3, "reference level compensation at %.0f dB: %+.2f", d, ref.fundDb);
        CHECK (std::abs (r.dc) < 1e-3, "DC at %.0f dB", d);
        prev = r.thd;
    }

    const auto odd  = tubeTone (true, 24.0, -1.0, 0.5);
    const auto mid  = tubeTone (true, 24.0,  0.0, 0.5);
    const auto even = tubeTone (true, 24.0, +1.0, 0.5);
    std::printf ("    Focus (Drive 24 dB): 홀수 쪽 H2 %.1f dB / 중앙 H2 %.1f dB / 짝수 쪽 H2 %.1f dB\n",
                 odd.h2db, mid.h2db, even.h2db);
    CHECK (odd.h2db < -90.0, "focus -1 must remove even harmonics (H2 %.1f)", odd.h2db);
    CHECK (even.h2db > mid.h2db + 4.0, "focus +1 must raise H2 (%.1f vs %.1f)", even.h2db, mid.h2db);

    const auto small = tubeTone (true, 0.0, 0.0, 0.01);
    std::printf ("    Drive 0 dB, -40 dBFS 소신호 게인 %+.3f dB\n", small.fundDb);
    CHECK (std::abs (small.fundDb) < 0.05, "small-signal gain at 0 dB drive %+.3f", small.fundDb);
}

//------------------------------------------------------------------------------
static ToneResult xfTone (double lo, double mid, double hi, double f0, double level)
{
    const double fsOS = 192000.0;
    const int total = (int) fsOS;
    TransformerStage t;
    t.prepare (fsOS);
    t.setAmounts (lo, mid, hi);
    t.reset();
    auto x = sine (fsOS, f0, level, total);
    float* ptr = x.data();
    t.process (ptr, total);
    double h[10], dc;
    harmonics (x, fsOS, f0, total / 2, total / 2, h, 10, &dc);
    return { thdPercent (h, 10), 20.0 * std::log10 (h[1] / h[0]), 20.0 * std::log10 (h[2] / h[0]),
             20.0 * std::log10 (h[0] / level), dc };
}

static void testTransformer()
{
    std::printf ("[12] 트랜스포머 색채 (구간별)\n");
    {
        TransformerStage t;
        t.prepare (192000.0);
        t.setAmounts (0, 0, 0);
        t.reset();
        auto x = sine (192000.0, 100.0, 0.9, 20000), x0 = x;
        float* ptr = x.data();
        t.process (ptr, (int) x.size());
        CHECK (x == x0, "all-zero transformer must be bit-exact bypass");
    }
    const auto low60  = xfTone (1, 0, 0, 60.0, 0.5);
    const auto low1k  = xfTone (1, 0, 0, 1000.0, 0.5);
    const auto mid1k  = xfTone (0, 1, 0, 1000.0, 0.5);
    const auto mid60  = xfTone (0, 1, 0, 60.0, 0.5);
    const auto hi8k   = xfTone (0, 0, 1, 8000.0, 0.5);
    const auto hi1k   = xfTone (0, 0, 1, 1000.0, 0.5);
    std::printf ("    LOW 100%%  : 60 Hz THD %.2f %% (H3 %.1f dB) / 1 kHz THD %.3f %%\n", low60.thd, low60.h3db, low1k.thd);
    std::printf ("    MID 100%%  : 1 kHz THD %.2f %% (H2 %.1f, H3 %.1f dB) / 60 Hz THD %.3f %%\n", mid1k.thd, mid1k.h2db, mid1k.h3db, mid60.thd);
    std::printf ("    HIGH 100%% : 8 kHz THD %.2f %% / 1 kHz THD %.3f %%\n", hi8k.thd, hi1k.thd);
    CHECK (low60.thd > 2.0 && low60.thd < 8.0, "low band THD %.2f", low60.thd);
    CHECK (low1k.thd < low60.thd / 5.0, "low band must stay in the lows");
    CHECK (mid1k.thd > 0.5 && mid1k.thd < 3.0, "mid band THD %.2f", mid1k.thd);
    CHECK (mid60.thd < mid1k.thd / 3.0, "mid band must stay in the mids");
    CHECK (hi8k.thd > 1.0 && hi8k.thd < 6.0, "high band THD %.2f", hi8k.thd);
    CHECK (hi1k.thd < hi8k.thd / 5.0, "high band must stay in the highs");
    CHECK (std::abs (low60.dc) < 1e-3 && std::abs (mid1k.dc) < 1e-3, "transformer DC");

    // 곡선 표 (오디오 루프용) == 식: h, H, 그리고 ADAA가 쓰는 (H(b) - H(a)) / (b - a)
    {
        const auto& tc = transformerCurves();
        double worstH = 0.0, worstCapH = 0.0, worstQ = 0.0, worstSmallQ = 0.0;
        for (int c = 0; c < TransformerCurves::kNumCurves; ++c)
        {
            const auto curve = (TransformerCurves::Curve) c;
            for (int k = 0; k <= 40000; ++k)
            {
                const double u = -10.0 + 20.0 * (double) k / 40000.0 + 1.234e-6;   // 표 밖(|u| > 8)도
                double h, H, he, He;
                tc.eval (curve, tc.at (u), h, H);
                TransformerCurves::exact (curve, u, he, He);
                worstH    = std::max (worstH, std::abs (h - he));
                worstCapH = std::max (worstCapH, std::abs (H - He));

                const double du = 1.0e-3;   // 기울기(ADAA 몫)도 격자 사이에서 맞아야 함
                double h2, H2, he2, He2;
                tc.eval (curve, tc.at (u + du), h2, H2);
                TransformerCurves::exact (curve, u + du, he2, He2);
                worstQ = std::max (worstQ, std::abs ((H2 - H) / du - (He2 - He) / du));
            }
            // 아주 작은 신호: 가짜 선형 이득이 없어야 함. 몫 / 진폭은 원래 a (짝수) · a^2 (홀수)에 비례해
            // 0으로 가야 함 (MID 짝수 성분: 1e-5에서 약 3e-7). 3차 보간이면 여기서 약 3e-4가 나옴
            for (double a : { 1e-6, 1e-5 })
            {
                double h, H, h2, H2;
                tc.eval (curve, tc.at (a), h, H);
                tc.eval (curve, tc.at (0.5 * a), h2, H2);
                worstSmallQ = std::max (worstSmallQ, std::abs ((H - H2) / (0.5 * a)) / a);
            }
        }
        std::printf ("    곡선 표 == 식: h %.1e, H %.1e, ADAA 몫 %.1e, 작은 신호 몫/진폭 %.1e\n", worstH, worstCapH, worstQ, worstSmallQ);
        CHECK (worstH < 5e-8 && worstCapH < 1e-10 && worstQ < 1e-7 && worstSmallQ < 1e-6,
               "transformer tables must match the formulas");
    }
}

//------------------------------------------------------------------------------
// 간단한 radix-2 FFT (에일리어싱 측정용)
static void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// 신호에서 "배음이 아닌 성분"(= 에일리어싱)의 에너지를 기본파 대비 dB로
static double aliasingDb (const std::vector<float>& y, double fs, double f0)
{
    const size_t n = 65536;
    std::vector<std::complex<double>> a (n);
    const size_t start = y.size() - n;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / (double) (n - 1);   // Blackman-Harris 창
        const double w = 0.35875 - 0.48829 * std::cos (2 * kPi * t) + 0.14128 * std::cos (4 * kPi * t) - 0.01168 * std::cos (6 * kPi * t);
        a[i] = (double) y[start + i] * w;
    }
    fft (a);
    const double binHz = fs / (double) n;
    std::vector<bool> harmonic (n / 2, false);
    for (int k = 0; k * f0 < fs / 2.0; ++k)   // k = 0 은 DC
    {
        const long c = std::lround (k * f0 / binHz);
        for (long b = c - 10; b <= c + 10; ++b)
            if (b >= 0 && b < (long) (n / 2)) harmonic[(size_t) b] = true;
    }
    double fund = 0.0, alias = 0.0;
    const long cf = std::lround (f0 / binHz);
    for (long b = cf - 10; b <= cf + 10; ++b) fund += std::norm (a[(size_t) b]);
    for (size_t b = 0; b < n / 2; ++b)
        if (! harmonic[b]) alias += std::norm (a[b]);
    return 10.0 * std::log10 (alias / fund + 1e-300);
}

static void testZeroLatencyAntiAliasing()
{
    std::printf ("[14] ZERO LATENCY (세션 레이트) 안티에일리어싱 — ADAA\n");
    const auto& c = ef804sCurve();

    // psi 테이블 = dev의 적분 (회로를 직접 풀어 촘촘히 적분한 값과 비교)
    double worstPsi = 0.0;
    for (double v : { -40.0, -8.0, -1.0, -0.05, 0.02, 0.3, 2.0, 9.0, 60.0 })
    {
        const int steps = 4000;
        double acc = 0.0;
        for (int k = 0; k < steps; ++k)
        {
            const double a0 = v * k / steps, a1 = v * (k + 1) / steps, am = 0.5 * (a0 + a1);
            acc += (a1 - a0) / 6.0 * (c.solveDev (a0) + 4.0 * c.solveDev (am) + c.solveDev (a1));
        }
        worstPsi = std::max (worstPsi, std::abs (c.psi (v) - acc) / std::max (std::abs (acc), 1e-30));
    }
    std::printf ("    역도함수 테이블 상대 오차 최대 %.2e\n", worstPsi);
    CHECK (worstPsi < 1e-5, "psi table error %.2e", worstPsi);

    const double fs = 48000.0;
    const int total = 1 << 17;

    // 튜브: ADAA vs 그냥 곡선(같은 곡선, 안티에일리어싱 없음)
    for (double driveDb : { 24.0, 36.0 })
    {
        const double f0 = 6127.0;
        TubeStage t;
        t.prepare (fs);
        t.setTargets (true, driveDb, 0.0);
        t.reset();
        auto x = sine (fs, f0, 0.5, total);
        std::vector<float> naive ((size_t) total);
        for (int i = 0; i < total; ++i) naive[(size_t) i] = (float) t.transfer (x[(size_t) i]);
        float* ptr = x.data();
        t.process (ptr, total);
        const double aN = aliasingDb (naive, fs, f0), aA = aliasingDb (x, fs, f0);
        std::printf ("    EF804S Drive %2.0f dB, 6.1 kHz @48k: 에일리어싱 그냥 %.1f dBc -> ADAA %.1f dBc (%.1f dB 감소)\n",
                     driveDb, aN, aA, aN - aA);
        CHECK (aN - aA > 6.0, "ADAA must reduce tube aliasing (drive %.0f): %.1f -> %.1f", driveDb, aN, aA);
    }

    // 트랜스포머 HIGH: 7 kHz — 같은 곡선을 그냥 샘플마다 적용한 것과 비교
    {
        const double f0 = 7031.0, amp = 0.7;
        TransformerStage t;
        t.prepare (fs);
        t.setAmounts (0, 0, 1);
        t.reset();
        auto x = sine (fs, f0, amp, total);
        std::vector<float> naive ((size_t) total);
        for (int i = 0; i < total; ++i)   // 7 kHz는 거의 전부 HIGH 구간 -> 곡선을 원신호에 직접
            naive[(size_t) i] = (float) (x[(size_t) i] + TransformerStage::hHigh (x[(size_t) i]));
        float* ptr = x.data();
        t.process (ptr, total);
        const double aN = aliasingDb (naive, fs, f0), aA = aliasingDb (x, fs, f0);
        std::printf ("    트랜스포머 HIGH 100%%, 7 kHz @48k: 에일리어싱 그냥 %.1f dBc -> ADAA %.1f dBc (%.1f dB 감소)\n", aN, aA, aN - aA);
        CHECK (aN - aA > 10.0, "transformer aliasing %.1f -> %.1f", aN, aA);
    }

    // 소신호 투명도 (세션 레이트)
    {
        TubeStage t;
        t.prepare (fs);
        t.setTargets (true, 0.0, 0.0);
        t.reset();
        auto x = sine (fs, 1000.0, 0.01, (int) fs);
        float* ptr = x.data();
        t.process (ptr, (int) x.size());
        double h[3], dc;
        harmonics (x, fs, 1000.0, (int) fs / 2, (int) fs / 2, h, 3, &dc);
        const double g = 20.0 * std::log10 (h[0] / 0.01);
        std::printf ("    Drive 0 dB, -40 dBFS @48k 게인 %+.3f dB\n", g);
        CHECK (std::abs (g) < 0.05, "small-signal gain @48k %+.3f", g);
    }

    // 무거운 드라이브: 고역 기본파 레벨/위상이 정확한 곡선(오버샘플링과 같은 결과)과 같아야 함
    //  (보정 없는 ADAA는 압축 성분이 반 샘플 늦어서 고역이 수~10 dB 뜸)
    auto toneAt = [&] (TubeStage& t, const std::vector<float>& x, double f0, bool naive) {
        std::vector<float> y (x.size());
        if (naive) for (size_t i = 0; i < x.size(); ++i) y[i] = (float) t.transfer (x[i]);
        else { y = x; float* ptr = y.data(); t.process (ptr, (int) y.size()); }
        const size_t start = y.size() / 2, len = y.size() / 2;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < len; ++i)
        {
            const double ph = 2.0 * kPi * f0 * (double) (start + i) / fs;
            re += y[start + i] * std::sin (ph);
            im += y[start + i] * std::cos (ph);
        }
        return std::complex<double> (re, im) * (2.0 / (double) len);
    };
    {
        double worstDb = 0.0, worstDeg = 0.0;
        for (double driveDb : { 30.0, 36.0, 42.0, 48.0 })
            for (double amp : { 0.25, 0.5, 0.9 })
                for (double f0 : { 1000.0, 5000.0, 10000.0, 15000.0 })
                {
                    TubeStage t;
                    t.prepare (fs);
                    t.setTargets (true, driveDb, 0.0);
                    t.reset();
                    const auto x = sine (fs, f0, amp, (int) fs);
                    const auto ref = toneAt (t, x, f0, true);
                    t.reset();
                    const auto got = toneAt (t, x, f0, false);
                    worstDb  = std::max (worstDb,  std::abs (20.0 * std::log10 (std::abs (got) / std::abs (ref))));
                    worstDeg = std::max (worstDeg, std::abs (std::arg (got / ref)) * 180.0 / kPi);
                }
        std::printf ("    사인 1개 (Drive 30~48, -12~-1 dBFS, 1~15 kHz) 기본파: 정확한 곡선 대비 최대 %.3f dB / %.2f도\n", worstDb, worstDeg);
        CHECK (worstDb < 0.05 && worstDeg < 0.5, "single-tone fundamental error %.3f dB %.2f deg", worstDb, worstDeg);
    }
    {
        // 큰 저음(100 Hz) 위의 작은 고역 (-34 dBFS): 저음이 동작점을 흔드는 만큼 고역이 눌려야 함
        double worstDb = 0.0, plainWorst = 0.0;
        for (double driveDb : { 36.0, 42.0, 48.0 })
            for (double lo : { 0.25, 0.5, 0.9 })
                for (double fh : { 10000.0, 15000.0 })
                {
                    std::vector<float> x ((size_t) fs);
                    for (size_t i = 0; i < x.size(); ++i)
                        x[i] = (float) (lo * std::sin (2.0 * kPi * 100.0 * (double) i / fs) + 0.02 * std::sin (2.0 * kPi * fh * (double) i / fs));
                    TubeStage t;
                    t.prepare (fs);
                    t.setTargets (true, driveDb, 0.0);
                    t.reset();
                    const double ref = std::abs (toneAt (t, x, fh, true));
                    t.reset();
                    const double got = std::abs (toneAt (t, x, fh, false));
                    worstDb = std::max (worstDb, std::abs (20.0 * std::log10 (got / ref)));
                    plainWorst = std::max (plainWorst, 20.0 * std::log10 (ref / 0.02));
                }
        std::printf ("    큰 저음 + 작은 고역: 고역 레벨 정확한 곡선 대비 최대 %.2f dB\n", worstDb);
        CHECK (worstDb < 0.5, "HF-on-bass error %.2f dB", worstDb);
    }
    {
        // Focus 끝값에서도 (짝수 성분 2배 / 0)
        double worstDb = 0.0;
        for (double focus : { -1.0, 1.0 })
            for (double f0 : { 5000.0, 12000.0 })
            {
                TubeStage t;
                t.prepare (fs);
                t.setTargets (true, 42.0, focus);
                t.reset();
                const auto x = sine (fs, f0, 0.5, (int) fs);
                const auto ref = toneAt (t, x, f0, true);
                t.reset();
                const auto got = toneAt (t, x, f0, false);
                worstDb = std::max (worstDb, std::abs (20.0 * std::log10 (std::abs (got) / std::abs (ref))));
            }
        std::printf ("    Focus -100/+100, Drive 42: 기본파 오차 최대 %.3f dB\n", worstDb);
        CHECK (worstDb < 0.05, "focus extremes fundamental error %.3f dB", worstDb);
    }
    {
        // 트랜스포머: 큰 신호에서 기본파는 "눌리기만" 해야 함 (크로스오버 근처에서 확장 X), 위상도 거의 그대로
        double worstUp = -99.0, worstDeg = 0.0, lowComp = 0.0;
        for (int band = 0; band < 3; ++band)
            for (double f0 : { 30.0, 60.0, 120.0, 200.0, 350.0, 1000.0, 2500.0, 4000.0, 6000.0, 10000.0, 16000.0 })
            {
                TransformerStage t;
                t.prepare (fs);
                t.setAmounts (band == 0 ? 1 : 0, band == 1 ? 1 : 0, band == 2 ? 1 : 0);
                t.reset();
                const double amp = 0.9;
                auto x = sine (fs, f0, amp, (int) fs);
                float* ptr = x.data();
                t.process (ptr, (int) x.size());
                double re = 0.0, im = 0.0;
                const size_t start = x.size() / 2, len = x.size() / 2;
                for (size_t i = 0; i < len; ++i)
                {
                    const double ph = 2.0 * kPi * f0 * (double) (start + i) / fs;
                    re += x[start + i] * std::sin (ph);
                    im += x[start + i] * std::cos (ph);
                }
                const auto shelf = svfResponse (makeHighShelfMatched (TransformerStage::kShelfHz, TransformerStage::kShelfQ,
                                                               band == 2 ? -TransformerStage::kShelfMaxCutDb : 0.0, fs), f0, fs);
                const auto rel = std::complex<double> (re, im) * (2.0 / (double) len) / (amp * shelf);
                const double db = 20.0 * std::log10 (std::abs (rel));
                worstUp  = std::max (worstUp, db);
                worstDeg = std::max (worstDeg, std::abs (std::arg (rel)) * 180.0 / kPi);
                if (band == 0 && f0 < 50.0) lowComp = db;
            }
        std::printf ("    트랜스포머 100%%, -1 dBFS 기본파: 최대 %+.2f dB (확장 없어야 함), 위상 최대 %.1f도, LOW 30 Hz 압축 %+.2f dB\n",
                     worstUp, worstDeg, lowComp);
        CHECK (worstUp < 0.05, "transformer must not expand: %+.2f dB", worstUp);
        CHECK (worstDeg < 8.0, "transformer fundamental phase %.1f deg", worstDeg);
        CHECK (lowComp < -1.0, "LOW band should compress big bass: %+.2f dB", lowComp);
    }
}

//------------------------------------------------------------------------------
static void testSessionRateCurves()
{
    std::printf ("[15] 세션 레이트 곡선 = 아날로그 곡선 (매칭 설계, 크램핑 없음)\n");
    auto bellA = [] (double W, double Q, double A) {
        const double re = 1.0 - W * W, nu = A * W / Q, de = W / (Q * A);
        return 10.0 * std::log10 ((re * re + nu * nu) / (re * re + de * de));
    };
    auto shelfA = [] (double W, double Q, double A) {
        const double r = std::sqrt (A) * W / Q;
        return 10.0 * std::log10 (A * A * ((1 - A * W * W) * (1 - A * W * W) + r * r) / ((A - W * W) * (A - W * W) + r * r));
    };
    auto tiltA = [] (double f, double tiltDb) {   // 1차: outGain * (1 + (G - 1) * wp / (s + wp))
        const double G = std::pow (10.0, -2.0 * tiltDb / 20.0), fp = kTiltPivotHz / std::sqrt (G);
        const std::complex<double> s (0.0, f / fp);
        return 20.0 * std::log10 (std::abs (std::pow (10.0, tiltDb / 20.0) * (1.0 + (G - 1.0) / (s + 1.0))));
    };
    for (double fs : { 44100.0, 48000.0 })
    {
        double wBell = 0.0, wShelf = 0.0, wBil = 0.0, wTilt = 0.0;
        for (double f = 20.0; f <= 20000.0; f *= 1.02)
        {
            for (double fc : { 1400.0, 2000.0, 2800.0, 4000.0 })
                for (double q : { 0.3, 1.0, 6.0 })
                    for (double g : { 1.0, 3.0, 6.0 })
                    {
                        const double a = bellA (f / fc, q, std::pow (10.0, g / 40.0));
                        wBell = std::max (wBell, std::abs (20.0 * std::log10 (std::abs (svfResponse (makeBellMatched (fc, q, g, fs), f, fs))) - a));
                        wBil  = std::max (wBil,  std::abs (20.0 * std::log10 (std::abs (svfResponse (makeBell (fc, q, g, fs), f, fs))) - a));
                    }
            for (double q : { 0.3, 0.5, 1.0, 2.0 })
                for (double g : { -18.0, -6.0, 4.0, 12.0 })
                {
                    const double a = shelfA (f / kTrebleShelfHz, q, std::pow (10.0, g / 40.0));
                    wShelf = std::max (wShelf, std::abs (20.0 * std::log10 (std::abs (svfResponse (makeHighShelfMatched (kTrebleShelfHz, q, g, fs), f, fs))) - a));
                }
            for (double g : { -1.5, -0.5 })
            {
                const double a = shelfA (f / TransformerStage::kShelfHz, TransformerStage::kShelfQ, std::pow (10.0, g / 40.0));
                wShelf = std::max (wShelf, std::abs (20.0 * std::log10 (std::abs (svfResponse (
                             makeHighShelfMatched (TransformerStage::kShelfHz, TransformerStage::kShelfQ, g, fs), f, fs))) - a));
            }
            for (double t : { -4.0, -1.0, 2.0, 4.0 })
                wTilt = std::max (wTilt, std::abs (20.0 * std::log10 (std::abs (tiltResponse (makeTilt (kTiltPivotHz, t, fs), f, fs))) - tiltA (f, t)));
        }
        std::printf ("    %.1f kHz: Presence 벨 최대 %.2f dB (쌍선형이면 %.2f) / 셸프 최대 %.2f dB / Pivot 최대 %.2f dB\n",
                     fs / 1000.0, wBell, wBil, wShelf, wTilt);
        CHECK (wBell < 0.45, "matched bell error %.2f dB @ %.0f", wBell, fs);
        CHECK (wShelf < 0.35, "matched shelf error %.2f dB @ %.0f", wShelf, fs);
        CHECK (wTilt < 0.1, "tilt error %.2f dB @ %.0f", wTilt, fs);
    }

    // 0 dB = 정확히 통과 (EQ off 비트 동일 조건)
    const auto b0 = makeBellMatched (2800.0, 1.0, 0.0, 48000.0);
    const auto s0 = makeHighShelfMatched (kTrebleShelfHz, 0.5, 0.0, 48000.0);
    CHECK (sameValue (b0.m0, 1.0) && sameValue (b0.m1, 0.0) && sameValue (b0.m2, 0.0), "bell 0 dB must be identity");
    CHECK (sameValue (s0.m0, 1.0) && sameValue (s0.m1, 0.0) && sameValue (s0.m2, 0.0), "shelf 0 dB must be identity");
}

//------------------------------------------------------------------------------
static void testRobustness()
{
    std::printf ("[16] 견고성 (표 끝, 온셋, DC, NaN)\n");
    const double fs = 48000.0;
    const auto& c = ef804sCurve();

    // 1) 표 끝(약 ±1500 V)을 넘나드는 아주 큰 그리드 전압: 출력이 정적 곡선 범위를 넘지 않아야 함
    {
        double worstRatio = 0.0;
        for (double drive : { 42.0, 48.0 })
            for (double amp : { 8.0, 11.9, 13.0, 20.0 })
            {
                TubeStage t;
                t.prepare (fs);
                t.setTargets (true, drive, 0.0);
                t.reset();
                auto x = sine (fs, 100.0, amp, (int) fs / 2);
                double refPeak = 0.0;
                for (float v : x) refPeak = std::max (refPeak, std::abs (t.transfer (v)));
                float* ptr = x.data();
                t.process (ptr, (int) x.size());
                double peak = 0.0;
                for (size_t i = x.size() / 2; i < x.size(); ++i) peak = std::max (peak, (double) std::abs (x[i]));
                worstRatio = std::max (worstRatio, peak / refPeak);
            }
        // 엔진 전체: 로우 셸프 +12 dB (160 Hz, 날카로운 Q) + Drive 48, 100 Hz +5 dBFS
        Engine e;
        e.prepare (fs, 1, 512);
        Params p;
        p.driveOn = true;
        p.drive = 48.0f;
        p.bands[0].used = true; p.bands[0].type = BandType::LowShelf;
        p.bands[0].freq = 160.0f; p.bands[0].gain = 12.0f; p.bands[0].q = 2.83f;
        e.setParams (p);
        e.reset();
        auto x = sine (fs, 100.0, std::pow (10.0, 5.0 / 20.0), (int) fs);
        double peak = 0.0;
        for (size_t start = 0; start < x.size(); start += 512)
        {
            float* ch[1] = { x.data() + start };
            e.process (ch, 1, (int) std::min<size_t> (512, x.size() - start));
        }
        for (size_t i = x.size() / 2; i < x.size(); ++i) peak = std::max (peak, (double) std::abs (x[i]));
        std::printf ("    그리드 수백~2500 V: 피크 / 정적 곡선 피크 최대 %.2f배, 엔진(Bass +12 Q2, Drive 48, +5 dBFS) 피크 %.2f\n",
                     worstRatio, peak);
        CHECK (worstRatio < 1.25, "table-edge overshoot x%.2f", worstRatio);
        CHECK (peak < 1.0, "engine peak at extreme drive %.2f", peak);
    }

    // 2) 온셋: 무음 뒤 60 Hz 버스트. 같은 DC 블로커를 거친 정적 곡선과 첫 주기 피크가 같아야 함 (튀는 것 없음)
    {
        double worst = 0.0;
        for (double drive : { 36.0, 48.0 })
        {
            TubeStage t;
            t.prepare (fs);
            t.setTargets (true, drive, 0.0);
            t.reset();
            const int n = (int) (fs * 0.2), start = (int) (fs * 0.05);
            std::vector<float> x ((size_t) n, 0.0f);
            for (int i = start; i < n; ++i) x[(size_t) i] = (float) (0.9 * std::sin (2.0 * kPi * 60.0 * (i - start) / fs));
            // 기준: norm * (x + 홀수(x) + DC블로커(짝수(x)))
            const double scale = TubeStage::kGridVoltsPerFullScale * dbToGain (drive);
            const double norm  = t.transfer (1.0e-3) / (1.0e-3 + c.dev (1.0e-3 * scale) / scale);
            const double R = std::exp (-2.0 * kPi * 5.0 / fs);
            double x1 = 0.0, y1 = 0.0, refPeak = 0.0;
            for (int i = 0; i < start + (int) (fs / 60.0); ++i)
            {
                const double v = x[(size_t) i] * scale, dp = c.dev (v), dn = c.dev (-v);
                const double even = 0.5 * (dp + dn) / scale, hp = even - x1 + R * y1;
                x1 = even; y1 = hp;
                refPeak = std::max (refPeak, std::abs (norm * (x[(size_t) i] + 0.5 * (dp - dn) / scale + hp)));
            }
            float* ptr = x.data();
            t.process (ptr, n);
            double peak = 0.0;
            for (int i = start; i < start + (int) (fs / 60.0); ++i) peak = std::max (peak, (double) std::abs (x[(size_t) i]));
            worst = std::max (worst, std::abs (20.0 * std::log10 (peak / refPeak)));
        }
        std::printf ("    무음 뒤 60 Hz 버스트 첫 주기 피크: 정적 곡선 대비 최대 %.2f dB\n", worst);
        CHECK (worst < 0.3, "onset peak deviation %.2f dB", worst);
    }

    // 3) 저역 사인의 DC: 비대칭 곡선이라도 출력 DC가 남지 않아야 함
    {
        double worstDc = 0.0;
        for (double drive : { 36.0, 48.0 })
            for (double f0 : { 20.0, 50.0 })
            {
                TubeStage t;
                t.prepare (fs);
                t.setTargets (true, drive, 1.0);   // Focus +100: 짝수 성분 최대
                t.reset();
                auto x = sine (fs, f0, 0.5, (int) (fs * 4));
                float* ptr = x.data();
                t.process (ptr, (int) x.size());
                double sum = 0.0;
                const size_t len = (size_t) (fs * 2);
                for (size_t i = x.size() - len; i < x.size(); ++i) sum += x[i];
                worstDc = std::max (worstDc, std::abs (sum / (double) len));
            }
        std::printf ("    20/50 Hz, Drive 36/48, Focus +100: 출력 DC 최대 %.1e\n", worstDc);
        CHECK (worstDc < 1.0e-5, "DC leak %.2e", worstDc);
    }

    // 4) NaN이 들어와도 표 조회가 정의되지 않은 동작을 하지 않고, reset 뒤에는 정상
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        double dp, dn, pp, pn;
        c.evalPair (nan, dp, dn, pp, pn);
        CHECK (std::isnan (c.dev (nan)) && std::isnan (c.psi (nan)) && std::isnan (dp) && std::isnan (pp), "NaN must stay NaN");
        double dO, dE, pO, pE;
        c.evalParts (nan, dO, dE, pO, pE);
        double th, tH;
        const auto& tc = transformerCurves();
        tc.eval (TransformerCurves::MidOdd, tc.at (nan), th, tH);
        CHECK (std::isnan (dO) && std::isnan (pE) && std::isnan (th) && std::isnan (tH), "NaN must stay NaN (fast tables)");
        TubeStage t;
        t.prepare (fs);
        t.setTargets (true, 30.0, 0.0);
        t.reset();
        auto x = sine (fs, 440.0, 0.5, 4800);
        x[100] = std::numeric_limits<float>::quiet_NaN();
        float* ptr = x.data();
        t.process (ptr, (int) x.size());
        t.reset();
        auto y = sine (fs, 440.0, 0.5, 4800);
        ptr = y.data();
        t.process (ptr, (int) y.size());
        bool finite = true;
        for (float v : y) finite = finite && std::isfinite (v);
        CHECK (finite, "tube must recover after reset");
    }
}

//------------------------------------------------------------------------------
// 사인 성분 크기 (dBFS, 직사각 창 -> 정수 주기가 되게 길이를 고름)
static double toneDb (const std::vector<float>& y, double fs, double f, int start, int len)
{
    std::complex<double> acc (0.0, 0.0);
    const double w = 2.0 * kPi * f / fs;
    for (int i = start; i < start + len; ++i)
        acc += (double) y[(size_t) i] * std::complex<double> (std::cos (w * i), -std::sin (w * i));
    return 20.0 * std::log10 (std::max (2.0 * std::abs (acc) / len, 1.0e-15));
}

static void runEngine (const Params& p, double fs, std::vector<float>& L, std::vector<float>& R, int nch = 2)
{
    Engine e;
    e.prepare (fs, nch, 512);
    e.setParams (p);
    e.reset();
    for (size_t start = 0; start < L.size(); start += 512)
    {
        const int n = (int) std::min<size_t> (512, L.size() - start);
        float* ptr[2] = { L.data() + start, R.data() + start };
        e.process (ptr, nch, n);
    }
}

static void testBandTubeRegions()
{
    std::printf ("[18] 밴드마다의 튜브 · 트랜스포머 (그 밴드 부분만, 밴드마다 드라이브 · 포커스 · 믹스 · 양)\n");
    const double fs = 96000.0;
    const int len = (int) fs, mStart = len / 2, mLen = len / 2;   // 뒤쪽 0.5 s 측정 (2 Hz 단위 정수 주기)

    auto twoTone = [&] (double f1, double f2, double a)
    {
        std::vector<float> x ((size_t) len);
        for (int i = 0; i < len; ++i)
            x[(size_t) i] = (float) (a * std::sin (2.0 * kPi * f1 * i / fs) + a * std::sin (2.0 * kPi * f2 * i / fs));
        return x;
    };
    auto harm = [&] (const std::vector<float>& y, double f)   // 2 · 3차 배음 중 큰 것 (기본파 대비 dBc)
    {
        const double f0 = toneDb (y, fs, f, mStart, mLen);
        return std::max (toneDb (y, fs, 2.0 * f, mStart, mLen), toneDb (y, fs, 3.0 * f, mStart, mLen)) - f0;
    };
    auto tubeBand = [] (BandType t, double f, double q, float drive, Placement pl = Placement::Stereo)
    {
        auto b = makeBand (t, f, 0.0, q, pl);
        b.tube = true;
        b.drive = drive;
        return b;
    };

    const auto x = twoTone (100.0, 3000.0, 0.3);
    auto run = [&] (const Params& p) { auto L = x, R = x; runEngine (p, fs, L, R); return L; };

    Params all;     all.driveOn = true; all.drive = 36.0f;
    Params bell;    bell.bands[0]  = tubeBand (BandType::Bell, 3000.0, 2.0, 36.0f);
    Params shelf;   shelf.bands[0] = tubeBand (BandType::LowShelf, 150.0, 1.0, 36.0f);
    Params shelf12; shelf12.bands[0] = tubeBand (BandType::LowShelf, 150.0, 1.0, 12.0f);
    Params both;    both.bands[0] = bell.bands[0]; both.bands[1] = shelf12.bands[0];

    const auto yAll = run (all), yBell = run (bell), yShelf = run (shelf), yShelf12 = run (shelf12), yBoth = run (both);
    std::printf ("    100 Hz + 3 kHz: ALL(36 dB) -> 100 Hz 배음 %.1f / 3 kHz 배음 %.1f dBc\n", harm (yAll, 100.0), harm (yAll, 3000.0));
    std::printf ("      벨 3 kHz만 TUBE 36 dB -> 100 Hz %.1f / 3 kHz %.1f dBc\n", harm (yBell, 100.0), harm (yBell, 3000.0));
    std::printf ("      로우 셸프 150 Hz만 TUBE 36 dB -> 100 Hz %.1f / 3 kHz %.1f dBc\n", harm (yShelf, 100.0), harm (yShelf, 3000.0));
    std::printf ("      둘 다 (벨 36 dB + 셸프 12 dB) -> 3 kHz %.1f (벨만 %.1f), 100 Hz %.1f (셸프 12 dB만 %.1f) dBc\n",
                 harm (yBoth, 3000.0), harm (yBell, 3000.0), harm (yBoth, 100.0), harm (yShelf12, 100.0));
    CHECK (harm (yBell, 3000.0) > -60.0 && harm (yBell, 100.0) < harm (yAll, 100.0) - 50.0, "bell region: only 3 kHz saturated");
    CHECK (harm (yShelf, 100.0) > -60.0 && harm (yShelf, 3000.0) < harm (yAll, 3000.0) - 50.0, "shelf region: only 100 Hz saturated");
    CHECK (std::abs (harm (yBoth, 3000.0) - harm (yBell, 3000.0)) < 1.0 && std::abs (harm (yBoth, 100.0) - harm (yShelf12, 100.0)) < 1.0,
           "each band keeps its own drive");
    CHECK (harm (yShelf, 100.0) > harm (yShelf12, 100.0) + 10.0, "band drive changes the amount");

    // 밴드 튜브 MIX: 0이면 비트 단위 그대로, 50 %면 배음이 6 dB 작음
    {
        Params m0 = bell;  m0.bands[0].tubeMix = 0.0f;
        Params m5 = bell;  m5.bands[0].tubeMix = 0.5f;
        CHECK (run (m0) == x, "band tube mix 0 -> bit-exact");
        const auto y5 = run (m5);
        const double d = (toneDb (yBell, fs, 9000.0, mStart, mLen) - toneDb (y5, fs, 9000.0, mStart, mLen));
        std::printf ("      벨 TUBE MIX 50 %% -> 3차 배음 %.2f dB 작아짐\n", d);
        CHECK (std::abs (d - 6.02) < 0.5, "band tube mix 50%% halves the coloration (%.2f dB)", d);
    }

    // 밴드 FOCUS: 홀수 쪽(-1)이면 짝수 배음이 거의 없음
    {
        Params odd = bell;  odd.bands[0].focus = -1.0f;
        Params even = bell; even.bands[0].focus = 1.0f;
        const auto yo = run (odd), ye = run (even);
        const double h2o = toneDb (yo, fs, 6000.0, mStart, mLen), h2e = toneDb (ye, fs, 6000.0, mStart, mLen);
        std::printf ("      벨 FOCUS 홀수 -> 2차 %.1f dB, 짝수 -> 2차 %.1f dB\n", h2o, h2e);
        CHECK (h2e > h2o + 40.0, "band focus controls even harmonics");
    }

    // ALL과 같이 켜면 밴드 쪽 착색이 더해짐 (v0.7: 밴드마다 따로)
    {
        Params a12;  a12.driveOn = true; a12.drive = 12.0f;
        Params a12b = a12; a12b.bands[0] = bell.bands[0];
        const double ha = harm (run (a12), 3000.0), hb = harm (run (a12b), 3000.0);
        std::printf ("      ALL 12 dB -> 3 kHz %.1f dBc, ALL 12 dB + 벨 TUBE 36 dB -> %.1f dBc\n", ha, hb);
        CHECK (hb > ha + 10.0, "band tube adds to ALL");
    }

    // 아무 데도 안 걸면 (밴드 게인 0) 비트 단위로 그대로
    {
        Params none;
        none.bands[0] = makeBand (BandType::Bell, 3000.0, 0.0, 2.0);
        CHECK (run (none) == x, "analog unused -> bit-exact passthrough");
    }

    // Mid 밴드: M 신호에서만 (Side 성분은 깨끗)
    {
        std::vector<float> L ((size_t) len), R ((size_t) len);
        for (int i = 0; i < len; ++i)
        {
            const double m = 0.3 * std::sin (2.0 * kPi * 1000.0 * i / fs), s = 0.3 * std::sin (2.0 * kPi * 1300.0 * i / fs);
            L[(size_t) i] = (float) (m + s);
            R[(size_t) i] = (float) (m - s);
        }
        Params p;
        p.bands[0] = tubeBand (BandType::Bell, 1000.0, 1.0, 36.0f, Placement::Mid);
        runEngine (p, fs, L, R);
        std::vector<float> M ((size_t) len), S ((size_t) len);
        for (int i = 0; i < len; ++i) { M[(size_t) i] = 0.5f * (L[(size_t) i] + R[(size_t) i]); S[(size_t) i] = 0.5f * (L[(size_t) i] - R[(size_t) i]); }
        const double midH = harm (M, 1000.0);
        const double sideH = std::max ({ toneDb (M, fs, 2600.0, mStart, mLen), toneDb (M, fs, 3900.0, mStart, mLen),
                                         toneDb (S, fs, 2600.0, mStart, mLen), toneDb (S, fs, 3900.0, mStart, mLen) })
                             - toneDb (S, fs, 1300.0, mStart, mLen);
        const double midInSide = std::max (toneDb (S, fs, 2000.0, mStart, mLen), toneDb (S, fs, 3000.0, mStart, mLen)) - toneDb (M, fs, 1000.0, mStart, mLen);
        std::printf ("      Mid 벨 1 kHz TUBE -> Mid 배음 %.1f dBc, Side 1.3 kHz 배음 %.1f dBc, Side로 샌 Mid 배음 %.1f dBc\n", midH, sideH, midInSide);
        CHECK (midH > -60.0 && sideH < -95.0 && midInSide < -95.0, "mid band: tube works on M only");
    }

    // 밴드 트랜스포머: 그 부분만, 성격은 밴드 주파수로 (저역 = 대칭 3차, 중역 = 2차 + 3차)
    {
        const auto x2 = twoTone (100.0, 8000.0, 0.4);
        auto runX = [&] (const Params& p) { auto L = x2, R = x2; runEngine (p, fs, L, R); return L; };
        Params hs; hs.xfAll = false;
        hs.bands[0] = makeBand (BandType::HighShelf, 5000.0, 0.0, 1.0); hs.bands[0].xfmr = true; hs.bands[0].xfAmount = 1.0f;
        const auto yh = runX (hs);
        const double low3 = toneDb (yh, fs, 300.0, mStart, mLen) - toneDb (yh, fs, 100.0, mStart, mLen);
        const double hi3  = toneDb (yh, fs, 24000.0, mStart, mLen) - toneDb (yh, fs, 8000.0, mStart, mLen);

        const auto xl = twoTone (60.0, 1000.0, 0.4);
        auto runL = [&] (const Params& p) { auto L = xl, R = xl; runEngine (p, fs, L, R); return L; };
        Params ls; ls.xfAll = false;
        ls.bands[0] = makeBand (BandType::LowShelf, 120.0, 0.0, 1.0); ls.bands[0].xfmr = true; ls.bands[0].xfAmount = 1.0f;
        Params bm; bm.xfAll = false;
        bm.bands[0] = makeBand (BandType::Bell, 1000.0, 0.0, 1.0); bm.bands[0].xfmr = true; bm.bands[0].xfAmount = 1.0f;
        const auto yl = runL (ls), ym = runL (bm);
        const double lowH2 = toneDb (yl, fs, 120.0, mStart, mLen) - toneDb (yl, fs, 60.0, mStart, mLen);
        const double lowH3 = toneDb (yl, fs, 180.0, mStart, mLen) - toneDb (yl, fs, 60.0, mStart, mLen);
        const double midH2 = toneDb (ym, fs, 2000.0, mStart, mLen) - toneDb (ym, fs, 1000.0, mStart, mLen);
        std::printf ("      XFMR 하이 셸프 5 kHz: 100 Hz 3차 %.1f, 8 kHz 3차 %.1f dBc / 로우 셸프 120 Hz: 60 Hz 2차 %.1f, 3차 %.1f / 벨 1 kHz: 2차 %.1f dBc\n",
                     low3, hi3, lowH2, lowH3, midH2);
        CHECK (low3 < -95.0 && hi3 > -80.0, "transformer band only on its range");
        CHECK (lowH3 > -60.0 && lowH3 > lowH2 + 20.0 && midH2 > -80.0, "transformer character follows band frequency");
    }

    // 모노: Side 밴드는 할 일이 없음
    {
        auto L = x, R = x;
        Params p;
        p.bands[0] = tubeBand (BandType::Bell, 3000.0, 2.0, 36.0f, Placement::Side);
        runEngine (p, fs, L, R, 1);
        CHECK (L == x, "mono: side-band tube does nothing");
    }

    // 16개 전부 튜브 + 트랜스포머 (모양 · 채널 섞음): 유한
    {
        Params p;
        for (int i = 0; i < kMaxBands; ++i)
        {
            p.bands[(size_t) i] = tubeBand ((BandType) (i % kNumBandTypes), 60.0 * std::pow (1.4, i), 1.5, 6.0f * (float) (i % 8),
                                            (Placement) (i % kNumPlacements));
            p.bands[(size_t) i].gain = i % 2 ? 3.0f : -3.0f;
            p.bands[(size_t) i].xfmr = i % 3 == 0;
            p.bands[(size_t) i].xfAmount = 0.7f;
        }
        auto L = x, R = x;
        runEngine (p, fs, L, R);
        bool finite = true; double peak = 0.0;
        for (float v : L) { finite = finite && std::isfinite (v); peak = std::max (peak, (double) std::abs (v)); }
        std::printf ("      16밴드 전부 튜브 (드라이브 제각각, 모양 · 채널 섞음): 피크 %.2f\n", peak);
        CHECK (finite && peak < 4.0, "all 16 analog bands stay finite (%.2f)", peak);
    }
}

//------------------------------------------------------------------------------
static void testMigration()
{
    std::printf ("[17] v0.4 이하 세션 변환 (모듈 -> 밴드, 같은 곡선)\n");
    const double fs = processingRateFor (48000.0);

    // v0.4 처리와 같은 계산 (예전 ResponseModel)
    auto legacy = [&] (const LegacyModule& md, bool st, double f)
    {
        const double tDb = st ? quantizeTreble (md.trebleDb) : std::clamp (md.trebleDb, kTrebleMinDb, kTrebleMaxDb);
        const double tHz = st ? nearestStep (kTrebleSteps, md.trebleHz) : clampToSteps (kTrebleSteps, md.trebleHz);
        const double bDb = st ? quantizeBass (md.bassDb) : std::clamp (md.bassDb, kBassMinDb, kBassMaxDb);
        const double bHz = st ? nearestStep (kBassSteps, md.bassHz) : clampToSteps (kBassSteps, md.bassHz);
        std::complex<double> h = svfResponse (makeHighShelfMatched (tHz / kShelfRefToMid, std::clamp (md.trebleQ, kShelfQMin, kShelfQMax), tDb, fs), f, fs)
                               * svfResponse (makeLowShelf (bHz * kShelfRefToMid, std::clamp (md.bassQ, kShelfQMin, kShelfQMax), bDb, fs), f, fs);
        if (md.modeB)
        {
            const double pDb = st ? quantizePresence (md.presDb) : std::clamp (md.presDb, 0.0f, kPresMaxDb);
            const double pHz = st ? nearestStep (kPresenceSteps, md.presHz) : clampToSteps (kPresenceSteps, md.presHz);
            h *= svfResponse (makeBellMatched (pHz, std::clamp (md.presQ, kPresQMin, kPresQMax), pDb, fs), f, fs);
        }
        else
        {
            const double tilt = st ? quantizeTilt (md.tiltDb) : std::clamp (md.tiltDb, -kTiltMaxDb, kTiltMaxDb);
            h *= tiltResponse (makeTilt (kTiltPivotHz, tilt, fs), f, fs);
        }
        return 20.0 * std::log10 (std::abs (h));
    };

    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    auto randomLegacy = [&]
    {
        LegacyModule m;
        m.modeB    = u (rng) < 0.6f;
        m.trebleDb = u (rng) < 0.2f ? 0.0f : -18.0f + 30.0f * u (rng);
        m.bassDb   = u (rng) < 0.2f ? 0.0f : -20.0f + 32.0f * u (rng);
        m.trebleQ  = 0.3f + 1.7f * u (rng);
        m.bassQ    = 0.3f + 1.7f * u (rng);
        m.trebleHz = 5000.0f + 11000.0f * u (rng);
        m.bassHz   = 20.0f + 100.0f * u (rng);
        m.tiltDb   = -4.0f + 8.0f * u (rng);
        m.presHz   = 700.0f + 7300.0f * u (rng);
        m.presDb   = u (rng) < 0.2f ? 0.0f : 6.0f * u (rng);
        m.presQ    = 0.3f + 5.7f * u (rng);
        return m;
    };

    double worst = 0.0;
    int maxBands = 0;
    for (int t = 0; t < 80; ++t)
    {
        LegacyParams L;
        L.link = u (rng) < 0.5f;
        L.midSide = u (rng) < 0.5f;
        L.stepped = u (rng) < 0.3f;
        L.mod[0] = randomLegacy();
        L.mod[1] = randomLegacy();
        const auto bands = migrateLegacyBands (L);
        maxBands = std::max (maxBands, (int) bands.size());
        Params p;
        for (size_t i = 0; i < bands.size(); ++i) p.bands[i] = bands[i];
        const ResponseModel m (p, fs);
        const int mods = L.link ? 1 : 2;
        for (int k = 0; k < mods; ++k)
        {
            const Placement view = L.link ? Placement::Stereo
                                 : (L.midSide ? (k == 0 ? Placement::Mid : Placement::Side)
                                              : (k == 0 ? Placement::Left : Placement::Right));
            for (double f : logFreqs (20.0, 20000.0, 60))
                worst = std::max (worst, std::abs (m.magnitudeDb (f, view) - legacy (L.mod[(size_t) k], L.stepped, f)));
        }
    }
    std::printf ("    무작위 80개 (링크/L·R/M·S, 스텝 포함): 예전 곡선 대비 최대 %.4f dB, 밴드 수 최대 %d\n", worst, maxBands);
    CHECK (worst < 0.05, "migrated response differs %.4f dB", worst);
    CHECK (maxBands <= 6, "too many migrated bands %d", maxBands);

    LegacyParams flat;   // 게인 0 = 밴드 없음
    CHECK (migrateLegacyBands (flat).empty(), "flat legacy session must create no bands");
}

//------------------------------------------------------------------------------
static void dumpCsv (const std::string& dir)
{
    const double fs = processingRateFor (48000.0);
    const auto freqs = logFreqs (10.0, 22000.0, 400);
    auto writeSet = [&] (const std::string& name, const std::vector<std::pair<std::string, Params>>& set)
    {
        std::vector<std::vector<float>> irs;
        for (auto& s : set) irs.push_back (impulseResponse (s.second, fs, 96000, 1.0f, 1.0f, 0));
        std::ofstream f (dir + "/" + name + ".csv");
        f << "freq";
        for (auto& s : set) f << "," << s.first;
        f << "\n";
        for (double fr : freqs)
        {
            f << fr;
            for (auto& h : irs) f << "," << dftDb (h, fr, fs);
            f << "\n";
        }
        std::printf ("  wrote %s/%s.csv\n", dir.c_str(), name.c_str());
    };
    auto one = [] (BandParams b) { Params p; p.bands[0] = b; return p; };

    // 원본 RZ062 위치 그대로: 로우 셸프 160 Hz(40 Hz 기준), 하이 셸프 2.5 kHz(10 kHz 기준), 원본 Q(0.5 -> 0.71)
    const double vq = kVintageShelfQ / kButterworthQ;
    std::vector<std::pair<std::string, Params>> shelves;
    for (float g : { 12.f, 8.f, 4.f, -4.f, -8.f, -12.f, -16.f, -20.f })
        shelves.push_back ({ "bass" + std::to_string ((int) g), one (makeBand (BandType::LowShelf, kBassShelfHz, g, vq)) });
    for (float g : { 12.f, 8.f, 4.f, -4.f, -8.f, -12.f, -18.f })
        shelves.push_back ({ "treble" + std::to_string ((int) g), one (makeBand (BandType::HighShelf, kTrebleShelfHz, g, vq)) });
    writeSet ("shelves", shelves);

    std::vector<std::pair<std::string, Params>> shelfq;
    for (float q : { 0.3f, 0.5f, 0.707f, 1.0f, 2.0f })   // 원본 표기 Q (새 Q = 원본 / 0.707)
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "bassQ%.3f", q);
        shelfq.push_back ({ nm, one (makeBand (BandType::LowShelf, kBassShelfHz, 10.0, q / kButterworthQ)) });
        std::snprintf (nm, sizeof nm, "trebleQ%.3f", q);
        shelfq.push_back ({ nm, one (makeBand (BandType::HighShelf, kTrebleShelfHz, -10.0, q / kButterworthQ)) });
    }
    writeSet ("shelfq", shelfq);

    std::vector<std::pair<std::string, Params>> pres;
    for (float q : { 0.3f, 0.6f, 1.0f, 2.0f, 4.0f, 6.0f })
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "presQ%.1f", q);
        pres.push_back ({ nm, one (makeBand (BandType::Bell, 2800.0, 6.0, q)) });
    }
    writeSet ("presence", pres);

    std::vector<std::pair<std::string, Params>> tilt;
    for (int i = -4; i <= 4; ++i)
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "tilt%+.1f", (double) i);
        tilt.push_back ({ nm, one (makeBand (BandType::Tilt, kTiltPivotHz, (double) i)) });
    }
    writeSet ("tilt", tilt);

    std::vector<std::pair<std::string, Params>> cuts;
    for (int s : { 0, 1, 3, 5, 7 })   // 6 / 12 / 24 / 48 / 96 dB/oct
    {
        cuts.push_back ({ "lc" + std::to_string (kCutSlopes[(size_t) s]), one (makeBand (BandType::LowCut,  100.0,  0, 1.0, Placement::Stereo, s)) });
        cuts.push_back ({ "hc" + std::to_string (kCutSlopes[(size_t) s]), one (makeBand (BandType::HighCut, 8000.0, 0, 1.0, Placement::Stereo, s)) });
    }
    writeSet ("cuts", cuts);

    std::vector<std::pair<std::string, Params>> notch;
    for (int q : { 1, 4, 12 })
        notch.push_back ({ "notchQ" + std::to_string (q), one (makeBand (BandType::Notch, 1000.0, 0, (double) q)) });
    writeSet ("notch", notch);

    // 튜브: Drive별 왜곡 (Focus -1 / 0 / +1)
    {
        std::ofstream f (dir + "/tube.csv");
        f << "drive,thd_odd,thd_mid,thd_even,h2_mid,h3_mid\n";
        for (int d = 0; d <= 48; d += 3)
        {
            const auto o = tubeTone (true, d, -1.0, 0.5), m = tubeTone (true, d, 0.0, 0.5), e = tubeTone (true, d, 1.0, 0.5);
            f << d << "," << o.thd << "," << m.thd << "," << e.thd << "," << m.h2db << "," << m.h3db << "\n";
        }
        std::printf ("  wrote %s/tube.csv\n", dir.c_str());
    }
    // 트랜스포머: 구간별 THD vs 주파수 (각 100 %)
    {
        std::ofstream f (dir + "/xfmr.csv");
        f << "freq,low,mid,high\n";
        for (double raw : logFreqs (30.0, 12000.0, 25))
        {
            const double fr = 2.0 * std::round (raw / 2.0);   // 0.5초 분석 창에 정수 주기 -> 누설 없음
            f << fr << "," << xfTone (1, 0, 0, fr, 0.5).thd << "," << xfTone (0, 1, 0, fr, 0.5).thd
              << "," << xfTone (0, 0, 1, fr, 0.5).thd << "\n";
        }
        std::printf ("  wrote %s/xfmr.csv\n", dir.c_str());
    }
}

//------------------------------------------------------------------------------
int main (int argc, char** argv)
{
   #ifdef _WIN32
    SetConsoleOutputCP (CP_UTF8);
   #endif

    if (argc >= 3 && std::strcmp (argv[1], "--dump") == 0)
    {
        dumpCsv (argv[2]);
        return 0;
    }

    testNeutral();
    testBandShapes();
    testAnalyticMatchesProcessing();
    testPlacements();
    testQBehaviour();
    testSampleRateConsistency();
    testClicks();
    testStress();
    testBlockSizeIndependence();
    testEf804sCurve();
    testTube();
    testTransformer();
    testZeroLatencyAntiAliasing();
    testSessionRateCurves();
    testRobustness();
    testMigration();
    testBandTubeRegions();

    std::printf ("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
