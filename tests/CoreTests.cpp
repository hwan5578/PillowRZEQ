// =============================================================================
//  Pillow RZ EQ v0.3 — DSP 코어 단독 테스트 (JUCE 불필요)
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

static ModuleParams randomModule (std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    ModuleParams m;
    m.mode      = u (rng) < 0.5f ? Mode::A_Tilt : Mode::B_Presence;
    m.trebleDb  = kTrebleMinDb + u (rng) * (kTrebleMaxDb - kTrebleMinDb);
    m.bassDb    = kBassMinDb + u (rng) * (kBassMaxDb - kBassMinDb);
    m.trebleQ   = kShelfQMin * std::pow (kShelfQMax / kShelfQMin, u (rng));
    m.bassQ     = kShelfQMin * std::pow (kShelfQMax / kShelfQMin, u (rng));
    m.tiltDb    = -kTiltMaxDb + u (rng) * 2.0f * kTiltMaxDb;
    m.presIndex = (int) (u (rng) * 3.999f);
    m.presDb    = u (rng) * kPresMaxDb;
    m.presQ     = kPresQMin * std::pow (kPresQMax / kPresQMin, u (rng));
    m.driveDb   = u (rng) * kDriveMaxDb;
    m.outputDb  = -6.0f + 12.0f * u (rng);
    return m;
}

