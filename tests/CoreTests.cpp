// =============================================================================
//  Pillow RZ EQ — DSP 코어 단독 테스트 (JUCE 불필요)
//
//    빌드:  g++ -std=c++17 -O2 -I../Source CoreTests.cpp -o CoreTests
//    실행:  ./CoreTests            (모든 테스트)
//           ./CoreTests --dump out (그래프용 CSV 저장)
// =============================================================================
#include "dsp/RZ062Core.h"

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

#define CHECK(cond, ...)                                   \
    do {                                                   \
        ++g_checks;                                        \
        if (! (cond)) {                                    \
            ++g_failures;                                  \
            std::printf ("  FAIL (%s:%d): ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                     \
            std::printf ("\n");                            \
        }                                                  \
    } while (0)

//------------------------------------------------------------------------------
static std::vector<float> impulseResponse (const Params& p, double fs, int len)
{
    Engine e;
    e.prepare (fs, 1);
    e.setParams (p);
    e.reset();
    std::vector<float> buf ((size_t) len, 0.0f);
    buf[0] = 1.0f;
    float* ptr[1] = { buf.data() };
    e.processEQ (ptr, 1, len);
    return buf;
}

static double dftDb (const std::vector<float>& h, double f, double fs)
{
    const double w = 2.0 * kPi * f / fs;
    const std::complex<double> rot (std::cos (w), -std::sin (w));
    std::complex<double> ph (1.0, 0.0), acc (0.0, 0.0);
    for (float v : h) { acc += (double) v * ph; ph *= rot; }
    return 20.0 * std::log10 (std::max (std::abs (acc), 1.0e-12));
}

static std::vector<double> logFreqs (double lo, double hi, int n)
{
    std::vector<double> f ((size_t) n);
    for (int i = 0; i < n; ++i)
        f[(size_t) i] = lo * std::pow (hi / lo, (double) i / (n - 1));
    return f;
}

static Params vintage()
{
    Params p;
    p.mode = Mode::B_Presence;
    return p;
}

//------------------------------------------------------------------------------
static void testQuantizers()
{
    std::printf ("[1] 원본 스텝 양자화\n");
    CHECK (quantizeBass (12.0f) == 12.0f, "bass 12");
    CHECK (quantizeBass (1.1f) == 2.0f, "bass 1.1 -> 2");
    CHECK (quantizeBass (-1.0f) == 0.0f, "bass -1 -> 0 (got %f)", quantizeBass (-1.0f));
    CHECK (quantizeBass (-2.5f) == -4.0f, "bass -2.5 -> -4 (got %f)", quantizeBass (-2.5f));
    CHECK (quantizeBass (-4.9f) == -4.0f, "bass -4.9 -> -4");
    CHECK (quantizeBass (-5.1f) == -6.0f, "bass -5.1 -> -6");
    CHECK (quantizeBass (-30.0f) == -20.0f, "bass clamp");
    CHECK (quantizeTreble (-17.2f) == -18.0f, "treble -17.2 -> -18");
    CHECK (quantizeTreble (0.9f) == 0.0f, "treble 0.9 -> 0");
    CHECK (std::abs (quantizeTilt (3.9f) - 4.0f) < 1e-5f, "tilt 3.9 -> 4.0");
    CHECK (std::abs (quantizeTilt (-1.3f) + 1.6f) < 1e-5f, "tilt -1.3 -> -1.6 (got %f)", quantizeTilt (-1.3f));
    CHECK (std::abs (quantizePresence (4.5f) - 4.8f) < 1e-5f, "pres 4.5 -> 4.8");
    CHECK (quantizePresence (-1.0f) == 0.0f, "pres clamp low");
    CHECK (bassDetents().size() == 16, "bass detents %zu", bassDetents().size());
    CHECK (trebleDetents().size() == 16, "treble detents");
    CHECK (presenceDetents().size() == 7, "presence detents");
}

//------------------------------------------------------------------------------
static void testNeutralIsBitExact()
{
    std::printf ("[2] 모든 게인 0 -> 완전 투명(비트 동일)\n");
    for (int m = 0; m < 2; ++m)
    {
        Params p = vintage();
        p.mode = m == 0 ? Mode::A_Tilt : Mode::B_Presence;
        p.bassQ = 1.7f; p.trebleQ = 0.35f; p.presQ = 5.0f;   // Q는 게인 0이면 영향 없어야 함
        Engine e;
        e.prepare (192000.0, 2);
        e.setParams (p);
        e.reset();
        std::mt19937 rng (1);
        std::uniform_real_distribution<float> d (-1.0f, 1.0f);
        std::vector<float> L (4096), R (4096);
        for (size_t i = 0; i < L.size(); ++i) { L[i] = d (rng); R[i] = d (rng); }
        auto L0 = L, R0 = R;
        float* ptr[2] = { L.data(), R.data() };
        e.processEQ (ptr, 2, 4096);
        e.processOutput (ptr, 2, 4096);
        CHECK (L == L0 && R == R0, "mode %d: neutral not bit-exact", m);
    }
}

//------------------------------------------------------------------------------
static Params randomParams (std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    Params p;
    p.mode      = u (rng) < 0.5f ? Mode::A_Tilt : Mode::B_Presence;
    p.stepped   = u (rng) < 0.3f;
    p.trebleDb  = kTrebleMinDb + u (rng) * (kTrebleMaxDb - kTrebleMinDb);
    p.bassDb    = kBassMinDb + u (rng) * (kBassMaxDb - kBassMinDb);
    p.trebleQ   = kShelfQMin * std::pow (kShelfQMax / kShelfQMin, u (rng));
    p.bassQ     = kShelfQMin * std::pow (kShelfQMax / kShelfQMin, u (rng));
    p.tiltDb    = -kTiltMaxDb + u (rng) * 2.0f * kTiltMaxDb;
    p.presIndex = (int) (u (rng) * 3.999f);
    p.presDb    = u (rng) * kPresMaxDb;
    p.presQ     = kPresQMin * std::pow (kPresQMax / kPresQMin, u (rng));
    return p;
}

static void testAnalyticMatchesProcessing()
{
    std::printf ("[3] GUI 해석 곡선 == 실제 처리 응답 (임펄스 측정)\n");
    std::mt19937 rng (42);
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    double worst = 0.0;
    for (double fs : rates)
    {
        for (int t = 0; t < 12; ++t)
        {
            Params p = randomParams (rng);
            if (t == 0) { p = vintage(); p.bassDb = 12; p.trebleDb = 12; p.presDb = 4.8f; p.presQ = 6.0f; p.presIndex = 0; }
            const auto h = impulseResponse (p, fs, (int) (fs * 1.5));
            const ResponseModel model (p, fs);
            for (double f : logFreqs (20.0, std::min (20000.0, 0.45 * fs), 60))
            {
                const double err = std::abs (dftDb (h, f, fs) - model.magnitudeDb (f));
                worst = std::max (worst, err);
            }
        }
    }
    std::printf ("    최대 오차 %.5f dB\n", worst);
    CHECK (worst < 0.01, "analytic vs measured worst %.5f dB", worst);
}

//------------------------------------------------------------------------------
static void testSpecPoints()
{
    std::printf ("[4] 원본 사양 지점 (빈티지 Q, 48 kHz 세션의 처리 레이트)\n");
    const double fs = processingRateFor (48000.0);
    auto at = [&] (const Params& p, double f) { return ResponseModel (p, fs).magnitudeDb (f); };

    Params p = vintage();
    p.bassDb = 12.0f;
    const double b40 = at (p, 40.0), b1k = at (p, 1000.0);
    std::printf ("    Bass +12 : 40 Hz %.2f dB, 1 kHz %.2f dB\n", b40, b1k);
    CHECK (b40 > 11.0 && b40 <= 12.05, "bass +12 @40Hz = %.2f", b40);
    CHECK (b1k < 0.6, "bass +12 @1kHz = %.2f", b1k);

    p = vintage(); p.bassDb = -20.0f;
    const double c40 = at (p, 40.0), c1k = at (p, 1000.0);
    std::printf ("    Bass -20 : 40 Hz %.2f dB, 1 kHz %.2f dB\n", c40, c1k);
    CHECK (c40 < -18.0, "bass -20 @40Hz = %.2f", c40);
    CHECK (c1k > -1.0, "bass -20 @1kHz = %.2f", c1k);

    p = vintage(); p.trebleDb = 12.0f;
    const double t10 = at (p, 10000.0), t1k = at (p, 1000.0), t400 = at (p, 400.0);
    std::printf ("    Treble +12 : 10 kHz %.2f dB, 1 kHz %.2f dB, 400 Hz %.2f dB\n", t10, t1k, t400);
    CHECK (t10 > 11.0 && t10 <= 12.05, "treble +12 @10k = %.2f", t10);
    CHECK (t1k < 2.0, "treble +12 @1k = %.2f", t1k);
    CHECK (t400 < 0.6, "treble +12 @400Hz = %.2f", t400);

    // 650 Hz 축 대칭 설계 확인: treble(f) == bass(400000/f)
    {
        double worstSym = 0.0;
        for (double g : { 12.0, -12.0 })
            for (double q : { 0.3, 0.5, 1.0, 2.0 })
                for (double f : logFreqs (400.0, 15000.0, 40))
                {
                    Params a = vintage(); a.trebleDb = (float) g; a.trebleQ = (float) q;
                    Params b = vintage(); b.bassDb   = (float) g; b.bassQ   = (float) q;
                    worstSym = std::max (worstSym, std::abs (at (a, f) - at (b, 400000.0 / f)));
                }
        std::printf ("    베이스/트레블 대칭 오차 최대 %.3f dB\n", worstSym);
        CHECK (worstSym < 0.05, "bass/treble symmetry %.3f", worstSym);
    }

    p = vintage(); p.trebleDb = -18.0f;
    const double tc10 = at (p, 10000.0);
    std::printf ("    Treble -18 : 10 kHz %.2f dB\n", tc10);
    CHECK (tc10 < -16.0, "treble -18 @10k = %.2f", tc10);

    p = vintage(); p.mode = Mode::A_Tilt; p.tiltDb = 4.0f;
    const double p650 = at (p, 650.0), p10k = at (p, 10000.0), p100 = at (p, 100.0);
    std::printf ("    Tilt +4 : 100 Hz %.2f, 650 Hz %.3f, 10 kHz %.2f dB\n", p100, p650, p10k);
    CHECK (std::abs (p650) < 0.05, "tilt pivot %.3f", p650);
    CHECK (std::abs (p10k - 4.0) < 0.2, "tilt 10k %.2f", p10k);
    CHECK (std::abs (p100 + 4.0) < 0.35, "tilt 100Hz %.2f", p100);

    for (int i = 0; i < 4; ++i)
    {
        p = vintage(); p.presIndex = i; p.presDb = 4.8f;
        const double pk = at (p, kPresenceHz[(size_t) i]);
        CHECK (std::abs (pk - 4.8) < 0.02, "presence %d peak %.3f", i, pk);
    }

    // A 모드에서는 프레즌스가, B 모드에서는 틸트가 꺼져야 함
    p = vintage(); p.mode = Mode::A_Tilt; p.presDb = 4.8f;
    CHECK (std::abs (at (p, 2800.0)) < 1e-6, "presence must be off in A mode");
    p = vintage(); p.mode = Mode::B_Presence; p.tiltDb = 4.0f;
    CHECK (std::abs (at (p, 10000.0)) < 1e-6, "tilt must be off in B mode");
}

//------------------------------------------------------------------------------
// 피크 기준 절반(dB) 지점 사이의 대역폭(옥타브)
static double halfGainBandwidthOct (const Params& p, double fs, double fc)
{
    const ResponseModel m (p, fs);
    const double peak = m.magnitudeDb (fc), half = 0.5 * peak;
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
        Params p = vintage(); p.presIndex = 2; p.presDb = 4.8f; p.presQ = q;
        const double bw = halfGainBandwidthOct (p, fs, 2800.0);
        std::printf ("    Presence 2.8 kHz +4.8 dB, Q %.1f -> 대역폭 %.2f oct\n", q, bw);
        CHECK (bw < prevBw, "bandwidth must shrink as Q grows (Q %.1f)", q);
        prevBw = bw;
    }

    // 셸프: Q가 클수록 중간점 부근 기울기가 가팔라지고, Q>0.707이면 오버슈트
    double prevSlope = 0.0;
    for (float q : { 0.3f, 0.5f, 0.707f, 1.0f, 2.0f })
    {
        Params p = vintage(); p.bassDb = 12.0f; p.bassQ = q;
        const ResponseModel m (p, fs);
        double maxSlope = 0.0, maxGain = -100.0, minGain = 100.0;
        for (double f : logFreqs (20.0, 20000.0, 2000))
        {
            const double s = std::abs (m.magnitudeDb (f * 1.01) - m.magnitudeDb (f)) / std::log2 (1.01);
            maxSlope = std::max (maxSlope, s);
            maxGain  = std::max (maxGain, m.magnitudeDb (f));
            minGain  = std::min (minGain, m.magnitudeDb (f));
        }
        std::printf ("    Bass +12, Q %.3f -> 최대 기울기 %.2f dB/oct, 최대 %.2f dB, 최소 %.2f dB\n",
                     q, maxSlope, maxGain, minGain);
        CHECK (maxSlope > prevSlope, "shelf slope must grow with Q (Q %.3f)", q);
        if (q <= 0.71f) CHECK (maxGain < 12.05, "no overshoot expected at Q %.3f (max %.2f)", q, maxGain);
        if (q >= 1.0f)  CHECK (maxGain > 12.3 && minGain < -0.3, "resonant shelf expected at Q %.3f", q);
        prevSlope = maxSlope;
    }
}

//------------------------------------------------------------------------------
static void testSampleRateConsistency()
{
    std::printf ("[6] 세션 샘플레이트 간 일관성 (20 Hz ~ 20 kHz, 오버샘플 처리 레이트 기준)\n");
    std::mt19937 rng (7);
    double worst = 0.0, worstNoOs = 0.0;
    for (int t = 0; t < 40; ++t)
    {
        const Params p = randomParams (rng);
        const ResponseModel ref (p, 768000.0);   // 사실상 아날로그 기준
        const ResponseModel a (p, processingRateFor (44100.0)), b (p, processingRateFor (48000.0)),
                            c (p, processingRateFor (96000.0)), d (p, processingRateFor (192000.0));
        const ResponseModel raw (p, 44100.0);    // 오버샘플링 없이 44.1k에서 돌렸다면
        for (double f : logFreqs (20.0, 20000.0, 100))
        {
            const double r = ref.magnitudeDb (f);
            worst = std::max ({ worst, std::abs (a.magnitudeDb (f) - r), std::abs (b.magnitudeDb (f) - r),
                                std::abs (c.magnitudeDb (f) - r), std::abs (d.magnitudeDb (f) - r) });
            worstNoOs = std::max (worstNoOs, std::abs (raw.magnitudeDb (f) - r));
        }
    }
    std::printf ("    44.1k/48k/96k/192k 세션 vs 아날로그 기준 최대 차이 %.3f dB (오버샘플링 없을 때 %.2f dB)\n",
                 worst, worstNoOs);
    CHECK (worst < 0.15, "sample-rate deviation %.3f dB", worst);
}

//------------------------------------------------------------------------------
// 고역통과 후 최대값으로 "클릭"을 측정 (300 Hz 사인에 HP 6 kHz)
struct ClickResult { double steady, transition; };

static ClickResult measureClick (const Params& before, const Params& after, double smoothingMs)
{
    const double fs = 48000.0;   // 실제 플러그인(192k 처리)보다 불리한 조건
    Engine e;
    e.prepare (fs, 1);
    e.setSmoothingMs (smoothingMs);
    e.setParams (before);
    e.reset();

    SvfCoeffs hp = makeBell (6000.0, 0.707, 0.0, fs);   // 계수 틀만 재사용
    hp.k = 1.0 / 0.707; hp.m0 = 1.0; hp.m1 = -hp.k; hp.m2 = -1.0;
    hp = finishSvf (hp);
    SvfState hs;

    const int total = (int) fs * 2, switchAt = (int) fs;
    std::vector<float> x ((size_t) total);
    for (int i = 0; i < total; ++i)
        x[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 300.0 * i / fs);

    double steady = 0.0, trans = 0.0;
    const int block = 64;
    for (int start = 0; start < total; start += block)
    {
        if (start == switchAt) e.setParams (after);
        float* ptr[1] = { x.data() + start };
        e.processEQ (ptr, 1, block);
        for (int i = start; i < start + block; ++i)
        {
            const double y = std::abs (svfTick (hp, hs, x[(size_t) i]));
            if (i > switchAt / 2 && i < switchAt) steady = std::max (steady, y);
            else if (i >= switchAt && i < switchAt + (int) (0.3 * fs)) trans = std::max (trans, y);
            else if (i > switchAt + (int) (0.6 * fs)) steady = std::max (steady, y);
        }
    }
    return { steady, trans };
}

static void testClicks()
{
    std::printf ("[7] 파라미터 변경 시 클릭 (스무딩 on vs off)\n");
    struct Case { const char* name; Params a, b; };
    std::vector<Case> cases;
    {
        Params a = vintage(); a.bassDb = 12; Params b = a; b.bassDb = -20;
        cases.push_back ({ "Bass +12 -> -20", a, b });
    }
    {
        Params a = vintage(); a.presDb = 4.8f; a.presQ = 6.0f; a.presIndex = 0;
        Params b = a; b.presIndex = 3;
        cases.push_back ({ "Presence 1.4k -> 4k (Q 6)", a, b });
    }
    {
        Params a = vintage(); a.mode = Mode::A_Tilt; a.tiltDb = -4; a.presDb = 4.8f;
        Params b = a; b.mode = Mode::B_Presence;
        cases.push_back ({ "Mode A(tilt -4) -> B(pres 4.8)", a, b });
    }
    {
        Params a = vintage(); a.bassDb = 9.0f; a.bassQ = 0.3f; Params b = a; b.bassQ = 2.0f;
        cases.push_back ({ "Bass Q 0.3 -> 2.0", a, b });
    }
    {
        Params a = vintage(); a.bassDb = -3.0f; a.trebleDb = 5.0f; Params b = a; b.stepped = true;
        cases.push_back ({ "Steps off -> on", a, b });
    }
    for (auto& c : cases)
    {
        const auto on  = measureClick (c.a, c.b, 15.0);
        const auto off = measureClick (c.a, c.b, 0.0);
        const double rOn = on.transition / on.steady, rOff = off.transition / off.steady;
        std::printf ("    %-32s 스무딩 on: x%.2f   off: x%.2f\n", c.name, rOn, rOff);
        CHECK (rOn < 3.0, "%s: click ratio %.2f", c.name, rOn);
    }
}

//------------------------------------------------------------------------------
static void testStress()
{
    std::printf ("[8] 무작위 자동화 스트레스 (NaN/발산 없음)\n");
    for (double fs : { 44100.0, 192000.0 })
    {
        Engine e;
        e.prepare (fs, 2);
        std::mt19937 rng (99);
        std::normal_distribution<float> n (0.0f, 0.3f);
        const int total = (int) (fs * 6.0), block = 64;
        std::vector<float> L ((size_t) block), R ((size_t) block);
        double peak = 0.0;
        bool finite = true;
        for (int start = 0; start < total; start += block)
        {
            e.setParams (randomParams (rng));
            for (int i = 0; i < block; ++i) { L[(size_t) i] = n (rng); R[(size_t) i] = n (rng); }
            float* ptr[2] = { L.data(), R.data() };
            e.processEQ (ptr, 2, block);
            e.processOutput (ptr, 2, block);
            for (int i = 0; i < block; ++i)
            {
                finite = finite && std::isfinite (L[(size_t) i]) && std::isfinite (R[(size_t) i]);
                peak = std::max ({ peak, (double) std::abs (L[(size_t) i]), (double) std::abs (R[(size_t) i]) });
            }
        }
        std::printf ("    fs %.0f: 최대 피크 %.2f\n", fs, peak);
        CHECK (finite, "non-finite output at %.0f", fs);
        CHECK (peak < 20.0, "output too large at %.0f (%.2f)", fs, peak);
    }
}

//------------------------------------------------------------------------------
static void testBlockSizeIndependence()
{
    std::printf ("[9] 블록 크기와 무관하게 같은 결과\n");
    const double fs = 48000.0;
    const int total = 2048 * 24;
    std::mt19937 rng (3);
    std::normal_distribution<float> n (0.0f, 0.2f);
    std::vector<float> input ((size_t) total);
    for (auto& v : input) v = n (rng);
    std::vector<Params> plist;
    for (int i = 0; i < total / 2048; ++i) plist.push_back (randomParams (rng));

    auto run = [&] (int block) {
        Engine e;
        e.prepare (fs, 1);
        e.setParams (plist[0]);
        e.reset();
        auto x = input;
        for (int start = 0; start < total; start += block)
        {
            if (start % 2048 == 0) e.setParams (plist[(size_t) (start / 2048)]);
            float* ptr[1] = { x.data() + start };
            e.processEQ (ptr, 1, block);
            e.processOutput (ptr, 1, block);
        }
        return x;
    };
    const auto ref = run (2048);
    for (int b : { 1, 16, 64, 512 })
        CHECK (run (b) == ref, "block size %d differs from 2048", b);
}

//------------------------------------------------------------------------------
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

static void testTube()
{
    std::printf ("[10] 튜브 스테이지\n");
    const double fsOS = 192000.0, f0 = 1000.0;
    const int total = (int) fsOS;           // 1초 = 정확히 1000 사이클
    auto run = [&] (double amount, double level) {
        TubeStage t;
        t.prepare (fsOS, 1);
        t.setAmount (amount);
        t.reset();
        std::vector<float> x ((size_t) total);
        for (int i = 0; i < total; ++i)
            x[(size_t) i] = (float) (level * std::sin (2.0 * kPi * f0 * i / fsOS));
        auto in = x;
        float* ptr[1] = { x.data() };
        t.process (ptr, 1, total);
        return std::make_pair (in, x);
    };

    {
        auto [in, out] = run (0.0, 0.9);
        CHECK (in == out, "tube amount 0 must be bit-exact bypass");
    }

    std::printf ("    -6 dBFS 1 kHz 사인 기준:\n");
    double prevThd = 0.0;
    for (double a : { 0.1, 0.25, 0.5, 0.75, 1.0 })
    {
        auto [in, out] = run (a, 0.5);
        double h[10], dc;
        harmonics (out, fsOS, f0, total / 2, total / 2, h, 10, &dc);
        double sum = 0.0;
        for (int k = 1; k < 10; ++k) sum += h[k] * h[k];
        const double thd = 100.0 * std::sqrt (sum) / h[0];
        const double fundDb = 20.0 * std::log10 (h[0] / 0.5);
        std::printf ("      amount %.2f: THD %6.2f %%  H2 %6.1f dB  H3 %6.1f dB  기본파 %+.2f dB  DC %.1e\n",
                     a, thd, 20.0 * std::log10 (h[1] / h[0]), 20.0 * std::log10 (h[2] / h[0]), fundDb, dc);
        CHECK (thd > prevThd, "THD must grow with amount");
        CHECK (std::abs (dc) < 1e-3, "DC offset %.2e at amount %.2f", dc, a);
        if (a <= 0.25) CHECK (h[1] > h[2], "low drive should be 2nd-harmonic dominant");
        prevThd = thd;
    }

    {
        auto [in, out] = run (1.0, 0.01);   // -40 dBFS
        double h[3], dc;
        harmonics (out, fsOS, f0, total / 2, total / 2, h, 3, &dc);
        const double g = 20.0 * std::log10 (h[0] / 0.01);
        std::printf ("    -40 dBFS 소신호 게인 (amount 1.0): %+.3f dB\n", g);
        CHECK (std::abs (g) < 0.1, "small-signal gain %.3f dB", g);
    }
}

//------------------------------------------------------------------------------
static void dumpCsv (const std::string& dir)
{
    const double fs = processingRateFor (48000.0);
    const auto freqs = logFreqs (10.0, 22000.0, 400);
    auto writeSet = [&] (const std::string& name, const std::vector<std::pair<std::string, Params>>& set) {
        std::vector<std::vector<float>> irs;
        for (auto& s : set) irs.push_back (impulseResponse (s.second, fs, 96000));
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
    { Params p = vintage(); p.bassDb = g; shelves.push_back ({ "bass" + std::to_string ((int) g), p }); }
    for (float g : { 12.f, 8.f, 4.f, -4.f, -8.f, -12.f, -18.f })
    { Params p = vintage(); p.trebleDb = g; shelves.push_back ({ "treble" + std::to_string ((int) g), p }); }
    writeSet ("shelves", shelves);

    std::vector<std::pair<std::string, Params>> shelfq;
    for (float q : { 0.3f, 0.5f, 0.707f, 1.0f, 2.0f })
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "bassQ%.3f", q);
        Params p = vintage(); p.bassDb = 10.0f; p.bassQ = q; shelfq.push_back ({ nm, p });
        std::snprintf (nm, sizeof nm, "trebleQ%.3f", q);
        Params t = vintage(); t.trebleDb = -10.0f; t.trebleQ = q; shelfq.push_back ({ nm, t });
    }
    writeSet ("shelfq", shelfq);

    std::vector<std::pair<std::string, Params>> pres;
    for (int i = 0; i < 4; ++i)
    { Params p = vintage(); p.presIndex = i; p.presDb = 4.8f; pres.push_back ({ "freq" + std::to_string (i), p }); }
    for (float q : { 0.3f, 0.6f, 1.0f, 2.0f, 4.0f, 6.0f })
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "presQ%.1f", q);
        Params p = vintage(); p.presIndex = 2; p.presDb = 4.8f; p.presQ = q; pres.push_back ({ nm, p });
    }
    writeSet ("presence", pres);

    std::vector<std::pair<std::string, Params>> tilt;
    for (int i = -5; i <= 5; ++i)
    {
        char nm[32];
        std::snprintf (nm, sizeof nm, "tilt%+.1f", 0.8 * i);
        Params p = vintage(); p.mode = Mode::A_Tilt; p.tiltDb = 0.8f * (float) i; tilt.push_back ({ nm, p });
    }
    writeSet ("tilt", tilt);
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
    testNeutralIsBitExact();
    testAnalyticMatchesProcessing();
    testSpecPoints();
    testQBehaviour();
    testSampleRateConsistency();
    testClicks();
    testStress();
    testBlockSizeIndependence();
    testTube();

    std::printf ("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
