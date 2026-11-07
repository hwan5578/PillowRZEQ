// =============================================================================
//  Pillow RZ EQ — DSP 코어 v0.5 (JUCE 비의존, C++17 헤더 온리)
//
//  자유 밴드 EQ (최대 16개) + Klangfilm RZ062 계열 색채:
//   - 밴드마다 모양(벨 · 로우/하이 셸프 · 로우/하이 컷 · 노치 · 틸트), 주파수, 게인, Q,
//     컷 기울기(6~96 dB/oct), 적용 채널(스테레오 · L · R · Mid · Side)
//   - 튜브: Telefunken EF804S(3극관 결선) Koren 모델로 계산한 실제 전달곡선
//           Drive(0~48 dB, 기준 레벨 보상), Focus(홀수 <-> 짝수 배음), Mix
//   - 출력 트랜스포머 색채: 저역 / 중역 / 고역 따로 조절
//
//  처리 순서:
//    [L/R 밴드] -> [M/S 인코드 -> Mid/Side 밴드 -> 디코드] -> EF804S -> 트랜스포머 -> Mix(원음) -> Output
//  ZERO LATENCY 모드는 세션 레이트에서 그대로(비선형은 ADAA), HQ 모드는 오버샘플된 레이트에서 처리
//
//  필터는 Cytomic(Andrew Simper) 선형 사다리꼴 SVF. 벨·셸프는 나이퀴스트 근처까지 아날로그를 따르는
//  매칭 설계, 컷은 버터워스 계단. GUI 응답 곡선은 실제 처리와 같은 계수로 계산 (ResponseModel).
//  v0.4 이하의 모듈 설정(Treble · Pivot/Presence · Bass)은 같은 곡선의 밴드로 변환 (migrateLegacyBands).
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace pillow::rz
{
inline constexpr double kPi = 3.14159265358979323846;

//==============================================================================
//  EQ 상수 (Audified·NEOLD 매뉴얼, GroupDIY 실측; 추정치는 README에 표시)
//==============================================================================
// Bass / Treble 주파수 = "기준 주파수"(셸프가 거의 다 올라오는 곳). 셸프 중간점은 기준에서 2옥타브 안쪽
//  원본: Bass 40 Hz 기준 -> 중간점 160 Hz, Treble 10 kHz 기준 -> 중간점 2.5 kHz (650 Hz 축 대칭, 추정)
inline constexpr double kBassRefHz     = 40.0;
inline constexpr double kTrebleRefHz   = 10000.0;
inline constexpr double kShelfRefToMid = 4.0;      // 중간점 = Bass 기준 x 4, Treble 기준 / 4
inline constexpr double kBassShelfHz   = kBassRefHz * kShelfRefToMid;     // 160 Hz (기본값일 때)
inline constexpr double kTrebleShelfHz = kTrebleRefHz / kShelfRefToMid;   // 2.5 kHz (기본값일 때)
inline constexpr double kTiltPivotHz   = 650.0;

// v0.4: 주파수 단계 (노브 눈금 · VINTAGE STEPS일 때 붙는 위치). 사이 값도 자유롭게 쓸 수 있음
inline constexpr std::array<double, 6> kBassSteps     { 20.0, 30.0, 40.0, 60.0, 80.0, 120.0 };
inline constexpr std::array<double, 5> kTrebleSteps   { 5000.0, 7000.0, 10000.0, 12000.0, 16000.0 };
inline constexpr std::array<double, 8> kPresenceSteps { 700.0, 1000.0, 1400.0, 2000.0, 2800.0, 4000.0, 5600.0, 8000.0 };   // 반 옥타브
inline constexpr std::array<double, 4> kPresenceHz    { 1400.0, 2000.0, 2800.0, 4000.0 };   // 원본 4단계 (v0.3 이하 세션 변환용)
inline constexpr double kPresenceDefaultHz = 2800.0;

inline constexpr float kBassMinDb   = -20.0f, kBassMaxDb   = 12.0f;
inline constexpr float kTrebleMinDb = -18.0f, kTrebleMaxDb = 12.0f;
inline constexpr float kTiltMaxDb   = 4.0f;    // A 모듈 Pivot +-4 dB
inline constexpr float kPresMaxDb   = 6.0f;    // B 모듈 Presence: NEOLD 기준 1 dB x 6 (실물 측정 자료는 0.8 dB x 6)
inline constexpr float kShelfStepDb = 2.0f;    // 원본 2 dB 스텝
inline constexpr float kMidStepDb   = 1.0f;    // Pivot / Presence 스텝

inline constexpr float kShelfQMin = 0.3f, kShelfQMax = 2.0f, kVintageShelfQ = 0.5f;
inline constexpr float kPresQMin  = 0.3f, kPresQMax  = 6.0f, kVintagePresQ  = 1.0f;

inline constexpr float kDriveMaxDb = 48.0f;    // NEOLD: 최대 48 dB 보상 드라이브

//==============================================================================
//  작은 헬퍼
//==============================================================================
// 부동소수 "정확히 같음" 비교 (-Wfloat-equal 경고 없이). 변경 감지용.
inline bool sameValue (double a, double b) noexcept { return ! (a < b) && ! (b < a); }
inline float cleanZero (float v) noexcept { return std::abs (v) < 1.0e-4f ? 0.0f : v; }
inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
inline double clamp01 (double v) noexcept { return std::clamp (v, 0.0, 1.0); }

//==============================================================================
//  원본 스텝(디텐트) 양자화
//==============================================================================
inline float quantizeTreble (float db) noexcept
{
    const float q = std::round (db / kShelfStepDb) * kShelfStepDb;
    return cleanZero (std::clamp (q, kTrebleMinDb, kTrebleMaxDb));
}

// 원본 베이스 노브는 컷 방향 첫 칸이 -4 dB, 그 뒤로 2 dB 간격 (+12 ... +2, 0, -4 ... -20)
inline float quantizeBass (float db) noexcept
{
    float q = std::round (db / kShelfStepDb) * kShelfStepDb;
    q = (db >= -2.0f) ? std::max (q, 0.0f) : std::min (q, -4.0f);
    return cleanZero (std::clamp (q, kBassMinDb, kBassMaxDb));
}

inline float quantizeTilt (float db) noexcept
{
    return cleanZero (std::clamp (std::round (db / kMidStepDb) * kMidStepDb, -kTiltMaxDb, kTiltMaxDb));
}

inline float quantizePresence (float db) noexcept
{
    return cleanZero (std::clamp (std::round (db / kMidStepDb) * kMidStepDb, 0.0f, kPresMaxDb));
}

// 로그(옥타브) 거리로 가장 가까운 단계
template <size_t N>
inline double nearestStep (const std::array<double, N>& steps, double hz) noexcept
{
    double best = steps[0], bestDist = 1.0e9;
    for (double st : steps)
    {
        const double d = std::abs (std::log (std::max (hz, 1.0) / st));
        if (d < bestDist) { bestDist = d; best = st; }
    }
    return best;
}

template <size_t N>
inline double clampToSteps (const std::array<double, N>& steps, double hz) noexcept
{
    return std::clamp (hz, steps.front(), steps.back());
}

//==============================================================================
//  자유 밴드 (v0.5)
//==============================================================================
inline constexpr int kMaxBands = 16;

enum class BandType  { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, Tilt };
inline constexpr int kNumBandTypes = 7;

enum class Placement { Stereo = 0, Left, Right, Mid, Side };
inline constexpr int kNumPlacements = 5;

inline constexpr std::array<int, 8> kCutSlopes { 6, 12, 18, 24, 36, 48, 72, 96 };   // dB/oct
inline constexpr int   kDefaultSlope = 1;                                            // 12 dB/oct
inline constexpr float kFreqMin = 10.0f, kFreqMax = 30000.0f;
inline constexpr float kGainMax = 24.0f;
inline constexpr float kQMin = 0.1f, kQMax = 24.0f;
inline constexpr double kButterworthQ = 0.70710678118654752440;   // 셸프·컷은 Q 1 = 출렁임 없는 기본 모양

inline constexpr bool typeHasGain (BandType t) noexcept
{
    return t == BandType::Bell || t == BandType::LowShelf || t == BandType::HighShelf || t == BandType::Tilt;
}
inline constexpr bool typeHasSlope (BandType t) noexcept { return t == BandType::LowCut || t == BandType::HighCut; }
inline constexpr bool typeHasQ (BandType t) noexcept { return t != BandType::Tilt; }
inline constexpr bool isMsPlacement (Placement p) noexcept { return p == Placement::Mid || p == Placement::Side; }

struct BandParams
{
    bool      used  = false;            // 밴드가 있음 (그래프에 점이 보임)
    bool      on    = true;             // 켜짐 (꺼도 설정은 남음)
    BandType  type  = BandType::Bell;
    float     freq  = 1000.0f;          // Hz
    float     gain  = 0.0f;             // dB (벨 · 셸프 · 틸트)
    float     q     = 1.0f;
    int       slope = kDefaultSlope;    // kCutSlopes 인덱스 (컷)
    Placement place = Placement::Stereo;

    bool isActive() const noexcept { return used && on; }

    bool operator== (const BandParams& o) const noexcept
    {
        return used == o.used && on == o.on && type == o.type && slope == o.slope && place == o.place
            && sameValue (freq, o.freq) && sameValue (gain, o.gain) && sameValue (q, o.q);
    }
    bool operator!= (const BandParams& o) const noexcept { return ! (*this == o); }
};

struct Params
{
    bool  eqOn    = true;
    bool  driveOn = false;
    float drive   = 0.0f;     // EF804S 드라이브 0..48 dB
    float focus   = 0.0f;     // -1 = 홀수 배음(대칭)  0 = EF804S 그대로  +1 = 짝수 배음 2배
    float mix     = 1.0f;     // 0..1 (원음 블렌드)
    float xfLow = 0.0f, xfMid = 0.0f, xfHigh = 0.0f;   // 트랜스포머 색채 0..1 (구간별)
    float outputDb = 0.0f;
    std::array<BandParams, kMaxBands> bands {};

    bool operator== (const Params& o) const noexcept
    {
        return eqOn == o.eqOn && driveOn == o.driveOn
            && sameValue (drive, o.drive) && sameValue (focus, o.focus) && sameValue (mix, o.mix)
            && sameValue (xfLow, o.xfLow) && sameValue (xfMid, o.xfMid) && sameValue (xfHigh, o.xfHigh)
            && sameValue (outputDb, o.outputDb) && bands == o.bands;
    }
    bool operator!= (const Params& o) const noexcept { return ! (*this == o); }
};

//==============================================================================
//  v0.4 이하 세션의 모듈 설정 (변환용)
//==============================================================================
struct LegacyModule
{
    bool  modeB    = true;            // false = A (Pivot), true = B (Presence)
    float trebleDb = 0.0f, trebleQ = kVintageShelfQ, trebleHz = (float) kTrebleRefHz;
    float bassDb   = 0.0f, bassQ   = kVintageShelfQ, bassHz   = (float) kBassRefHz;
    float tiltDb   = 0.0f;
    float presHz   = (float) kPresenceDefaultHz, presDb = 0.0f, presQ = kVintagePresQ;
};

struct LegacyParams
{
    bool link = true, midSide = false, stepped = false;
    std::array<LegacyModule, 2> mod {};
};

//==============================================================================
//  Simper SVF (선형 사다리꼴):  출력 = m0*v0 + m1*v1(band) + m2*v2(low)
//==============================================================================
struct SvfCoeffs
{
    double g = 0.0, k = 1.0, a1 = 1.0, a2 = 0.0, a3 = 0.0;
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

struct SvfState { double ic1 = 0.0, ic2 = 0.0; };

inline double clampFreq (double f, double fs) noexcept { return std::clamp (f, 1.0, 0.45 * fs); }

inline SvfCoeffs finishSvf (SvfCoeffs c) noexcept
{
    c.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
    c.a2 = c.g * c.a1;
    c.a3 = c.g * c.a2;
    return c;
}

inline SvfCoeffs makeBell (double fc, double q, double gainDb, double fs) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs);
    c.k  = 1.0 / (q * A);
    c.m0 = 1.0;
    c.m1 = c.k * (A * A - 1.0);
    c.m2 = 0.0;
    return finishSvf (c);
}