// 모든 기능을 무작위로 (스트레스·블록 크기 테스트용)
static Params randomParams (std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    Params p;
    p.routing = u (rng) < 0.5f ? Routing::Stereo : Routing::MidSide;
    p.link    = u (rng) < 0.5f;
    p.stepped = u (rng) < 0.3f;
    p.eqOn    = u (rng) < 0.9f;
    p.driveOn = u (rng) < 0.6f;
    p.focus   = -1.0f + 2.0f * u (rng);
    p.mix     = u (rng) < 0.3f ? u (rng) : 1.0f;
    p.xfLow   = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.xfMid   = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.xfHigh  = u (rng) < 0.5f ? u (rng) : 0.0f;
    p.mod[0]  = randomModule (rng);
    p.mod[1]  = randomModule (rng);
    return p;
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

//------------------------------------------------------------------------------
static void testQuantizers()
{
    std::printf ("[1] 원본 스텝 양자화\n");
    CHECK (quantizeBass (12.0f) == 12.0f, "bass 12");
    CHECK (quantizeBass (1.1f) == 2.0f, "bass 1.1 -> 2");
    CHECK (quantizeBass (-1.0f) == 0.0f, "bass -1 -> 0");
    CHECK (quantizeBass (-2.5f) == -4.0f, "bass -2.5 -> -4");
    CHECK (quantizeBass (-5.1f) == -6.0f, "bass -5.1 -> -6");
    CHECK (quantizeBass (-30.0f) == -20.0f, "bass clamp");
    CHECK (quantizeTreble (-17.2f) == -18.0f, "treble -17.2 -> -18");
    CHECK (std::abs (quantizeTilt (3.9f) - 4.0f) < 1e-5f, "tilt 3.9 -> 4");
    CHECK (std::abs (quantizeTilt (-1.3f) + 1.0f) < 1e-5f, "tilt -1.3 -> -1 (got %f)", quantizeTilt (-1.3f));
    CHECK (std::abs (quantizePresence (4.5f) - 5.0f) < 1e-5f, "pres 4.5 -> 5");
    CHECK (std::abs (quantizePresence (6.4f) - 6.0f) < 1e-5f, "pres clamp 6");
    CHECK (quantizePresence (-1.0f) == 0.0f, "pres clamp low");
    CHECK (bassDetents().size() == 16, "bass detents");
    CHECK (trebleDetents().size() == 16, "treble detents");
    CHECK (tiltDetents().size() == 9, "tilt detents %zu", tiltDetents().size());
    CHECK (presenceDetents().size() == 7, "presence detents");
}

//------------------------------------------------------------------------------
static void testNeutral()
{
    std::printf ("[2] 기본 상태(게인 0, Drive off, 트랜스포머 0) -> 투명\n");
    std::mt19937 rng (1);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    std::vector<float> L0 (8192), R0 (8192);
    for (size_t i = 0; i < L0.size(); ++i) { L0[i] = d (rng); R0[i] = d (rng); }

    for (int routing = 0; routing < 2; ++routing)
        for (int link = 0; link < 2; ++link)
        {
            Params p;
            p.routing = routing == 0 ? Routing::Stereo : Routing::MidSide;
            p.link = link == 1;
            p.mod[0].bassQ = 1.7f; p.mod[0].presQ = 5.0f;   // 게인 0이면 Q는 영향 없어야 함
            p.mod[1].trebleQ = 0.35f; p.mod[1].mode = Mode::A_Tilt;
            Engine e;
            e.prepare (192000.0, 2, 2048);
            e.setParams (p);
            e.reset();
            auto L = L0, R = R0;
            float* ptr[2] = { L.data(), R.data() };
            e.process (ptr, 2, (int) L.size());
            double err = 0.0;
            for (size_t i = 0; i < L.size(); ++i)
                err = std::max ({ err, (double) std::abs (L[i] - L0[i]), (double) std::abs (R[i] - R0[i]) });
            if (routing == 0)
                CHECK (err == 0.0, "stereo link=%d not bit-exact (%.3g)", link, err);
            else
                CHECK (err < 1.0e-6, "M/S link=%d error %.3g", link, err);
        }

    // 모노
    Engine e;
    e.prepare (192000.0, 1, 2048);
    e.setParams (Params {});
    e.reset();
    auto L = L0;
    float* ptr[1] = { L.data() };
    e.process (ptr, 1, (int) L.size());
    CHECK (L == L0, "mono not bit-exact");
}

//------------------------------------------------------------------------------
static void testAnalyticMatchesProcessing()
{
    std::printf ("[3] GUI 해석 곡선 == 실제 처리 응답 (모듈별, 트랜스포머 셸프·Mix 포함)\n");
    std::mt19937 rng (42);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };   // ZERO LATENCY(세션 레이트) + HQ
    double worst = 0.0;
    for (double fs : rates)
        for (int t = 0; t < 8; ++t)
        {
            Params p;
            p.link = false;
            p.stepped = u (rng) < 0.3f;
            p.mod[0] = randomModule (rng);
            p.mod[1] = randomModule (rng);
            p.xfLow = u (rng); p.xfMid = u (rng); p.xfHigh = u (rng);
            p.mix = u (rng) < 0.4f ? 0.3f + 0.7f * u (rng) : 1.0f;
            const int len = (int) (fs * 0.6);
            for (int m = 0; m < 2; ++m)
            {
                const auto h = impulseResponse (p, fs, len, m == 0 ? 1.0f : 0.0f, m == 1 ? 1.0f : 0.0f, m);
                const ResponseModel model (p, m, fs);
                const double level = resolveModule (p, m).outputDb;   // GUI 곡선은 모듈 Level 제외
                for (double f : logFreqs (20.0, 20000.0, 50))
                {
                    const double ref = model.magnitudeDb (f) + level;
                    if (ref > -40.0)
                        worst = std::max (worst, std::abs (dftDb (h, f, fs) - ref));
                }
            }
        }
    std::printf ("    최대 오차 %.5f dB\n", worst);
    CHECK (worst < 0.01, "analytic vs measured worst %.5f dB", worst);
}