inline SvfCoeffs makeLowShelf (double fc, double q, double gainDb, double fs) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs) / std::sqrt (A);
    c.k  = 1.0 / q;
    c.m0 = 1.0;
    c.m1 = c.k * (A - 1.0);
    c.m2 = A * A - 1.0;
    return finishSvf (c);
}

inline SvfCoeffs makeHighShelf (double fc, double q, double gainDb, double fs) noexcept
{
    const double A = std::pow (10.0, gainDb / 40.0);
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs) * std::sqrt (A);
    c.k  = 1.0 / q;
    c.m0 = A * A;
    c.m1 = c.k * (1.0 - A) * A;
    c.m2 = 1.0 - A * A;
    return finishSvf (c);
}

inline SvfCoeffs makeLowpass (double fc, double q, double fs) noexcept
{
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs);
    c.k  = 1.0 / q;
    c.m0 = 0.0; c.m1 = 0.0; c.m2 = 1.0;
    return finishSvf (c);
}

inline SvfCoeffs makeHighpass (double fc, double q, double fs) noexcept
{
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs);
    c.k  = 1.0 / q;
    c.m0 = 1.0; c.m1 = -c.k; c.m2 = -1.0;
    return finishSvf (c);
}

inline double svfTick (const SvfCoeffs& c, SvfState& s, double v0) noexcept
{
    const double v3 = v0 - s.ic2;
    const double v1 = c.a1 * s.ic1 + c.a2 * v3;
    const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

// 디지털 SVF의 정확한 주파수 응답 (쌍선형: s = j*tan(pi f/fs)/g)
inline std::complex<double> svfResponse (const SvfCoeffs& c, double f, double fs) noexcept
{
    const double t = std::tan (kPi * std::min (f, 0.4999 * fs) / fs);
    const std::complex<double> s (0.0, t / c.g);
    const std::complex<double> den = s * s + c.k * s + 1.0;
    return (c.m0 * den + c.m1 * s + c.m2) / den;
}

//==============================================================================
//  나이퀴스트 근처에서도 아날로그 곡선을 따르는 설계 ("매칭" 설계, Vicanek 2016)
//   쌍선형 변환은 무한대 주파수를 나이퀴스트로 접어 넣어서, 세션 레이트(44.1/48k)에서는
//   고역 쪽 곡선이 찌그러짐 (넓은 Presence는 20 kHz 근처에서 1.8 dB까지). 오버샘플링 없이도 곡선을 지키려고:
//   - 극점: 아날로그 극점을 z = e^{sT}로 그대로 옮김 (임펄스 불변)
//   - 분자: 크기 응답을 DC · 기준 주파수 · 나이퀴스트 세 점에서 아날로그와 똑같이 맞춘 최소 위상 분자
//           (기준 주파수: 벨은 중심, 셸프는 영점 주파수 -> 높은 Q 셸프의 굴곡까지 맞음)
//   만든 바이쿼드는 역-쌍선형으로 SVF 계수(g, k, m0, m1, m2)로 바꿔서 기존 SVF 그대로 처리 (변조에 강함)
//   아날로그 대비 최대 오차 (20 Hz~20 kHz, 44.1 kHz): 벨 1.86 -> 0.38 dB, 셸프 1.23 -> 0.30 dB
//==============================================================================

// 아날로그 2차 근 (고유 주파수 w0 x T, 감쇠비 zeta) -> 디지털 1 + c1 z^-1 + c2 z^-2
inline void mapRoots (double w0T, double zeta, double& c1, double& c2) noexcept
{
    const double e = std::exp (-zeta * w0T);
    c2 = e * e;
    c1 = zeta < 1.0 ? -2.0 * e * std::cos (w0T * std::sqrt (1.0 - zeta * zeta))
                    : -2.0 * e * std::cosh (w0T * std::sqrt (zeta * zeta - 1.0));
}

// (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) -> SVF 계수 (z = (1 + u) / (1 - u), u = g s 대입)
inline SvfCoeffs svfFromBiquad (double b0, double b1, double b2, double a1, double a2) noexcept
{
    const double d0 = 1.0 + a1 + a2, d2 = 1.0 - a1 + a2;
    SvfCoeffs c;
    c.g = std::sqrt (d0 / d2);
    c.k = 2.0 * (1.0 - a2) / d0 * c.g;
    const double n2 = (b0 - b1 + b2) / d0, n1 = 2.0 * (b0 - b2) / d0, n0 = (b0 + b1 + b2) / d0;
    c.m0 = n2 * c.g * c.g;
    c.m1 = n1 * c.g - c.m0 * c.k;
    c.m2 = n0 - c.m0;
    return finishSvf (c);
}

// 극점(a1, a2)이 정해진 상태에서 |H|^2 를 DC = 1, 나이퀴스트 = hN, 각주파수 wm(rad/샘플) = hm 으로 맞춤
//   |H(e^jw)|^2 = (B0 p0 + B1 p1 + B2 p2) / (A0 p0 + A1 p1 + A2 p2),  p1 = sin^2(w/2), p0 = 1 - p1, p2 = 4 p0 p1
inline SvfCoeffs matchedSvf (double a1, double a2, double hN, double wm, double hm) noexcept
{
    const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
    const double p1 = std::sin (0.5 * wm) * std::sin (0.5 * wm), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
    const double B0 = A0, B1 = hN * A1;
    const double B2 = (hm * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
    const double s0 = std::sqrt (B0), s1 = std::sqrt (B1), W = 0.5 * (s0 + s1);
    const double b0 = 0.5 * (W + std::sqrt (std::max (W * W + B2, 0.0)));
    return svfFromBiquad (b0, 0.5 * (s0 - s1), -B2 / (4.0 * b0), a1, a2);
}

// 정확히 통과하는 계수 (극점은 그대로 -> 게인이 0을 지나가도 상태가 이어짐)
inline SvfCoeffs identitySvf (double a1, double a2) noexcept
{
    SvfCoeffs c = svfFromBiquad (1.0, a1, a2, a1, a2);
    c.m0 = 1.0; c.m1 = 0.0; c.m2 = 0.0;
    return c;
}

// 매칭 설계가 무리하는 경우(나이퀴스트 근처의 깊은 컷, 날카로운 셸프 등)를 잡으려고
// 아날로그 곡선과 여러 점에서 비교해서 더 가까운 설계를 고름 (보통은 매칭이 선택됨)
//   탐침: 넓은 간격 몇 개 + 공진(극점 · 영점) 둘레를 그 대역폭에 맞춰 촘촘히
struct ProbeSet
{
    std::array<double, 40> f {};
    int n = 0;

    void add (double v) noexcept { if (n < (int) f.size()) f[(size_t) n++] = v; }

    void addBroad (double fc, double fs) noexcept
    {
        for (double r : { 0.25, 0.5, 0.7071, 1.4142, 2.0 }) add (fc * r);
        for (double r : { 0.2, 0.3, 0.4 })                  add (fs * r);
    }

    void addResonance (double fr, double zeta) noexcept
    {
        const double z = std::min (zeta, 0.5);
        for (double m : { -3.0, -2.0, -1.5, -1.0, -0.5, -0.25, 0.0, 0.25, 0.5, 1.0, 1.5, 2.0, 3.0 })
            add (fr * (1.0 + m * z));
    }
};

// t = tan(pi f / fs) 를 미리 구한 SVF 응답
inline std::complex<double> svfResponseT (const SvfCoeffs& c, double t) noexcept
{
    const std::complex<double> s (0.0, t / c.g);
    const std::complex<double> den = s * s + c.k * s + 1.0;
    return (c.m0 * den + c.m1 * s + c.m2) / den;
}

// analogMag2(W): 아날로그 |H|^2 (W = f / fc). 오차는 파워 비율로 비교 (로그 없이)
template <typename AnalogMag2>
inline SvfCoeffs pickCloser (const SvfCoeffs& matched, const SvfCoeffs& bilinear, AnalogMag2 analogMag2,
                             double fc, const ProbeSet& probes, double fs) noexcept
{
    double em = 1.0, eb = 1.0;
    for (int i = 0; i < probes.n; ++i)
    {
        const double f = probes.f[(size_t) i];
        if (! (f > 1.0 && f < 0.45 * fs))
            continue;
        const double a = std::max (analogMag2 (f / fc), 1.0e-18);
        const double t = std::tan (kPi * f / fs);
        const double rm = std::max (std::norm (svfResponseT (matched,  t)), 1.0e-18) / a;
        const double rb = std::max (std::norm (svfResponseT (bilinear, t)), 1.0e-18) / a;
        em = std::max (em, rm >= 1.0 ? rm : 1.0 / rm);
        eb = std::max (eb, rb >= 1.0 ? rb : 1.0 / rb);
    }
    return em <= eb * 1.0232929922807541 ? matched : bilinear;   // 0.1 dB까지는 매칭 쪽
}

// 벨 (makeBell과 같은 아날로그 원형: (s^2 + s A/Q + 1) / (s^2 + s /(Q A) + 1))
inline SvfCoeffs makeBellMatched (double fc, double q, double gainDb, double fs) noexcept
{
    fc = clampFreq (fc, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0T = 2.0 * kPi * fc / fs;
    double a1, a2;
    mapRoots (w0T, 1.0 / (2.0 * q * A), a1, a2);
    if (sameValue (gainDb, 0.0)) return identitySvf (a1, a2);   // EQ off = 비트 단위 동일

    auto mag2 = [&] (double W)   // 아날로그 |H(jW)|^2, W = f / fc
    {
        const double re = 1.0 - W * W, nu = A * W / q, de = W / (q * A);
        return (re * re + nu * nu) / (re * re + de * de);
    };
    const auto matched = matchedSvf (a1, a2, mag2 (0.5 * fs / fc), w0T, A * A * A * A);
    ProbeSet p;
    p.addBroad (fc, fs);
    p.addResonance (fc, std::min (1.0 / (2.0 * q * A), A / (2.0 * q)));   // 극점과 영점 중 날카로운 쪽
    return pickCloser (matched, makeBell (fc, q, gainDb, fs), mag2, fc, p, fs);
}

// 하이 셸프 (makeHighShelf와 같은 원형: A (A s^2 + s sqrt(A)/Q + 1) / (s^2 + s sqrt(A)/Q + A))
inline SvfCoeffs makeHighShelfMatched (double fc, double q, double gainDb, double fs) noexcept
{
    fc = clampFreq (fc, fs);
    const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
    const double wcT = 2.0 * kPi * fc / fs;
    if (std::max (wcT * sA, wcT / sA) > 0.9 * kPi   // 근이 나이퀴스트 근처/위
        || q > 8.0)                                  // 아주 날카로운 셸프: 세 점 맞춤으로는 굴곡 모양을 못 따라감
        return makeHighShelf (fc, q, gainDb, fs);
    double a1, a2;
    mapRoots (wcT * sA, 0.5 / q, a1, a2);             // 극점: wc sqrt(A)
    if (sameValue (gainDb, 0.0)) return identitySvf (a1, a2);

    auto mag2 = [&] (double W)                         // 아날로그 |H(jW)|^2, W = f / fc
    {
        const double r = sA * W / q, n = 1.0 - A * W * W, d = A - W * W;
        return A * A * (n * n + r * r) / (d * d + r * r);
    };
    const double Wz = 1.0 / sA;                         // 영점 주파수 (셸프 굴곡이 있는 곳)
    const auto matched = matchedSvf (a1, a2, mag2 (0.5 * fs / fc), wcT * Wz, mag2 (Wz));
    ProbeSet p;
    p.addBroad (fc, fs);
    p.addResonance (fc * sA, 0.5 / q);
    p.addResonance (fc / sA, 0.5 / q);
    return pickCloser (matched, makeHighShelf (fc, q, gainDb, fs), mag2, fc, p, fs);
}

// 로우 셸프 = 반대 게인의 하이 셸프 x 전체 게인 (아날로그 원형에서 정확히 같음: LS(s; A) = A^2 HS(s; 1/A))
inline SvfCoeffs makeLowShelfMatched (double fc, double q, double gainDb, double fs) noexcept
{
    SvfCoeffs c = makeHighShelfMatched (fc, q, -gainDb, fs);
    if (sameValue (gainDb, 0.0))
        return c;
    const double k = dbToGain (gainDb);
    c.m0 *= k;  c.m1 *= k;  c.m2 *= k;
    return c;
}

// 노치: (s^2 + 1) / (s^2 + s/Q + 1)
inline SvfCoeffs makeNotch (double fc, double q, double fs) noexcept
{
    SvfCoeffs c;
    c.g  = std::tan (kPi * clampFreq (fc, fs) / fs);
    c.k  = 1.0 / q;
    c.m0 = 1.0; c.m1 = -c.k; c.m2 = 0.0;
    return finishSvf (c);
}

// 버터워스 n차를 2차 구간으로 나눴을 때 k번째(0부터, Q가 작은 것부터) 구간의 Q. 홀수 차수는 1차 하나가 따로
inline double butterworthSectionQ (int order, int k) noexcept
{
    const double theta = (order % 2 == 0) ? kPi * (2.0 * k + 1.0) / (2.0 * order)
                                          : kPi * (k + 1.0) / (double) order;
    return 1.0 / (2.0 * std::cos (theta));
}

//==============================================================================
//  틸트 (원본 A 모듈 Pivot): 1차 셸프를 축 주파수 중심으로 회전. 게인 +g = 고역 +g, 저역 -g
//==============================================================================
struct TiltCoeffs { double g = 0.0, G1 = 0.0, lfMinus1 = 0.0, outGain = 1.0; };
struct OnePoleState { double s = 0.0; };

inline TiltCoeffs makeTilt (double pivotHz, double tiltDb, double fs) noexcept
{
    const double G  = std::pow (10.0, -2.0 * tiltDb / 20.0);   // 고역 대비 저역 게인
    const double fp = pivotHz / std::sqrt (G);
    TiltCoeffs c;
    c.g        = std::tan (kPi * clampFreq (fp, fs) / fs);
    c.G1       = c.g / (1.0 + c.g);
    c.lfMinus1 = G - 1.0;
    c.outGain  = std::pow (10.0, tiltDb / 20.0);
    return c;
}

inline double tiltTick (const TiltCoeffs& c, OnePoleState& st, double x) noexcept
{
    const double v  = (x - st.s) * c.G1;
    const double lp = v + st.s;
    st.s = lp + v;
    return c.outGain * (x + c.lfMinus1 * lp);
}

inline std::complex<double> tiltResponse (const TiltCoeffs& c, double f, double fs) noexcept
{
    const double t = std::tan (kPi * std::min (f, 0.4999 * fs) / fs);
    const std::complex<double> lp = 1.0 / (1.0 + std::complex<double> (0.0, t / c.g));
    return c.outGain * (1.0 + c.lfMinus1 * lp);
}

//==============================================================================
//  1폴 스무더
//==============================================================================
struct Smoother
{
    double current = 0.0, target = 0.0, coeff = 1.0, eps = 1.0e-6;

    void setTimeConstant (double seconds, double updateRate) noexcept
    {
        coeff = (seconds <= 0.0 || updateRate <= 0.0) ? 1.0 : 1.0 - std::exp (-1.0 / (seconds * updateRate));
    }
    void snap() noexcept { current = target; }
    bool isSettled() const noexcept { return sameValue (current, target); }
    double step() noexcept
    {
        current += (target - current) * coeff;
        if (std::abs (target - current) <= eps)
            current = target;
        return current;
    }
};

//==============================================================================
//  Telefunken EF804S (3극관 결선) 증폭단 — Koren 3극관 모델로 정적 전달곡선 계산
//
//   Ip = 2 * E1^Ex / Kg1,  E1 = (Vak/Kp) * ln(1 + exp(Kp * (1/mu + Vgk / sqrt(Kvb + Vak^2))))
//   mu = 38      : EF804S 데이터시트 3극관 결선 값
//   Ex, Kvb      : EF86(같은 계열) Koren 모델 기반
//   Kg1, Kp      : 데이터시트 "3극관 결선 RC 증폭 이득 31 (250 V, 100 kOhm)"에 맞춰 보정
//   회로         : B+ 290 V, 플레이트 부하 220 kOhm, 무바이패스 캐소드 저항 (유휴 0.5 mA)
//                  — GroupDIY의 RZ062 실물 추정값 기반. 그리드 전류는 0.6 V 부근 소프트 클램프
//   결과         : 이득 약 14배, 약하게 걸면 2차 배음, 세게 걸면 컷오프/그리드 전류로 3차 배음
//
//  테이블 2개 (asinh 워핑 격자, 3차 보간):
//   dev(v) = T(v) - v          정규화 전달곡선의 "직선에서 벗어난 부분"
//   psi(v) = 적분 0..v dev(u)   그 역도함수 (ADAA 안티에일리어싱용)
//==============================================================================
class Ef804sCurve
{
public:
    static constexpr double kMu = 38.0, kEx = 1.3, kKp = 900.0, kKvb = 300.0, kKg1 = 1420.0;
    static constexpr double kBplus = 290.0, kRl = 220.0e3, kIdleA = 0.5e-3, kGridClampV = 0.6;
    static constexpr int    kSize = 4097;              // 홀수 -> 0 V가 정확히 격자점
    static constexpr double kV0 = 0.05, kVmax = 1500.0; // asinh 워핑: 작은 신호 쪽을 촘촘하게

    Ef804sCurve() { build(); }

    // 정규화 전달곡선 T(v): v = 그리드 신호 전압(V), T(0) = 0, T'(0) = 1, 위상 반전 보정됨
    double operator() (double v) const noexcept { return v + dev (v); }

    // T(v) - v. 표의 보간 구간 밖(약 ±1484 V 바깥)은 포화(평탄)로 연장
    //  (비교를 이렇게 써야 NaN이 표 조회로 들어가지 않음)
    double dev (double v) const noexcept
    {
        if (v >= -vEdge && v <= vEdge) return lookup (devTable, v);
        return v > 0.0 ? tEdgePos - v : tEdgeNeg - v;
    }

    // 적분 0..v (T(u) - u) du. 표 밖은 평탄 곡선의 적분으로 연장 (경계에서 값이 이어짐)
    double psi (double v) const noexcept
    {
        if (v >= -vEdge && v <= vEdge) return lookup (psiTable, v);
        return v > 0.0 ? psiEdgePos + tEdgePos * (v - vEdge) - 0.5 * (v * v - vEdge * vEdge)
                       : psiEdgeNeg + tEdgeNeg * (v + vEdge) - 0.5 * (v * v - vEdge * vEdge);
    }

    // dev(v), dev(-v), psi(v), psi(-v)를 한 번에 (오디오 루프용: asinh 1번)
    void evalPair (double v, double& devPos, double& devNeg, double& psiPos, double& psiNeg) const noexcept
    {
        if (! (std::abs (v) <= vEdge))   // 표 밖 (NaN 포함)
        {
            devPos = dev (v);  devNeg = dev (-v);
            psiPos = psi (v);  psiNeg = psi (-v);
            return;
        }
        const double w = std::asinh (v / kV0);
        const Point pp = locate (w), pn = locate (-w);
        devPos = interp (devTable, pp);  devNeg = interp (devTable, pn);
        psiPos = interp (psiTable, pp);  psiNeg = interp (psiTable, pn);
    }

    // 회로 그대로 풀기 (테이블 생성·테스트용)
    static double plateCurrent (double vak, double vgk) noexcept
    {
        if (vak <= 0.0) return 0.0;
        const double z  = kKp * (1.0 / kMu + vgk / std::sqrt (kKvb + vak * vak));
        const double sp = z > 30.0 ? z : std::log1p (std::exp (z));
        const double e1 = vak / kKp * sp;
        return e1 > 0.0 ? 2.0 * std::pow (e1, kEx) / kKg1 : 0.0;
    }

    static double gridClamp (double vgk) noexcept
    {
        return vgk <= 0.0 ? vgk : vgk / (1.0 + vgk / kGridClampV);
    }

    // 무바이패스 캐소드 증폭단의 애노드 전류 (입력 = 그리드 신호 전압)
    static double stageCurrent (double vin, double rk) noexcept
    {
        double lo = 0.0, hi = kBplus / (kRl + rk);
        for (int it = 0; it < 64; ++it)
        {
            const double ia = 0.5 * (lo + hi);
            const double g  = ia - plateCurrent (kBplus - ia * (kRl + rk), gridClamp (vin - ia * rk));
            (g > 0.0 ? hi : lo) = ia;
        }
        return 0.5 * (lo + hi);
    }

    // 회로를 직접 풀어서 T(v) - v
    double solveDev (double v) const noexcept
    {
        const double vp = kBplus - stageCurrent (v, rk) * kRl;
        return (vp0 - vp) / gain - v;
    }

    double cathodeResistor() const noexcept { return rk; }
    double idlePlateVoltage() const noexcept { return vp0; }
    double stageGain() const noexcept { return gain; }

private:
    using Table = std::array<double, kSize>;

    struct Point { size_t i; double f; };

    Point locate (double w) const noexcept
    {
        const double pos = std::clamp ((w + wMax) * invDw, 1.0, (double) kSize - 3.0);
        const auto i = (size_t) pos;
        return { i, pos - (double) i };
    }

    // Catmull-Rom
    static double interp (const Table& t, Point p) noexcept
    {
        const double p0 = t[p.i - 1], p1 = t[p.i], p2 = t[p.i + 1], p3 = t[p.i + 2], f = p.f;
        return p1 + 0.5 * f * (p2 - p0 + f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + f * (3.0 * (p1 - p2) + p3 - p0)));
    }

    double lookup (const Table& t, double v) const noexcept { return interp (t, locate (std::asinh (v / kV0))); }

    void build()
    {
        // 1) 유휴 전류 0.5 mA가 되는 캐소드 저항
        double lo = 100.0, hi = 100.0e3;
        for (int it = 0; it < 64; ++it)
        {
            rk = 0.5 * (lo + hi);
            (stageCurrent (0.0, rk) > kIdleA ? lo : hi) = rk;
        }
        rk  = 0.5 * (lo + hi);
        vp0 = kBplus - stageCurrent (0.0, rk) * kRl;

        // 2) 소신호 이득 (중앙 차분)
        const double d = 1.0e-4;
        gain = ((kBplus - stageCurrent (-d, rk) * kRl) - (kBplus - stageCurrent (d, rk) * kRl)) / (2.0 * d);

        // 3) 격자점과 그 사이 중점에서 dev(v)를 직접 계산
        wMax  = std::asinh (kVmax / kV0);
        const double dw = 2.0 * wMax / (double) (kSize - 1);
        invDw = 1.0 / dw;
        const int c = (kSize - 1) / 2;

        auto wAt = [&] (double k) { return -wMax + dw * k; };
        auto integrand = [&] (double w, double devValue) { return devValue * kV0 * std::cosh (w); };   // dev(v) dv/dw

        for (int k = 0; k < kSize; ++k)
            devTable[(size_t) k] = (k == c) ? 0.0 : solveDev (kV0 * std::sinh (wAt (k)));

        // 4) psi = dev의 적분 (심프슨, 0 V에서 양쪽으로)
        psiTable[(size_t) c] = 0.0;
        for (int k = c; k < kSize - 1; ++k)
        {
            const double wm = wAt (k + 0.5);
            const double fm = integrand (wm, solveDev (kV0 * std::sinh (wm)));
            psiTable[(size_t) k + 1] = psiTable[(size_t) k]
                + dw / 6.0 * (integrand (wAt (k), devTable[(size_t) k]) + 4.0 * fm + integrand (wAt (k + 1), devTable[(size_t) k + 1]));
        }
        for (int k = c; k > 0; --k)
        {
            const double wm = wAt (k - 0.5);
            const double fm = integrand (wm, solveDev (kV0 * std::sinh (wm)));
            psiTable[(size_t) k - 1] = psiTable[(size_t) k]
                - dw / 6.0 * (integrand (wAt (k - 1), devTable[(size_t) k - 1]) + 4.0 * fm + integrand (wAt (k), devTable[(size_t) k]));
        }

        // 4점 보간이 표 안에서만 이뤄지는 마지막 격자점 (양쪽 대칭: 2번, kSize-3번)
        vEdge      = kV0 * std::sinh (wAt ((double) kSize - 3.0));
        tEdgePos   =  vEdge + devTable[(size_t) kSize - 3];
        tEdgeNeg   = -vEdge + devTable[2];
        psiEdgePos = psiTable[(size_t) kSize - 3];
        psiEdgeNeg = psiTable[2];
    }

    Table devTable {}, psiTable {};
    double wMax = 1.0, invDw = 1.0, rk = 0.0, vp0 = 0.0, gain = 1.0;
    double vEdge = kVmax, tEdgePos = 0.0, tEdgeNeg = 0.0, psiEdgePos = 0.0, psiEdgeNeg = 0.0;
};

// 프로세스 전체에서 한 번만 계산 (prepare 때 먼저 불러서 오디오 스레드에서 생성되지 않게 함)
inline const Ef804sCurve& ef804sCurve()
{
    static const Ef804sCurve curve;
    return curve;
}

// log(cosh(z)) — 큰 값에서도 넘치지 않게
inline double logCosh (double z) noexcept
{
    const double a = std::abs (z);
    return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994530942;
}

//==============================================================================
//  1차 ADAA + 선형 성분 정렬 (신호 하나)
//   ADAA: 왜곡 성분 h(x)를 "샘플 사이를 직선으로 이은 구간의 평균"으로 바꿔 에일리어싱을 줄임
//         (H = h의 역도함수, y = (H(x[n]) - H(x[n-1])) / (x[n] - x[n-1])). 레이턴시 0.
//   문제: 그 대가로 y는 반 샘플 늦고 고역에서 조금 작아짐. 왜곡 성분 안에는 압축(입력과 같은 위상의
//         음수 성분)도 들어 있어서, 그대로 원신호에 더하면 고역에서 압축이 덜 빠짐
//         (무거운 드라이브에서 10 kHz가 수~10 dB 뜸, 큰 저음 위의 고역은 더 심함).
//   해결: y에서 입력과 선형으로 상관된 부분(G * avg2)을 빼고, 원래 곡선의 선형 성분(N * x)을 지연 없이 넣음.
//         N = 미분끼리의 회귀 기울기 (dh 대 dx), G = (dy 대 d avg2), 둘 다 10 ms 평균.
//         미분 신호로 재기 때문에 고역 성분 기준으로 맞춰짐 (보정이 필요한 곳이 고역).
//   결과(48 kHz): 사인 하나는 모든 주파수에서 정확한 곡선과 0.01 dB 이내, 큰 저음 위 작은 고역도 0.3 dB 이내,
//                에일리어싱 감소량은 ADAA 그대로 (보정 항은 천천히 변하는 선형 이득이라 새 배음을 만들지 않음).
//==============================================================================
class AdaaAligned
{
public:
    static constexpr double kEps      = 1.0e-6;   // 이보다 작은 변화는 양 끝 평균 (오차 ~ eps^2)
    static constexpr double kTiny     = 1.0e-12;  // 아주 작은 신호에서는 보정 0으로
    static constexpr double kMaxSlope = 4.0;      // 안전 한계 (정상 동작에서는 닿지 않음)
    static constexpr double kAvgSeconds  = 0.010;   // N, G 회귀 평균
    static constexpr double kSlowSeconds = 0.040;   // (트랜스포머) 상보 구간에 거는 N은 한 번 더 평균

    void prepare (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        a  = std::exp (-1.0 / (kAvgSeconds * sr));
        b  = 1.0 - a;
        as = std::exp (-1.0 / (kSlowSeconds * sr));
        reset();
    }

    void reset() noexcept
    {
        xPrev = hPrev = HPrev = avPrev = yPrev = 0.0;
        nNum = nDen = gNum = gDen = nSlow = 0.0;
    }

    double lastInput() const noexcept { return xPrev; }

    // 곡선이 바뀌었을 때: 직전 입력에서의 값을 새 곡선으로 다시
    void rebase (double hAtLast, double HAtLast) noexcept { hPrev = hAtLast; HPrev = HAtLast; }

    struct Split { double distortion, slope; };

    // x = 입력, h = h(x), H = H(x). 반환: 안티에일리어싱 + 정렬된 왜곡 성분 = distortion + slope * x
    double tick (double x, double h, double H) noexcept
    {
        const Split sp = tickSplit (x, h, H);
        return sp.distortion + sp.slope * x;
    }

    // 같은 것을 둘로 나눠서: distortion = 배음·비대칭 DC + 압축의 빠른 변화분, slope = 천천히 변하는 압축 기울기
    //  -> distortion은 DC 블로커를 거쳐도 되고, slope * x는 원신호에 위상 그대로 걸 수 있음
    Split tickSplit (double x, double h, double H) noexcept
    {
        const double dx = x - xPrev;
        const double y  = std::abs (dx) > kEps ? (H - HPrev) / dx : 0.5 * (h + hPrev);
        const double av = 0.5 * (x + xPrev);
        const double dav = av - avPrev;
        const double dh  = h - hPrev, dy = y - yPrev;

        nNum = a * nNum + b * dh * dx;   nDen = a * nDen + b * dx * dx;
        gNum = a * gNum + b * dy * dav;  gDen = a * gDen + b * dav * dav;
        const double n = std::clamp (nNum / (nDen + kTiny), -kMaxSlope, kMaxSlope);
        const double g = std::clamp (gNum / (gDen + kTiny), -kMaxSlope, kMaxSlope);

        nSlow = as * nSlow + (1.0 - as) * n;

        xPrev = x;  hPrev = h;  HPrev = H;  avPrev = av;  yPrev = y;
        return { y - g * av + (n - nSlow) * x, nSlow };
    }

private:
    double a = 0.99, b = 0.01, as = 0.999;
    double xPrev = 0.0, hPrev = 0.0, HPrev = 0.0, avPrev = 0.0, yPrev = 0.0;
    double nNum = 0.0, nDen = 0.0, gNum = 0.0, gDen = 0.0, nSlow = 0.0;
};

// 1차 ADAA만 (정렬 없음): 입력과 상관없는 성분(짝수 배음·DC)에 사용
class Adaa1
{
public:
    void reset() noexcept { xPrev = hPrev = HPrev = 0.0; }
    void rebase (double hAtLast, double HAtLast) noexcept { hPrev = hAtLast; HPrev = HAtLast; }

    double tick (double x, double h, double H) noexcept
    {
        const double dx = x - xPrev;
        const double y  = std::abs (dx) > AdaaAligned::kEps ? (H - HPrev) / dx : 0.5 * (h + hPrev);
        xPrev = x;  hPrev = h;  HPrev = H;
        return y;
    }

private:
    double xPrev = 0.0, hPrev = 0.0, HPrev = 0.0;
};

//==============================================================================
//  튜브 스테이지 (채널 1개)
//   - Drive: 그리드에 들어가는 신호 크기 (0 dB = 풀스케일이 그리드 0.5 Vpk, 거의 깨끗)
//   - Focus: 전달곡선의 홀수 성분 + (1 + focus) x 짝수 성분
//   - 보상: -12 dBFS 사인의 기본파 레벨이 드라이브와 상관없이 같도록 정규화
//   - 1차 ADAA: 오버샘플링 없이도 에일리어싱을 줄임 -> 레이턴시 0. 원신호는 그대로 두고 "왜곡 성분"에만 적용
//     홀수 성분(압축 + 홀수 배음): ADAA + 선형 성분 정렬, DC 블로커를 거치지 않음 (저역 위상·온셋 그대로)
//     짝수 성분(짝수 배음 + 비대칭 DC): ADAA -> DC 블로커 (5 Hz)
//   - Drive off면 완전 바이패스 (비트 단위 동일)
//==============================================================================
class TubeStage
{
public:
    static constexpr double kGridVoltsPerFullScale = 0.5;
    static constexpr double kRefAmp = 0.25;     // -12 dBFS

    TubeStage()
    {
        for (int k = 0; k < kDfPoints; ++k)
            sinTable[(size_t) k] = std::sin (2.0 * kPi * ((double) k + 0.5) / (double) kDfPoints);
    }

    void prepare (double sampleRate)
    {
        fs    = sampleRate > 0.0 ? sampleRate : 48000.0;
        dcR   = std::exp (-2.0 * kPi * 5.0 / fs);
        curve = &ef804sCurve();
        oddPart.prepare (fs);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s) { smoothingSeconds = std::max (0.0, s); applySmoothing(); }

    void setTargets (bool on, double driveDb, double focus) noexcept
    {
        wet.target   = on ? 1.0 : 0.0;
        drive.target = std::clamp (driveDb, 0.0, (double) kDriveMaxDb);
        foc.target   = std::clamp (focus, -1.0, 1.0);
    }

    void reset() noexcept
    {
        wet.snap(); drive.snap(); foc.snap();
        clearState();
        samplesToNextUpdate = 0;
        updateDerived();
    }

    void clearState() noexcept { dcX1 = dcY1 = 0.0; oddPart.reset(); evenPart.reset(); }

    void process (float* x, int n) noexcept
    {
        int i = 0;
        while (i < n)
        {
            if (samplesToNextUpdate <= 0)
            {
                if (! (wet.isSettled() && drive.isSettled() && foc.isSettled()))
                {
                    wet.step(); drive.step(); foc.step();
                    updateDerived();
                    double ho, Ho, he, He;         // 곡선이 바뀌었으니 직전 입력의 값도 새 곡선으로
                    shapeAt (oddPart.lastInput(), ho, Ho, he, He);
                    oddPart.rebase (ho, Ho);
                    evenPart.rebase (he, He);
                }
                samplesToNextUpdate = kChunk;
            }
            const int len = std::min (samplesToNextUpdate, n - i);

            if (wetNow > 0.0)
            {
                float* p = x + i;
                for (int k = 0; k < len; ++k)
                {
                    const double in = p[k];
                    double ho, Ho, he, He;
                    shapeAt (in, ho, Ho, he, He);

                    const double odd  = oddPart.tick (in, ho, Ho);     // 압축 + 홀수 배음 (정렬됨)
                    const double even = evenPart.tick (in, he, He);    // 짝수 배음 + DC
                    const double hp   = even - dcX1 + dcR * dcY1;      // DC 블로커 (5 Hz)
                    dcX1 = even;
                    dcY1 = hp;

                    // 정적 곡선이면 norm * (x + dev(x)) -> 원신호 기준 차이만 wet 만큼
                    p[k] = (float) (in + wetNow * (norm * (in + odd + hp) - in));
                }
            }
            else
            {
                clearState();
            }

            samplesToNextUpdate -= len;
            i += len;
        }
    }

    // 테스트용: 현재 설정의 정적 곡선 (보상 포함, 안티에일리어싱 없음)
    double transfer (double x) const noexcept { return (x + deviation (x)) * norm; }

private:
    static constexpr int kChunk = 32, kDfPoints = 32;

    // shape(x) - x  (x 영역, focus 반영)
    double deviation (double x) const noexcept
    {
        const double v = x * scale;
        const double ep = curve->dev (v), en = curve->dev (-v);
        return (0.5 * (ep - en) + evenMul * 0.5 * (ep + en)) * invScale;
    }

    // h(x) = shape(x) - x 의 홀수·짝수 성분과 각각의 역도함수 H(x) = 적분 0..x h(u) du 를 한 번에
    void shapeAt (double x, double& hOdd, double& HOdd, double& hEven, double& HEven) const noexcept
    {
        double dp, dn, pp, pn;
        curve->evalPair (x * scale, dp, dn, pp, pn);
        hOdd  = 0.5 * (dp - dn) * invScale;
        hEven = evenMul * 0.5 * (dp + dn) * invScale;
        HOdd  = 0.5 * (pp + pn) * invScale * invScale;
        HEven = evenMul * 0.5 * (pp - pn) * invScale * invScale;
    }

    void applySmoothing() noexcept
    {
        const double rate = fs / kChunk;
        wet.setTimeConstant (std::max (smoothingSeconds, 0.005), rate);  wet.eps = 1.0e-5;
        drive.setTimeConstant (smoothingSeconds, rate);                   drive.eps = 1.0e-4;
        foc.setTimeConstant (smoothingSeconds, rate);                     foc.eps = 1.0e-5;
    }

    void updateDerived() noexcept
    {
        wetNow   = wet.current;
        scale    = kGridVoltsPerFullScale * dbToGain (drive.current);
        invScale = 1.0 / scale;
        evenMul  = 1.0 + foc.current;

        // 기준 레벨(-12 dBFS)에서의 기본파 이득 (describing function)
        double sum = 0.0;
        for (int k = 0; k < kDfPoints; ++k)
        {
            const double xk = kRefAmp * sinTable[(size_t) k];
            sum += (xk + deviation (xk)) * sinTable[(size_t) k];
        }
        const double df = (2.0 / (double) kDfPoints) * sum / kRefAmp;
        norm = df > 1.0e-9 ? 1.0 / df : 1.0;
    }

    const Ef804sCurve* curve = &ef804sCurve();
    std::array<double, kDfPoints> sinTable {};
    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double wetNow = 0.0, scale = kGridVoltsPerFullScale, invScale = 1.0 / kGridVoltsPerFullScale;
    double evenMul = 1.0, norm = 1.0, dcX1 = 0.0, dcY1 = 0.0;
    AdaaAligned oddPart;
    Adaa1 evenPart;
    Smoother wet, drive, foc;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  출력 트랜스포머 색채 — 저역 / 중역 / 고역 따로
//   LR4 크로스오버(200 Hz, 4 kHz)로 나눈 각 구간에서 "왜곡 성분"만 만들어 원신호에 더함
//   -> 모든 양이 0이면 완전 바이패스, 크로스오버 위상이 원신호에 묻지 않음
//   배음은 LR4 구간에서 만들고(구간별로 깔끔하게), 압축(선형 성분)은 합이 원신호와 같은 1차 상보 구간에 걸어
//   크로스오버 근처에서도 항상 "눌리는" 방향으로만 동작
//   LOW : 코어 포화 (대칭, 3차 위주, 큰 저음일수록 진해짐)
//   MID : 은은한 밀도 (약한 비대칭, 2차+3차)
//   HIGH: 고역 포화 + 12 kHz 위를 최대 -1.5 dB 부드럽게 (누설 인덕턴스/권선 용량 느낌)
//   각 구간의 왜곡 성분은 1차 ADAA + 선형 성분 정렬로 안티에일리어싱 (오버샘플링 없이 레이턴시 0)
//==============================================================================
class TransformerStage
{
public:
    static constexpr double kXoverLowHz = 200.0, kXoverHighHz = 4000.0;
    static constexpr double kLowDrive = 1.4;
    static constexpr double kMidDrive = 0.45, kMidBias = 0.12;
    static constexpr double kHighDrive = 1.2;
    static constexpr double kShelfHz = 12000.0, kShelfQ = 0.5, kShelfMaxCutDb = 1.5;

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double q = 1.0 / std::sqrt (2.0);
        lpLo = makeLowpass  (kXoverLowHz,  q, fs);
        hpLo = makeHighpass (kXoverLowHz,  q, fs);
        lpHi = makeLowpass  (kXoverHighHz, q, fs);
        hpHi = makeHighpass (kXoverHighHz, q, fs);
        gSplitLo = onePoleGain (kXoverLowHz, fs);
        gSplitHi = onePoleGain (kXoverHighHz, fs);
        dcR  = std::exp (-2.0 * kPi * 5.0 / fs);
        for (auto& b : bands) b.prepare (fs);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s) { smoothingSeconds = std::max (0.0, s); applySmoothing(); }

    void setAmounts (double low, double mid, double high) noexcept
    {
        aLow.target  = clamp01 (low);
        aMid.target  = clamp01 (mid);
        aHigh.target = clamp01 (high);
    }

    void reset() noexcept
    {
        aLow.snap(); aMid.snap(); aHigh.snap();
        clearState();
        samplesToNextUpdate = 0;
        updateDerived();
    }

    void clearState() noexcept
    {
        for (auto& s : xs) s = SvfState {};
        shelfState = SvfState {};
        splitLo = splitHi = 0.0;
        dcX1 = dcY1 = 0.0;
        for (auto& b : bands) b.reset();
        midEven.reset();
    }

    void process (float* x, int n) noexcept
    {
        int i = 0;
        while (i < n)
        {
            if (samplesToNextUpdate <= 0)
            {
                if (! (aLow.isSettled() && aMid.isSettled() && aHigh.isSettled()))
                {
                    aLow.step(); aMid.step(); aHigh.step();
                    updateDerived();
                }
                samplesToNextUpdate = kChunk;
            }
            const int len = std::min (samplesToNextUpdate, n - i);

            if (active)
            {
                float* p = x + i;
                for (int k = 0; k < len; ++k)
                {
                    const double in  = p[k];
                    const double lo  = svfTick (lpLo, xs[1], svfTick (lpLo, xs[0], in));
                    const double hb  = svfTick (hpLo, xs[3], svfTick (hpLo, xs[2], in));
                    const double mid = svfTick (lpHi, xs[5], svfTick (lpHi, xs[4], hb));
                    const double hi  = svfTick (hpHi, xs[7], svfTick (hpHi, xs[6], hb));

                    // 1차 상보 분할: 세 구간의 합이 정확히 원신호 -> 압축(선형 성분)을 여기에 걸면 위상이 원신호와 맞음
                    // (LR4 구간에 그대로 걸면 크로스오버 근처에서 위상이 180도 돌아 압축이 오히려 확장이 됨)
                    const double c1   = onePoleLp (in, gSplitLo, splitLo);
                    const double rest = in - c1;
                    const double c2   = onePoleLp (rest, gSplitHi, splitHi);
                    const double c3   = rest - c2;

                    // LOW·HIGH 곡선은 홀수 함수 -> DC가 생기지 않음
                    const auto sl = bands[0].tickSplit (lo, hLow (lo),  HLow (lo));
                    const auto sh = bands[2].tickSplit (hi, hHigh (hi), HHigh (hi));

                    // MID는 비대칭 -> 홀수 성분(압축·홀수 배음)과 짝수 성분(짝수 배음·DC)으로 나눠서
                    const double mp = hMid (mid), mn = hMid (-mid), Mp = HMid (mid), Mn = HMid (-mid);
                    const auto   sm = bands[1].tickSplit (mid, 0.5 * (mp - mn), 0.5 * (Mp + Mn));
                    const double me = midEven.tick (mid, 0.5 * (mp + mn), 0.5 * (Mp - Mn));
                    const double hp = me - dcX1 + dcR * dcY1;     // DC 블로커는 짝수 성분에만
                    dcX1 = me;
                    dcY1 = hp;

                    const double r = curLow  * (sl.distortion + sl.slope * c1)
                                   + curMid  * (sm.distortion + sm.slope * c2 + hp)
                                   + curHigh * (sh.distortion + sh.slope * c3);

                    p[k] = (float) (svfTick (shelf, shelfState, in) + r);
                }
            }
            else
            {
                clearState();
            }

            samplesToNextUpdate -= len;
            i += len;
        }
    }

    // 구간별 왜곡 성분 h(u) = f(u) - u 와 그 역도함수 H(u) (H(0) = 0)
    static double hLow (double u) noexcept  { return std::tanh (kLowDrive * u) / kLowDrive - u; }
    static double HLow (double u) noexcept  { return logCosh (kLowDrive * u) / (kLowDrive * kLowDrive) - 0.5 * u * u; }
    static double hHigh (double u) noexcept { return std::tanh (kHighDrive * u) / kHighDrive - u; }
    static double HHigh (double u) noexcept { return logCosh (kHighDrive * u) / (kHighDrive * kHighDrive) - 0.5 * u * u; }
    static double hMid (double u) noexcept
    {
        const double tb = std::tanh (kMidBias);
        return (std::tanh (kMidDrive * u + kMidBias) - tb) / (kMidDrive * (1.0 - tb * tb)) - u;
    }
    static double HMid (double u) noexcept
    {
        const double tb = std::tanh (kMidBias);
        const double n  = 1.0 / (kMidDrive * (1.0 - tb * tb));
        return n * ((logCosh (kMidDrive * u + kMidBias) - logCosh (kMidBias)) / kMidDrive - tb * u) - 0.5 * u * u;
    }

private:
    static constexpr int kChunk = 32;

    static double onePoleGain (double fc, double sampleRate) noexcept
    {
        const double g = std::tan (kPi * std::min (fc, 0.45 * sampleRate) / sampleRate);
        return g / (1.0 + g);
    }

    // 1차 TPT 로우패스
    static double onePoleLp (double x, double G, double& state) noexcept
    {
        const double v = (x - state) * G;
        const double y = v + state;
        state = y + v;
        return y;
    }

    void applySmoothing() noexcept
    {
        const double rate = fs / kChunk;
        for (auto* s : { &aLow, &aMid, &aHigh })
        {
            s->setTimeConstant (std::max (smoothingSeconds, 0.005), rate);
            s->eps = 1.0e-5;
        }
    }

    void updateDerived() noexcept
    {
        curLow  = aLow.current;
        curMid  = aMid.current;
        curHigh = aHigh.current;
        active  = curLow > 0.0 || curMid > 0.0 || curHigh > 0.0;
        shelf   = makeHighShelfMatched (kShelfHz, kShelfQ, -kShelfMaxCutDb * curHigh, fs);
    }

    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double curLow = 0.0, curMid = 0.0, curHigh = 0.0;
    double gSplitLo = 0.0, gSplitHi = 0.0, splitLo = 0.0, splitHi = 0.0;
    double dcX1 = 0.0, dcY1 = 0.0;
    bool active = false;
    SvfCoeffs lpLo, hpLo, lpHi, hpHi, shelf;
    std::array<SvfState, 8> xs {};
    std::array<AdaaAligned, 3> bands {};
    Adaa1 midEven;
    SvfState shelfState;
    Smoother aLow, aMid, aHigh;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  밴드 설계: 모양 · 주파수 · 게인 · Q · 기울기 -> 2차 SVF 최대 8개 + 1차 하나 (또는 1차 틸트)
//==============================================================================
struct BandDesign
{
    int numSections = 0;
    std::array<SvfCoeffs, 8> sec {};
    int firstOrder = 0;          // +1 = 1차 로우패스, -1 = 1차 하이패스 (홀수 차수 컷)
    double foGw = 0.0;           // 1차: tan(pi f / fs)
    double foG  = 0.0;           // 1차 TPT: G = g / (1 + g)
    bool tilt = false;
    TiltCoeffs tiltC {};
};

inline BandDesign designBand (BandType type, double freq, double gainDb, double q, int slopeIndex, double fs) noexcept
{
    BandDesign d;
    const double f  = clampFreq (std::clamp (freq, (double) kFreqMin, (double) kFreqMax), fs);
    const double g  = std::clamp (gainDb, -(double) kGainMax, (double) kGainMax);
    const double qq = std::clamp (q, (double) kQMin, (double) kQMax);

    switch (type)
    {
        case BandType::Bell:      d.numSections = 1; d.sec[0] = makeBellMatched (f, qq, g, fs); break;
        case BandType::LowShelf:  d.numSections = 1; d.sec[0] = makeLowShelfMatched  (f, qq * kButterworthQ, g, fs); break;
        case BandType::HighShelf: d.numSections = 1; d.sec[0] = makeHighShelfMatched (f, qq * kButterworthQ, g, fs); break;
        case BandType::Notch:     d.numSections = 1; d.sec[0] = makeNotch (f, qq, fs); break;
        case BandType::Tilt:      d.tilt = true; d.tiltC = makeTilt (f, g, fs); break;
        case BandType::LowCut:
        case BandType::HighCut:
        {
            const bool hp = (type == BandType::LowCut);
            const int order = kCutSlopes[(size_t) std::clamp (slopeIndex, 0, (int) kCutSlopes.size() - 1)] / 6;
            const int n2 = order / 2;
            for (int k = 0; k < n2; ++k)
            {
                double sq = butterworthSectionQ (order, k);
                if (k == n2 - 1)
                    sq *= qq;                       // 가장 날카로운 구간에 Q를 곱함 (Q 1 = 순수 버터워스)
                d.sec[(size_t) k] = hp ? makeHighpass (f, sq, fs) : makeLowpass (f, sq, fs);
            }
            d.numSections = n2;
            if (order % 2 == 1)
            {
                d.firstOrder = hp ? -1 : +1;
                d.foGw = std::tan (kPi * f / fs);
                d.foG  = d.foGw / (1.0 + d.foGw);
            }
            break;
        }
    }
    return d;
}

inline std::complex<double> designResponse (const BandDesign& d, double f, double fs) noexcept
{
    std::complex<double> h (1.0, 0.0);
    for (int k = 0; k < d.numSections; ++k)
        h *= svfResponse (d.sec[(size_t) k], f, fs);
    if (d.firstOrder != 0)
    {
        const double t = std::tan (kPi * std::min (f, 0.4999 * fs) / fs);
        const std::complex<double> s (0.0, t / d.foGw);
        h *= d.firstOrder > 0 ? 1.0 / (1.0 + s) : s / (1.0 + s);
    }
    if (d.tilt)
        h *= tiltResponse (d.tiltC, f, fs);
    return h;
}

//==============================================================================
//  밴드 하나 (최대 2채널)
//   - 주파수(로그) · 게인 · Q(로그)는 스무딩 + 0.33 ms(48k에서 16샘플)마다 계수 재계산, 그 사이는 샘플 단위 선형 보간
//   - 모양 · 기울기 · 적용 채널 · 켜기/끄기 · 추가/삭제는 8 ms 페이드아웃 -> 바꾸기(상태 초기화) -> 페이드인
//     (페이드 = 그 밴드의 효과만 줄였다 키움. 원신호는 그대로라 소리가 끊기지 않음)
//==============================================================================
class BandProcessor
{
public:
    static constexpr int    kSubBlockBase = 16;     // 48 kHz 기준 (0.33 ms)
    static constexpr double kFadeSeconds  = 0.008;

    BandProcessor() { prepare (48000.0); }

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        subBlock = kSubBlockBase * std::max (1, (int) std::lround (fs / 48000.0));
        fadeStep = 1.0 / std::max (1.0, kFadeSeconds * fs);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s) { smoothingSeconds = std::max (0.0, s); applySmoothing(); }

    // 블록마다. eqOn이 꺼지면 모든 밴드가 꺼진 것처럼 페이드아웃
    void setTarget (const BandParams& b, bool eqOn) noexcept
    {
        targetActive = b.isActive() && eqOn;
        if (! targetActive)
            return;                                   // 사라지는 동안은 마지막 설정 그대로 (값이 쓸려가지 않게)
        target = b;
        sF.target = std::log (std::clamp ((double) b.freq, (double) kFreqMin, (double) kFreqMax));
        sG.target = typeHasGain (b.type) ? std::clamp ((double) b.gain, -(double) kGainMax, (double) kGainMax) : 0.0;
        sQ.target = std::log (std::clamp ((double) b.q, (double) kQMin, (double) kQMax));
    }

    // 즉시 목표 상태로 (페이드 없이)
    void reset() noexcept
    {
        cfg = target;
        sF.snap(); sG.snap(); sQ.snap();
        mode = targetActive ? Mode::On : Mode::Off;
        fade = targetActive ? 1.0 : 0.0;
        clearState();
        cur = designNow();
        ramping = false;
        subLeft = 0;
        rampPos = 0;
    }

    void clearState() noexcept { for (auto& c : st) c = ChannelState {}; }

    // 엔진이 32샘플마다 부름: 바뀐 설정이 있으면 페이드아웃, 페이드아웃이 끝났으면 새 설정으로
    void updateState() noexcept
    {
        const bool cfgDiffers = target.type != cfg.type || target.slope != cfg.slope || target.place != cfg.place;
        switch (mode)
        {
            case Mode::Off:
                if (targetActive) start();
                break;
            case Mode::On:
            case Mode::FadingIn:
                if (! targetActive || cfgDiffers) mode = Mode::FadingOut;
                break;
            case Mode::FadingOut:
                if (fade <= 0.0)
                {
                    if (targetActive) start();
                    else              mode = Mode::Off;
                }
                else if (targetActive && ! cfgDiffers)
                {
                    mode = Mode::FadingIn;            // 도중에 원래대로 돌려놓으면 그 자리에서 다시 올라감
                }
                break;
        }
    }

    bool isRunning() const noexcept { return mode != Mode::Off; }
    Placement placement() const noexcept { return cfg.place; }

    // a, b: L/R 그룹이면 L·R, M/S 그룹이면 M·S. nc = 1(모노)이면 a만 (Side 밴드는 모노에서 할 일 없음)
    void process (float* a, float* b, int nc, int n) noexcept
    {
        bool doA, doB;
        if (nc < 2)
        {
            doA = cfg.place != Placement::Side;
            doB = false;
        }
        else
        {
            doA = cfg.place == Placement::Stereo || cfg.place == Placement::Left  || cfg.place == Placement::Mid;
            doB = cfg.place == Placement::Stereo || cfg.place == Placement::Right || cfg.place == Placement::Side;
        }

        int i = 0;
        while (i < n)
        {
            if (subLeft <= 0)
                beginSubBlock();
            const int len = std::min (subLeft, n - i);

            for (int k = 0; k < len; ++k)
            {
                if (ramping)
                    lerpInto (live, base, step, (double) (rampPos + k + 1));
                const BandDesign& d = ramping ? live : cur;

                if (mode == Mode::FadingIn)
                {
                    fade += fadeStep;
                    if (fade >= 1.0) { fade = 1.0; mode = Mode::On; }
                }
                else if (mode == Mode::FadingOut)
                {
                    fade = std::max (0.0, fade - fadeStep);
                }

                if (doA) a[i + k] = (float) tick (d, st[0], (double) a[i + k]);
                if (doB) b[i + k] = (float) tick (d, st[1], (double) b[i + k]);
            }

            rampPos += len;
            subLeft -= len;
            i += len;
        }
    }

private:
    enum class Mode { Off, FadingIn, On, FadingOut };

    struct ChannelState
    {
        std::array<SvfState, 8> sec {};
        double fo = 0.0;
        OnePoleState tilt {};
    };

    double tick (const BandDesign& d, ChannelState& s, double x) const noexcept
    {
        double v = x;
        for (int k = 0; k < d.numSections; ++k)
            v = svfTick (d.sec[(size_t) k], s.sec[(size_t) k], v);
        if (d.firstOrder != 0)
        {
            const double u  = (v - s.fo) * d.foG;
            const double lp = u + s.fo;
            s.fo = lp + u;
            v = d.firstOrder > 0 ? lp : v - lp;
        }
        if (d.tilt)
            v = tiltTick (d.tiltC, s.tilt, v);
        return fade >= 1.0 ? v : x + fade * (v - x);
    }

    void start() noexcept
    {
        cfg = target;
        sF.snap(); sG.snap(); sQ.snap();
        clearState();
        cur = designNow();
        ramping = false;
        subLeft = 0;
        fade = 0.0;
        mode = Mode::FadingIn;
    }

    BandDesign designNow() const noexcept
    {
        return designBand (cfg.type, std::exp (sF.current), sG.current, std::exp (sQ.current), cfg.slope, fs);
    }

    void beginSubBlock() noexcept
    {
        if (ramping)
            cur = next;                 // 직전 서브블록의 끝 = 목표 계수
        ramping = false;
        if (! (sF.isSettled() && sG.isSettled() && sQ.isSettled()))
        {
            sF.step(); sG.step(); sQ.step();
            next = designNow();
            base = cur;
            diffInto (step, next, base, (double) subBlock);
            live = base;
            ramping = true;
        }
        subLeft = subBlock;
        rampPos = 0;
    }

    static void diffInto (BandDesign& out, const BandDesign& to, const BandDesign& from, double n) noexcept
    {
        out = to;
        for (int k = 0; k < to.numSections; ++k)
        {
            const auto& t = to.sec[(size_t) k];
            const auto& f = from.sec[(size_t) k];
            auto& o = out.sec[(size_t) k];
            o.a1 = (t.a1 - f.a1) / n;  o.a2 = (t.a2 - f.a2) / n;  o.a3 = (t.a3 - f.a3) / n;
            o.m0 = (t.m0 - f.m0) / n;  o.m1 = (t.m1 - f.m1) / n;  o.m2 = (t.m2 - f.m2) / n;
        }
        out.foG = (to.foG - from.foG) / n;
        out.tiltC.G1       = (to.tiltC.G1 - from.tiltC.G1) / n;
        out.tiltC.lfMinus1 = (to.tiltC.lfMinus1 - from.tiltC.lfMinus1) / n;
        out.tiltC.outGain  = (to.tiltC.outGain - from.tiltC.outGain) / n;
    }

    static void lerpInto (BandDesign& out, const BandDesign& b, const BandDesign& d, double t) noexcept
    {
        for (int k = 0; k < b.numSections; ++k)
        {
            const auto& bb = b.sec[(size_t) k];
            const auto& dd = d.sec[(size_t) k];
            auto& o = out.sec[(size_t) k];
            o.a1 = bb.a1 + dd.a1 * t;  o.a2 = bb.a2 + dd.a2 * t;  o.a3 = bb.a3 + dd.a3 * t;
            o.m0 = bb.m0 + dd.m0 * t;  o.m1 = bb.m1 + dd.m1 * t;  o.m2 = bb.m2 + dd.m2 * t;
        }
        out.foG = b.foG + d.foG * t;
        out.tiltC.G1       = b.tiltC.G1 + d.tiltC.G1 * t;
        out.tiltC.lfMinus1 = b.tiltC.lfMinus1 + d.tiltC.lfMinus1 * t;
        out.tiltC.outGain  = b.tiltC.outGain + d.tiltC.outGain * t;
    }

    void applySmoothing() noexcept
    {
        const double rate = fs / subBlock;
        for (auto* s : { &sF, &sG, &sQ }) s->setTimeConstant (smoothingSeconds, rate);
        sF.eps = sQ.eps = 1.0e-6;   // 로그 도메인
        sG.eps = 1.0e-4;            // dB
    }

    double fs = 48000.0, smoothingSeconds = 0.015, fadeStep = 0.003, fade = 0.0;
    bool targetActive = false, ramping = false;
    Mode mode = Mode::Off;
    BandParams target, cfg;
    Smoother sF, sG, sQ;
    BandDesign cur, next, base, step, live;
    std::array<ChannelState, 2> st {};
    int subBlock = kSubBlockBase, subLeft = 0, rampPos = 0;
};

//==============================================================================
//  HQ 모드의 오버샘플링 배율 (플러그인과 테스트가 같은 규칙 사용)
//==============================================================================
inline int oversamplingOrderFor (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)  return 2;   // 44.1 / 48 kHz -> 4배
    if (sampleRate <= 100000.0) return 1;   // 88.2 / 96 kHz -> 2배
    return 0;
}

inline double processingRateFor (double sampleRate) noexcept
{
    return sampleRate * (double) (1 << oversamplingOrderFor (sampleRate));
}

//==============================================================================
//  엔진: 밴드 16개 -> EF804S -> 트랜스포머 -> Mix -> Output (채널 1~2개)
//   32샘플(절대 시간 기준) 덩어리마다 밴드 상태 전환을 확인 -> 호스트 블록 크기와 상관없이 같은 결과
//==============================================================================
class Engine
{
public:
    static constexpr int kChunk = 32;

    Engine() { setParams (Params {}); reset(); }

    void prepare (double processingRate, int numChannels, int maxBlock)
    {
        (void) maxBlock;   // 32샘플 덩어리로 처리하므로 블록 크기와 상관없음
        fs = processingRate > 0.0 ? processingRate : 48000.0;
        numCh = std::clamp (numChannels, 1, 2);
        for (auto& b : bands) b.prepare (fs);
        for (auto& t : tube)  t.prepare (fs);
        for (auto& x : xf)    x.prepare (fs);
        applySmoothing();
        reset();
    }

    double getProcessingRate() const noexcept { return fs; }

    void setSmoothingMs (double ms)
    {
        smoothingSeconds = std::max (0.0, ms) * 0.001;
        for (auto& b : bands) b.setSmoothingSeconds (smoothingSeconds);
        for (auto& t : tube)  t.setSmoothingSeconds (smoothingSeconds);
        for (auto& x : xf)    x.setSmoothingSeconds (smoothingSeconds);
        applySmoothing();
    }

    void setParams (const Params& p) noexcept
    {
        for (size_t i = 0; i < bands.size(); ++i)
            bands[i].setTarget (p.bands[i], p.eqOn);
        for (auto& t : tube) t.setTargets (p.driveOn, p.drive, p.focus);
        for (auto& x : xf)   x.setAmounts (p.xfLow, p.xfMid, p.xfHigh);
        mix.target     = clamp01 (p.mix);
        outGain.target = dbToGain (std::clamp ((double) p.outputDb, -24.0, 24.0));
    }

    void reset() noexcept
    {
        for (auto& b : bands) b.reset();
        for (auto& t : tube)  t.reset();
        for (auto& x : xf)    x.reset();
        mix.snap();
        outGain.snap();
        sampleCounter = 0;
    }

    void process (float* const* ch, int numChannels, int n) noexcept
    {
        const int nc = std::min (numChannels, numCh);
        if (nc <= 0 || n <= 0)
            return;

        float* L = ch[0];
        float* R = nc > 1 ? ch[1] : nullptr;
        int i = 0;
        while (i < n)
        {
            const int pos = (int) (sampleCounter % (std::uint64_t) kChunk);
            if (pos == 0)
                for (auto& b : bands) b.updateState();
            const int len = std::min (kChunk - pos, n - i);
            processChunk (L + i, R != nullptr ? R + i : nullptr, nc, len);
            i += len;
            sampleCounter += (std::uint64_t) len;
        }
    }

    BandProcessor& getBand (int i) noexcept { return bands[(size_t) std::clamp (i, 0, kMaxBands - 1)]; }
    TubeStage& getTube (int c) noexcept { return tube[(size_t) std::clamp (c, 0, 1)]; }
    TransformerStage& getTransformer (int c) noexcept { return xf[(size_t) std::clamp (c, 0, 1)]; }

private:
    void processChunk (float* L, float* R, int nc, int n) noexcept
    {
        const bool useMix = ! (mix.isSettled() && sameValue (mix.current, 1.0));
        if (useMix)
        {
            std::copy (L, L + n, dry[0].begin());
            if (R != nullptr) std::copy (R, R + n, dry[1].begin());
        }

        // EQ: L/R 그룹(스테레오 · L · R) 다음 M/S 그룹(Mid · Side). 모노면 Side 말고 전부 그 채널에
        bool anyMs = false;
        for (auto& b : bands)
        {
            if (! b.isRunning()) continue;
            if (nc == 2 && isMsPlacement (b.placement())) { anyMs = true; continue; }
            b.process (L, R, nc, n);
        }
        if (anyMs)
        {
            for (int k = 0; k < n; ++k)
            {
                const double l = L[k], r = R[k];
                L[k] = (float) (0.5 * (l + r));
                R[k] = (float) (0.5 * (l - r));
            }
            for (auto& b : bands)
                if (b.isRunning() && isMsPlacement (b.placement()))
                    b.process (L, R, nc, n);
            for (int k = 0; k < n; ++k)
            {
                const double m = L[k], s = R[k];
                L[k] = (float) (m + s);
                R[k] = (float) (m - s);
            }
        }

        tube[0].process (L, n);
        xf[0].process (L, n);
        if (R != nullptr)
        {
            tube[1].process (R, n);
            xf[1].process (R, n);
        }

        if (useMix)
            for (int k = 0; k < n; ++k)
            {
                const double m = mix.step();
                L[k] = (float) ((double) dry[0][(size_t) k] + m * ((double) L[k] - (double) dry[0][(size_t) k]));
                if (R != nullptr)
                    R[k] = (float) ((double) dry[1][(size_t) k] + m * ((double) R[k] - (double) dry[1][(size_t) k]));
            }

        if (! (outGain.isSettled() && sameValue (outGain.current, 1.0)))
            for (int k = 0; k < n; ++k)
            {
                const double g = outGain.step();
                L[k] = (float) ((double) L[k] * g);
                if (R != nullptr) R[k] = (float) ((double) R[k] * g);
            }
    }

    void applySmoothing() noexcept
    {
        mix.setTimeConstant (smoothingSeconds, fs);      mix.eps = 1.0e-6;
        outGain.setTimeConstant (smoothingSeconds, fs);  outGain.eps = 1.0e-7;
    }

    double fs = 48000.0, smoothingSeconds = 0.015;
    int numCh = 2;
    std::uint64_t sampleCounter = 0;
    std::array<BandProcessor, kMaxBands> bands;
    std::array<TubeStage, 2> tube;
    std::array<TransformerStage, 2> xf;
    Smoother mix, outGain;
    std::array<std::array<float, kChunk>, 2> dry {};
};

//==============================================================================
//  GUI 응답 곡선 (실제 처리와 같은 계수로 계산)
//   포함: 밴드, 트랜스포머 고역 셸프(선형 부분), Mix 블렌드
//   제외: 튜브/트랜스포머 비선형 성분(기준 레벨에서 이득 1), Output
//   뷰: Stereo = 스테레오 밴드만, Left/Right/Mid/Side = 스테레오 밴드 + 그 채널 밴드
//       (L/R 밴드와 M/S 밴드를 함께 쓰면 실제로는 서로 섞이므로 표시는 근사)
//==============================================================================
class ResponseModel
{
public:
    ResponseModel (const Params& p, double sampleRate) noexcept
        : fs (sampleRate > 0.0 ? sampleRate : 48000.0)
    {
        for (size_t i = 0; i < p.bands.size(); ++i)
        {
            const auto& b = p.bands[i];
            active[i] = b.isActive() && p.eqOn;
            place[i]  = b.place;
            if (active[i])
                designs[i] = designBand (b.type, b.freq, b.gain, b.q, b.slope, fs);
        }
        shelf = makeHighShelfMatched (TransformerStage::kShelfHz, TransformerStage::kShelfQ,
                                      -TransformerStage::kShelfMaxCutDb * clamp01 (p.xfHigh), fs);
        mix = clamp01 (p.mix);
    }

    bool isActive (int i) const noexcept { return active[(size_t) i]; }

    std::complex<double> bandResponse (int i, double f) const noexcept
    {
        return active[(size_t) i] ? designResponse (designs[(size_t) i], f, fs) : std::complex<double> (1.0, 0.0);
    }

    bool usesPlacement (Placement pl) const noexcept
    {
        for (size_t i = 0; i < active.size(); ++i)
            if (active[i] && place[i] == pl) return true;
        return false;
    }

    std::complex<double> eqResponse (double f, Placement view = Placement::Stereo) const noexcept
    {
        std::complex<double> h (1.0, 0.0);
        for (size_t i = 0; i < active.size(); ++i)
            if (active[i] && (place[i] == Placement::Stereo || place[i] == view))
                h *= designResponse (designs[i], f, fs);
        return h;
    }

    double magnitudeDb (double f, Placement view = Placement::Stereo) const noexcept
    {
        const auto h = (1.0 - mix) + mix * eqResponse (f, view) * svfResponse (shelf, f, fs);
        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
    }

private:
    double fs, mix = 1.0;
    SvfCoeffs shelf;
    std::array<bool, kMaxBands> active {};
    std::array<Placement, kMaxBands> place {};
    std::array<BandDesign, kMaxBands> designs {};
};

//==============================================================================
//  v0.4 이하 세션 변환: 모듈(Treble · Pivot/Presence · Bass) -> 같은 곡선의 밴드
//   - 링크면 스테레오 밴드, 아니면 모듈 1/2 -> L/R 또는 Mid/Side 밴드
//   - 스텝·범위 규칙은 예전 그대로 적용한 값으로, 게인 0인 부분은 밴드를 만들지 않음
//   - 셸프 주파수: 예전 "기준(다 올라오는 곳)" -> 중간점 (Bass x 4, Treble / 4), Q는 새 셸프 Q 기준으로 환산
//==============================================================================
inline std::vector<BandParams> migrateLegacyBands (const LegacyParams& L)
{
    std::vector<BandParams> out;
    const int modules = L.link ? 1 : 2;

    for (int m = 0; m < modules; ++m)
    {
        const LegacyModule& md = L.mod[(size_t) m];
        const Placement place = L.link ? Placement::Stereo
                              : (L.midSide ? (m == 0 ? Placement::Mid  : Placement::Side)
                                           : (m == 0 ? Placement::Left : Placement::Right));
        const bool st = L.stepped;

        auto add = [&] (BandType t, double f, double g, double q)
        {
            if (std::abs (g) < 1.0e-4 || (int) out.size() >= kMaxBands)
                return;
            BandParams b;
            b.used = true;  b.on = true;  b.type = t;  b.place = place;
            b.freq = (float) f;  b.gain = (float) g;  b.q = (float) std::clamp (q, (double) kQMin, (double) kQMax);
            out.push_back (b);
        };

        const double bassDb = st ? quantizeBass (md.bassDb) : std::clamp (md.bassDb, kBassMinDb, kBassMaxDb);
        const double bassHz = st ? nearestStep (kBassSteps, md.bassHz) : clampToSteps (kBassSteps, md.bassHz);
        const double bassQ  = std::clamp (md.bassQ, kShelfQMin, kShelfQMax);
        add (BandType::LowShelf, bassHz * kShelfRefToMid, bassDb, bassQ / kButterworthQ);

        if (md.modeB)
        {
            const double presDb = st ? quantizePresence (md.presDb) : std::clamp (md.presDb, 0.0f, kPresMaxDb);
            const double presHz = st ? nearestStep (kPresenceSteps, md.presHz) : clampToSteps (kPresenceSteps, md.presHz);
            add (BandType::Bell, presHz, presDb, std::clamp (md.presQ, kPresQMin, kPresQMax));
        }
        else
        {
            const double tiltDb = st ? quantizeTilt (md.tiltDb) : std::clamp (md.tiltDb, -kTiltMaxDb, kTiltMaxDb);
            add (BandType::Tilt, kTiltPivotHz, tiltDb, 1.0);
        }

        const double trebleDb = st ? quantizeTreble (md.trebleDb) : std::clamp (md.trebleDb, kTrebleMinDb, kTrebleMaxDb);
        const double trebleHz = st ? nearestStep (kTrebleSteps, md.trebleHz) : clampToSteps (kTrebleSteps, md.trebleHz);
        const double trebleQ  = std::clamp (md.trebleQ, kShelfQMin, kShelfQMax);
        add (BandType::HighShelf, trebleHz / kShelfRefToMid, trebleDb, trebleQ / kButterworthQ);
    }
    return out;
}

} // namespace pillow::rz