//------------------------------------------------------------------------------
static void testSpecPoints()
{
    std::printf ("[4] 원본 사양 지점 (빈티지 Q, 48 kHz 세션: ZERO LATENCY 48k / HQ 192k)\n");
    for (const double fs : { 48000.0, processingRateFor (48000.0) })
    {
    std::printf ("  처리 레이트 %.0f Hz\n", fs);
    auto at = [&] (const Params& p, double f) { return ResponseModel (p, 0, fs).magnitudeDb (f); };

    Params p;
    p.mod[0].bassDb = 12.0f;
    const double b40 = at (p, 40.0), b1k = at (p, 1000.0);
    std::printf ("    Bass +12 : 40 Hz %.2f dB, 1 kHz %.2f dB\n", b40, b1k);
    CHECK (b40 > 11.0 && b40 <= 12.05, "bass +12 @40Hz = %.2f", b40);
    CHECK (b1k < 0.6, "bass +12 @1kHz = %.2f", b1k);

    p = Params {}; p.mod[0].bassDb = -20.0f;
    CHECK (at (p, 40.0) < -18.0, "bass -20 @40Hz");

    p = Params {}; p.mod[0].trebleDb = 12.0f;
    const double t10 = at (p, 10000.0), t400 = at (p, 400.0);
    std::printf ("    Treble +12 : 10 kHz %.2f dB, 400 Hz %.2f dB\n", t10, t400);
    CHECK (t10 > 11.0 && t10 <= 12.05, "treble +12 @10k = %.2f", t10);
    CHECK (t400 < 0.6, "treble +12 @400Hz = %.2f", t400);

    p = Params {}; p.mod[0].mode = Mode::A_Tilt; p.mod[0].tiltDb = 4.0f;
    const double p650 = at (p, 650.0), p10k = at (p, 10000.0), p100 = at (p, 100.0);
    std::printf ("    Pivot +4 : 100 Hz %.2f, 650 Hz %.3f, 10 kHz %.2f dB\n", p100, p650, p10k);
    CHECK (std::abs (p650) < 0.05, "pivot center %.3f", p650);
    CHECK (std::abs (p10k - 4.0) < 0.2, "pivot 10k %.2f", p10k);
    CHECK (std::abs (p100 + 4.0) < 0.35, "pivot 100Hz %.2f", p100);

    for (int i = 0; i < 4; ++i)
    {
        p = Params {}; p.mod[0].presIndex = i; p.mod[0].presDb = 6.0f;
        const double pk = at (p, kPresenceHz[(size_t) i]);
        CHECK (std::abs (pk - 6.0) < 0.02, "presence %d peak %.3f", i, pk);
    }

    p = Params {}; p.mod[0].mode = Mode::A_Tilt; p.mod[0].presDb = 6.0f;
    CHECK (std::abs (at (p, 2800.0)) < 1e-6, "presence must be off in A mode");
    p = Params {}; p.mod[0].tiltDb = 4.0f;
    CHECK (std::abs (at (p, 10000.0)) < 1e-6, "pivot must be off in B mode");
    p = Params {}; p.mod[0].bassDb = 12.0f; p.eqOn = false;
    CHECK (std::abs (at (p, 40.0)) < 1e-6, "EQ off must be flat");
    p = Params {}; p.xfHigh = 1.0f;
    const double sh = at (p, 20000.0);
    std::printf ("    트랜스포머 HIGH 100%% : 20 kHz %.2f dB\n", sh);
    CHECK (sh < -1.0 && sh > -1.6, "transformer HF softening %.2f", sh);
    }
}

//------------------------------------------------------------------------------
static double halfGainBandwidthOct (const Params& p, double fs, double fc)
{
    const ResponseModel m (p, 0, fs);
    const double half = 0.5 * m.magnitudeDb (fc);
    auto edge = [&] (double dir) {
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
    std::printf ("[5] Q 조절 동작\n");
    const double fs = processingRateFor (48000.0);
    double prevBw = 1e9;
    for (float q : { 0.3f, 0.6f, 1.0f, 2.0f, 4.0f, 6.0f })
    {
        Params p; p.mod[0].presIndex = 2; p.mod[0].presDb = 6.0f; p.mod[0].presQ = q;
        const double bw = halfGainBandwidthOct (p, fs, 2800.0);
        std::printf ("    Presence 2.8 kHz +6 dB, Q %.1f -> 대역폭 %.2f oct\n", q, bw);
        CHECK (bw < prevBw, "bandwidth must shrink as Q grows (Q %.1f)", q);
        prevBw = bw;
    }

    double prevSlope = 0.0;
    for (float q : { 0.3f, 0.5f, 0.707f, 1.0f, 2.0f })
    {
        Params p; p.mod[0].bassDb = 12.0f; p.mod[0].bassQ = q;
        const ResponseModel m (p, 0, fs);
        double maxSlope = 0.0, maxGain = -100.0, minGain = 100.0;
        for (double f : logFreqs (20.0, 20000.0, 2000))
        {
            const double g = m.magnitudeDb (f);
            maxSlope = std::max (maxSlope, std::abs (m.magnitudeDb (f * 1.01) - g) / std::log2 (1.01));
            maxGain  = std::max (maxGain, g);
            minGain  = std::min (minGain, g);
        }
        std::printf ("    Bass +12, Q %.3f -> 최대 기울기 %.2f dB/oct, 최대 %.2f dB, 최소 %.2f dB\n",
                     q, maxSlope, maxGain, minGain);
        CHECK (maxSlope > prevSlope, "shelf slope must grow with Q (Q %.3f)", q);
        if (q <= 0.71f) CHECK (maxGain < 12.05, "no overshoot expected at Q %.3f", q);
        if (q >= 1.0f)  CHECK (maxGain > 12.3 && minGain < -0.3, "resonant shelf expected at Q %.3f", q);
        prevSlope = maxSlope;
    }
}

//------------------------------------------------------------------------------
static void testSampleRateConsistency()
{
    std::printf ("[6] 세션 샘플레이트 간 일관성 (20 Hz ~ 20 kHz)\n");
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    double worstHq = 0.0, worstZero = 0.0;
    for (int t = 0; t < 60; ++t)
    {
        Params p;
        p.mod[0] = randomModule (rng);
        p.xfHigh = u (rng);
        const ResponseModel ref (p, 0, 768000.0);
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const ResponseModel hq (p, 0, processingRateFor (sr)), zero (p, 0, sr);
            for (double f : logFreqs (20.0, 20000.0, 100))
            {
                const double r = ref.magnitudeDb (f);
                worstHq   = std::max (worstHq,   std::abs (hq.magnitudeDb (f) - r));
                worstZero = std::max (worstZero, std::abs (zero.magnitudeDb (f) - r));
            }
        }
    }
    std::printf ("    44.1k/48k/96k/192k 세션 vs 아날로그 기준 최대 차이: HQ %.3f dB / ZERO LATENCY %.3f dB\n", worstHq, worstZero);
    CHECK (worstHq < 0.15, "sample-rate deviation (HQ) %.3f dB", worstHq);
    CHECK (worstZero < 0.45, "sample-rate deviation (zero latency) %.3f dB", worstZero);
}

//------------------------------------------------------------------------------
// 300 Hz 사인(양 채널 동일 또는 약간 다르게) -> 6 kHz 고역통과 후 최대값으로 클릭 측정
struct ClickResult { double steady, transition; };

static ClickResult measureClick (const Params& before, const Params& after, double smoothingMs)
{
    const double fs = 48000.0;   // 실제 플러그인(192k 처리)보다 불리한 조건
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
    std::printf ("[7] 파라미터·라우팅 변경 시 클릭 (스무딩 on vs off)\n");
    struct Case { const char* name; Params a, b; };
    std::vector<Case> cases;
    auto add = [&] (const char* n, Params a, Params b) { cases.push_back ({ n, a, b }); };
    {
        Params a; a.mod[0].bassDb = 12; Params b = a; b.mod[0].bassDb = -20;
        add ("Bass +12 -> -20", a, b);
    }
    {
        Params a; a.mod[0].presDb = 6; a.mod[0].presQ = 6; a.mod[0].presIndex = 0;
        Params b = a; b.mod[0].presIndex = 3;
        add ("Presence 1.4k -> 4k (Q 6)", a, b);
    }
    {
        Params a; a.mod[0].mode = Mode::A_Tilt; a.mod[0].tiltDb = -4; a.mod[0].presDb = 6;
        Params b = a; b.mod[0].mode = Mode::B_Presence;
        add ("Type A -> B", a, b);
    }
    {
        Params a; a.mod[0].bassDb = 9; a.mod[0].bassQ = 0.3f; Params b = a; b.mod[0].bassQ = 2.0f;
        add ("Bass Q 0.3 -> 2.0", a, b);
    }
    {
        Params a; a.mod[0].bassDb = -3; a.mod[0].trebleDb = 5; Params b = a; b.stepped = true;
        add ("Steps off -> on", a, b);
    }
    {
        Params a; a.link = false; a.mod[0].bassDb = 8; a.mod[1].bassDb = -12; a.mod[1].trebleDb = 6;
        Params b = a; b.routing = Routing::MidSide;
        add ("Routing L/R -> M/S", a, b);
    }
    {
        Params a; a.link = true; a.mod[0].bassDb = 8; a.mod[1].bassDb = -12;
        Params b = a; b.link = false;
        add ("Link on -> off", a, b);
    }
    {
        Params a; a.mod[0].driveDb = 18; Params b = a; b.driveOn = true;
        add ("Drive off -> on (18 dB)", a, b);
    }
    {
        Params a; a.mod[0].presDb = 6; a.mod[0].presIndex = 0; Params b = a; b.mix = 0.3f;
        add ("Mix 100 -> 30 %", a, b);
    }
    {
        Params a; Params b = a; b.xfLow = 1; b.xfMid = 1; b.xfHigh = 1;
        add ("Transformer 0 -> 100 %", a, b);
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
    std::printf ("[8] 무작위 자동화 스트레스 (모든 기능, NaN/발산 없음)\n");
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
            CHECK (peak < 50.0, "output too large at %.0f/%d (%.2f)", fs, nch, peak);
        }
}

//------------------------------------------------------------------------------
static void testBlockSizeIndependence()
{
    std::printf ("[9] 블록 크기와 무관하게 같은 결과 (라우팅 전환·튜브·트랜스포머 포함)\n");
    const double fs = 96000.0;
    const int total = 2048 * 24;
    std::mt19937 rng (3);
    std::normal_distribution<float> n (0.0f, 0.2f);
    std::vector<float> inL ((size_t) total), inR ((size_t) total);
    for (int i = 0; i < total; ++i) { inL[(size_t) i] = n (rng); inR[(size_t) i] = n (rng); }
    std::vector<Params> plist;
    for (int i = 0; i < total / 2048; ++i) plist.push_back (randomParams (rng));

    auto run = [&] (int block) {
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
}

//------------------------------------------------------------------------------
static void testRoutingAndLink()
{
    std::printf ("[13] 링크 / Mid-Side / Mix\n");
    const double fs = 192000.0;
    const int len = (int) (fs * 0.4);

    // 링크: 모듈 2 설정이 달라도 무시
    {
        Params p; p.link = true;
        p.mod[0].bassDb = 6.0f; p.mod[0].driveDb = 20.0f;
        p.mod[1].bassDb = -15.0f; p.mod[1].trebleDb = 9.0f;
        p.driveOn = true;
        Engine e; e.prepare (fs, 2, 1024); e.setParams (p); e.reset();
        auto L = sine (fs, 220.0, 0.4, len), R = L;
        float* ptr[2] = { L.data(), R.data() };
        e.process (ptr, 2, len);
        CHECK (L == R, "linked modules must process identically");
    }

    // M/S: 미드만 들어오면 모듈 1, 사이드만 들어오면 모듈 2 응답
    {
        Params p; p.link = false; p.routing = Routing::MidSide;
        p.mod[0].presDb = 6.0f; p.mod[0].presIndex = 2;
        p.mod[1].bassDb = -12.0f; p.mod[1].trebleDb = 6.0f;
        const auto hMid  = impulseResponse (p, fs, len, 1.0f,  1.0f, 0);
        const auto hSide = impulseResponse (p, fs, len, 1.0f, -1.0f, 0);
        const ResponseModel mMid (p, 0, fs), mSide (p, 1, fs);
        double worst = 0.0;
        for (double f : logFreqs (30.0, 18000.0, 40))
            worst = std::max ({ worst, std::abs (dftDb (hMid, f, fs) - mMid.magnitudeDb (f)),
                                std::abs (dftDb (hSide, f, fs) - mSide.magnitudeDb (f)) });
        std::printf ("    M/S 모듈별 응답 오차 %.4f dB\n", worst);
        CHECK (worst < 0.01, "mid/side module response error %.4f", worst);
    }

    // Mix 0 -> 원음 그대로 (EQ, Drive, 트랜스포머가 켜져 있어도)
    {
        Params p; p.mix = 0.0f; p.driveOn = true; p.xfLow = 1; p.xfHigh = 1;
        p.mod[0].bassDb = 10.0f; p.mod[0].driveDb = 30.0f;
        Engine e; e.prepare (fs, 2, 1024); e.setParams (p); e.reset();
        auto L = sine (fs, 100.0, 0.7, len), R = sine (fs, 333.0, 0.3, len);
        const auto L0 = L, R0 = R;
        float* ptr[2] = { L.data(), R.data() };
        e.process (ptr, 2, len);
        CHECK (L == L0 && R == R0, "mix 0 must pass the dry signal untouched");
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
static void dumpCsv (const std::string& dir)
{
    const double fs = processingRateFor (48000.0);
    const auto freqs = logFreqs (10.0, 22000.0, 400);
    auto writeSet = [&] (const std::string& name, const std::vector<std::pair<std::string, Params>>& set) {
        std::vector<std::vector<float>> irs;
        for (auto& s : set) irs.push_back (impulseResponse (s.second, fs, 96000, 1.0f, 0.0f, 0));
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

    std::vector<std::pair<std::string, Params>> shelves;
    for (float g : { 12.f, 8.f, 4.f, -4.f, -8.f, -12.f, -16.f, -20.f })
    { Params p; p.mod[0].bassDb = g; shelves.push_back ({ "bass" + std::to_string ((int) g), p }); }
    for (float g : { 12.f, 8.f, 4.f, -4.f, -8.f, -12.f, -18.f })
    { Params p; p.mod[0].trebleDb = g; shelves.push_back ({ "treble" + std::to_string ((int) g), p }); }
    writeSet ("shelves", shelves);

    std::vector<std::pair<std::string, Params>> shelfq;
    for (float q : { 0.3f, 0.5f, 0.707f, 1.0f, 2.0f })
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "bassQ%.3f", q);
        Params p; p.mod[0].bassDb = 10.0f; p.mod[0].bassQ = q; shelfq.push_back ({ nm, p });
        std::snprintf (nm, sizeof nm, "trebleQ%.3f", q);
        Params t; t.mod[0].trebleDb = -10.0f; t.mod[0].trebleQ = q; shelfq.push_back ({ nm, t });
    }
    writeSet ("shelfq", shelfq);

    std::vector<std::pair<std::string, Params>> pres;
    for (float q : { 0.3f, 0.6f, 1.0f, 2.0f, 4.0f, 6.0f })
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "presQ%.1f", q);
        Params p; p.mod[0].presIndex = 2; p.mod[0].presDb = 6.0f; p.mod[0].presQ = q; pres.push_back ({ nm, p });
    }
    writeSet ("presence", pres);

    std::vector<std::pair<std::string, Params>> tilt;
    for (int i = -4; i <= 4; ++i)
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "tilt%+.1f", (double) i);
        Params p; p.mod[0].mode = Mode::A_Tilt; p.mod[0].tiltDb = (float) i; tilt.push_back ({ nm, p });
    }
    writeSet ("tilt", tilt);

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

    testQuantizers();
    testNeutral();
    testAnalyticMatchesProcessing();
    testSpecPoints();
    testQBehaviour();
    testSampleRateConsistency();
    testClicks();
    testStress();
    testBlockSizeIndependence();
    testEf804sCurve();
    testTube();
    testTransformer();
    testRoutingAndLink();
    testZeroLatencyAntiAliasing();
    testSessionRateCurves();

    std::printf ("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
