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
    // 이 밴드의 주파수 부분에만 거는 아날로그 (v0.6 켜기/끄기, v0.7 밴드마다 따로 설정)
    bool      tube     = false;         // EF804S 튜브
    float     drive    = 18.0f;         //   드라이브 dB (0..48)
    float     focus    = 0.0f;          //   -1 홀수 .. +1 짝수
    float     tubeMix  = 1.0f;          //   0..1 (그 부분에서 원음과 섞기)
    bool      xfmr     = false;         // 출력 트랜스포머
    float     xfAmount = 0.5f;          //   0..1 (성격은 밴드 주파수로 자동)

    bool isActive() const noexcept { return used && on; }

    bool operator== (const BandParams& o) const noexcept
    {
        return used == o.used && on == o.on && type == o.type && slope == o.slope && place == o.place
            && tube == o.tube && xfmr == o.xfmr
            && sameValue (freq, o.freq) && sameValue (gain, o.gain) && sameValue (q, o.q)
            && sameValue (drive, o.drive) && sameValue (focus, o.focus) && sameValue (tubeMix, o.tubeMix)
            && sameValue (xfAmount, o.xfAmount);
    }
    bool operator!= (const BandParams& o) const noexcept { return ! (*this == o); }
};

struct Params
{
    bool  eqOn    = true;
    bool  driveOn = false;    // 튜브를 신호 전체에 (ALL, 밴드들 뒤). 밴드마다의 튜브는 BandParams
    bool  xfAll   = true;     // 트랜스포머를 신호 전체에 (ALL, 튜브 뒤). 밴드마다의 트랜스포머는 BandParams
    float drive   = 0.0f;     // EF804S 드라이브 0..48 dB
    float focus   = 0.0f;     // -1 = 홀수 배음(대칭)  0 = EF804S 그대로  +1 = 짝수 배음 2배
    float mix     = 1.0f;     // 0..1 (원음 블렌드)
    float xfLow = 0.0f, xfMid = 0.0f, xfHigh = 0.0f;   // 트랜스포머 색채 0..1 (구간별)
    float outputDb = 0.0f;
    std::array<BandParams, kMaxBands> bands {};

    bool operator== (const Params& o) const noexcept
    {
        return eqOn == o.eqOn && driveOn == o.driveOn && xfAll == o.xfAll
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

// 아날로그 1차 틸트의 크기 제곱: 고역 +tilt dB, 저역 -tilt dB, 축(pivot)에서 0 dB
inline double tiltAnalogMag2 (double pivotHz, double tiltDb, double f) noexcept
{
    const double G  = std::pow (10.0, -2.0 * tiltDb / 20.0);   // 고역 대비 저역 게인
    const double K  = std::pow (10.0, tiltDb / 20.0);
    const double wp = pivotHz / std::sqrt (G);                 // 극점 (Hz), 영점 = wp * G
    return K * K * (f * f + wp * wp * G * G) / (f * f + wp * wp);
}

// 1차 틸트: DC · 나이퀴스트 · 축(세션 레이트의 1/4 위면 거기서)에서 크기가 아날로그와 같도록 (TPT 1폴 구조 그대로)
//  쌍선형 + 극점 맞춤(v0.6까지)은 극점이 나이퀴스트에 가깝거나 넘으면(높은 축 + 큰 틸트) 축 근처까지 몇~20 dB 틀어졌음.
//  20 Hz ~ 20 kHz, 축 10 Hz ~ 30 kHz, ±24 dB, 44.1 ~ 192 kHz 전체에서 아날로그와 최대 약 0.9 dB (보통 범위 0.4 dB 이하)
inline TiltCoeffs makeTilt (double pivotHz, double tiltDb, double fs) noexcept
{
    const double M0 = tiltAnalogMag2 (pivotHz, tiltDb, 0.0);
    const double MN = tiltAnalogMag2 (pivotHz, tiltDb, 0.5 * fs);
    TiltCoeffs c;
    c.outGain  = std::sqrt (MN);                  // 나이퀴스트 (1폴 로우패스가 0)
    c.lfMinus1 = std::sqrt (M0 / MN) - 1.0;       // DC = outGain * (1 + lfMinus1)

    // 극점: 세 번째 점(축)의 크기로 정함. 틸트 0 근처에서도 이어지도록 아주 작은 틸트는 작은 값으로 계산
    const double tg = std::abs (tiltDb) < 1.0e-3 ? std::copysign (1.0e-3, tiltDb == 0.0 ? 1.0 : tiltDb) : tiltDb;
    const double fm = std::min (std::max (pivotHz, 1.0), 0.25 * fs);
    const double a0 = tiltAnalogMag2 (pivotHz, tg, 0.0), aN = tiltAnalogMag2 (pivotHz, tg, 0.5 * fs);
    const double am = tiltAnalogMag2 (pivotHz, tg, fm);
    const double ratio = (am - aN) / (a0 - am);
    double g = std::tan (kPi * fm / fs) * std::sqrt (std::max (ratio, 0.0));
    if (! (g > 0.0) || ! std::isfinite (g))
        g = std::tan (kPi * clampFreq (pivotHz, fs) / fs);
    c.g  = std::clamp (g, 1.0e-9, 1.0e9);
    c.G1 = c.g / (1.0 + c.g);
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

    // 오디오 루프용: 홀수·짝수 성분을 한 번에 (격자 위치 한 번, Catmull-Rom 가중치 한 번)
    //  dO = (dev(v) - dev(-v)) / 2,  dE = (dev(v) + dev(-v)) / 2,  pO = (psi(v) + psi(-v)) / 2,  pE = (psi(v) - psi(-v)) / 2
    //  |v| 쪽 절반만 읽고 부호로 돌림 (dO · pE는 홀함수, dE · pO는 짝함수) -> 캐시에 머무는 표가 절반
    void evalParts (double v, double& dO, double& dE, double& pO, double& pE) const noexcept
    {
        if (! (std::abs (v) <= vEdge))   // 표 밖 (NaN 포함)
        {
            const double dp = dev (v), dn = dev (-v), pp = psi (v), pn = psi (-v);
            dO = 0.5 * (dp - dn);  dE = 0.5 * (dp + dn);
            pO = 0.5 * (pp + pn);  pE = 0.5 * (pp - pn);
            return;
        }
        const double ax = std::abs (v) / kV0;
        const double w  = std::log (ax + std::sqrt (ax * ax + 1.0));   // asinh (|v| / kV0)
        const Point  p  = locate (w);
        const double f = p.f, f2 = f * f, f3 = f2 * f;
        const double c0 = 0.5 * (2.0 * f2 - f - f3), c1 = 0.5 * (2.0 - 5.0 * f2 + 3.0 * f3);
        const double c2 = 0.5 * (f + 4.0 * f2 - 3.0 * f3), c3 = 0.5 * (f3 - f2);
        const Parts& a = parts[p.i - 1];
        const Parts& b = parts[p.i];
        const Parts& q = parts[p.i + 1];
        const Parts& d = parts[p.i + 2];
        dO = c0 * a.dO + c1 * b.dO + c2 * q.dO + c3 * d.dO;
        dE = c0 * a.dE + c1 * b.dE + c2 * q.dE + c3 * d.dE;
        pO = c0 * a.pO + c1 * b.pO + c2 * q.pO + c3 * d.pO;
        pE = c0 * a.pE + c1 * b.pE + c2 * q.pE + c3 * d.pE;
        if (v < 0.0)
        {
            dO = -dO;
            pE = -pE;
        }
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

        // 5) 홀수·짝수 성분 표 (격자가 0 V 기준 대칭이라 -v 쪽은 거울 위치: k <-> kSize-1-k)
        parts.resize ((size_t) kSize);
        for (size_t k = 0; k < (size_t) kSize; ++k)
        {
            const size_t m = (size_t) kSize - 1 - k;
            parts[k] = { 0.5 * (devTable[k] - devTable[m]), 0.5 * (devTable[k] + devTable[m]),
                         0.5 * (psiTable[k] + psiTable[m]), 0.5 * (psiTable[k] - psiTable[m]) };
        }
    }

    struct Parts { double dO, dE, pO, pE; };

    Table devTable {}, psiTable {};
    std::vector<Parts> parts;
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

    // mixAmount: 원음과 섞는 비율 (on/off와 함께 부드럽게 바뀜)
    void setTargets (bool on, double driveDb, double focus, double mixAmount = 1.0) noexcept
    {
        wet.target   = on ? clamp01 (mixAmount) : 0.0;
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

    // 켜져 있거나 꺼지는 중 (false면 process가 아무것도 바꾸지 않음)
    bool isEngaged() const noexcept { return wet.target > 0.0 || wetNow > 0.0; }

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
        double dO, dE, pO, pE;
        curve->evalParts (x * scale, dO, dE, pO, pE);
        hOdd  = dO * invScale;
        hEven = evenMul * dE * invScale;
        HOdd  = pO * invScale2;
        HEven = evenMul * pE * invScale2;
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
        invScale2 = invScale * invScale;
        evenMul  = 1.0 + foc.current;

        // 기준 레벨(-12 dBFS)에서의 기본파 이득 (describing function)
        //  짝수 성분은 기본파에 들어가지 않고, 사인은 반 주기 · 1/4 주기 대칭 -> 홀수 성분만 8점 (32점과 같은 값)
        double sum = 0.0;
        for (int k = 0; k < kDfPoints / 4; ++k)
        {
            const double s  = sinTable[(size_t) k];
            const double xk = kRefAmp * s;
            double dO, dE, pO, pE;
            curve->evalParts (xk * scale, dO, dE, pO, pE);
            sum += (xk + dO * invScale) * s;
        }
        const double df = (2.0 / (double) kDfPoints) * 4.0 * sum / kRefAmp;
        norm = df > 1.0e-9 ? 1.0 / df : 1.0;
    }

    const Ef804sCurve* curve = &ef804sCurve();
    std::array<double, kDfPoints> sinTable {};
    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double wetNow = 0.0, scale = kGridVoltsPerFullScale, invScale = 1.0 / kGridVoltsPerFullScale, invScale2 = invScale * invScale;
    double evenMul = 1.0, norm = 1.0, dcX1 = 0.0, dcY1 = 0.0;
    AdaaAligned oddPart;
    Adaa1 evenPart;
    Smoother wet, drive, foc;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  트랜스포머 곡선 (LOW · HIGH · MID) — 정확한 식
//   왜곡 성분 h(u) = f(u) - u, 역도함수 H(u) (H(0) = 0), 기울기 h'(u)
//==============================================================================
struct TransformerShapes
{
    static constexpr double kLowDrive = 1.4;
    static constexpr double kMidDrive = 0.45, kMidBias = 0.12;
    static constexpr double kHighDrive = 1.2;

    static double hSym (double u, double k) noexcept  { return std::tanh (k * u) / k - u; }
    static double HSym (double u, double k) noexcept  { return logCosh (k * u) / (k * k) - 0.5 * u * u; }
    static double dhSym (double u, double k) noexcept { const double t = std::tanh (k * u); return -t * t; }

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
    static double dhMid (double u) noexcept
    {
        const double tb = std::tanh (kMidBias), t = std::tanh (kMidDrive * u + kMidBias);
        return (1.0 - t * t) / (1.0 - tb * tb) - 1.0;
    }
};

//==============================================================================
//  트랜스포머 곡선 표 (오디오 루프용, 프로세스 전체에서 하나)
//   곡선 4개: LOW, HIGH, MID 홀수 성분, MID 짝수 성분. 격자 1/32 (u = -8..8)에 h, h', H 저장
//   h: 3차 에르미트 (값 + 기울기 h'). H: 5차 에르미트 (값 + 기울기 h + 곡률 h')
//    -> ADAA가 나누는 H의 차이가 격자 사이에서도 h와 맞고, 0 근처의 u^4 · u^5 항이 정확히 재현돼
//       작은 신호에서 가짜 선형 이득이 생기지 않음. |u| > 8 (+18 dBFS 넘음)은 식 그대로 계산
//==============================================================================
class TransformerCurves
{
public:
    enum Curve { Low = 0, High, MidOdd, MidEven, kNumCurves };
    static constexpr int    kPerUnit = 32;
    static constexpr double kRange   = 8.0;
    static constexpr int    kSize    = 2 * 8 * kPerUnit + 1;

    // 입력 값 하나의 격자 위치와 에르미트 기저 (곡선 여러 개에 같이 씀)
    struct At
    {
        double u = 0.0;
        double c00 = 1.0, c10 = 0.0, c01 = 0.0, c11 = 0.0;                  // 3차 (h)
        double q0 = 1.0, q1 = 0.0, q2 = 0.0, q3 = 0.0, q4 = 0.0, q5 = 0.0;   // 5차 (H)
        int i = 0;
        bool inside = true;
    };

    TransformerCurves() { build(); }

    At at (double u) const noexcept
    {
        At a;
        a.u = u;
        const double pos = (u + kRange) * (double) kPerUnit;
        a.inside = pos >= 0.0 && pos < (double) (kSize - 1);     // NaN -> 밖
        if (! a.inside)
            return a;
        a.i = (int) pos;
        const double t = pos - (double) a.i, t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
        const double s = 1.0 / (double) kPerUnit, s2 = s * s;
        a.c00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        a.c01 = 3.0 * t2 - 2.0 * t3;
        a.c10 = (t3 - 2.0 * t2 + t) * s;
        a.c11 = (t3 - t2) * s;
        a.q5 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
        a.q0 = 1.0 - a.q5;
        a.q1 = (t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5) * s;
        a.q4 = (7.0 * t4 - 4.0 * t3 - 3.0 * t5) * s;
        a.q2 = 0.5 * (t2 - 3.0 * t3 + 3.0 * t4 - t5) * s2;
        a.q3 = 0.5 * (t3 - 2.0 * t4 + t5) * s2;
        return a;
    }

    void eval (Curve c, const At& a, double& h, double& H) const noexcept
    {
        if (! a.inside)
        {
            exact (c, a.u, h, H);
            return;
        }
        const Node& n0 = table[(size_t) c][(size_t) a.i];
        const Node& n1 = table[(size_t) c][(size_t) a.i + 1];
        h = a.c00 * n0.h + a.c10 * n0.dh + a.c01 * n1.h + a.c11 * n1.dh;
        H = a.q0 * n0.H + a.q1 * n0.h + a.q2 * n0.dh + a.q3 * n1.dh + a.q4 * n1.h + a.q5 * n1.H;
    }

    static void exact (Curve c, double u, double& h, double& H) noexcept
    {
        using S = TransformerShapes;
        switch (c)
        {
            case Low:  h = S::hSym (u, S::kLowDrive);  H = S::HSym (u, S::kLowDrive);  return;
            case High: h = S::hSym (u, S::kHighDrive); H = S::HSym (u, S::kHighDrive); return;
            case MidOdd:
                h = 0.5 * (S::hMid (u) - S::hMid (-u));
                H = 0.5 * (S::HMid (u) + S::HMid (-u));
                return;
            case MidEven:
            case kNumCurves:
            default:
                h = 0.5 * (S::hMid (u) + S::hMid (-u));
                H = 0.5 * (S::HMid (u) - S::HMid (-u));
                return;
        }
    }

    static double exactSlope (Curve c, double u) noexcept
    {
        using S = TransformerShapes;
        switch (c)
        {
            case Low:     return S::dhSym (u, S::kLowDrive);
            case High:    return S::dhSym (u, S::kHighDrive);
            case MidOdd:  return 0.5 * (S::dhMid (u) + S::dhMid (-u));
            case MidEven:
            case kNumCurves:
            default:      return 0.5 * (S::dhMid (u) - S::dhMid (-u));
        }
    }

private:
    struct Node { double h, dh, H; };

    void build()
    {
        for (int c = 0; c < kNumCurves; ++c)
        {
            table[(size_t) c].resize ((size_t) kSize);
            for (int k = 0; k < kSize; ++k)
            {
                const double u = -kRange + (double) k / (double) kPerUnit;
                Node& n = table[(size_t) c][(size_t) k];
                exact ((Curve) c, u, n.h, n.H);
                n.dh = exactSlope ((Curve) c, u);
            }
        }
    }

    std::array<std::vector<Node>, kNumCurves> table;
};

inline const TransformerCurves& transformerCurves()
{
    static const TransformerCurves curves;
    return curves;
}

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
    static constexpr double kLowDrive  = TransformerShapes::kLowDrive;
    static constexpr double kMidDrive  = TransformerShapes::kMidDrive, kMidBias = TransformerShapes::kMidBias;
    static constexpr double kHighDrive = TransformerShapes::kHighDrive;
    static constexpr double kShelfHz = 12000.0, kShelfQ = 0.5, kShelfMaxCutDb = 1.5;

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        curves = &transformerCurves();
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
                    const auto& tc = *curves;
                    double h, H;
                    tc.eval (TransformerCurves::Low, tc.at (lo), h, H);
                    const auto sl = bands[0].tickSplit (lo, h, H);
                    tc.eval (TransformerCurves::High, tc.at (hi), h, H);
                    const auto sh = bands[2].tickSplit (hi, h, H);

                    // MID는 비대칭 -> 홀수 성분(압축·홀수 배음)과 짝수 성분(짝수 배음·DC)으로 나눠서
                    const auto am = tc.at (mid);
                    double he, He;
                    tc.eval (TransformerCurves::MidOdd, am, h, H);
                    tc.eval (TransformerCurves::MidEven, am, he, He);
                    const auto   sm = bands[1].tickSplit (mid, h, H);
                    const double me = midEven.tick (mid, he, He);
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

    bool isEngaged() const noexcept { return active || aLow.target > 0.0 || aMid.target > 0.0 || aHigh.target > 0.0; }

    // 구간별 왜곡 성분 h(u) = f(u) - u 와 그 역도함수 H(u) (H(0) = 0) — 정확한 식 (테스트용)
    static double hLow (double u) noexcept  { return TransformerShapes::hSym (u, kLowDrive); }
    static double HLow (double u) noexcept  { return TransformerShapes::HSym (u, kLowDrive); }
    static double hHigh (double u) noexcept { return TransformerShapes::hSym (u, kHighDrive); }
    static double HHigh (double u) noexcept { return TransformerShapes::HSym (u, kHighDrive); }
    static double hMid (double u) noexcept  { return TransformerShapes::hMid (u); }
    static double HMid (double u) noexcept  { return TransformerShapes::HMid (u); }

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
    const TransformerCurves* curves = &transformerCurves();
    SvfCoeffs lpLo, hpLo, lpHi, hpHi, shelf;
    std::array<SvfState, 8> xs {};
    std::array<AdaaAligned, 3> bands {};
    Adaa1 midEven;
    SvfState shelfState;
    Smoother aLow, aMid, aHigh;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  밴드 하나의 트랜스포머 (v0.7) — 구간을 나누지 않고 그 밴드 부분에 거는 포화
//   성격은 밴드 주파수로 자동: 200 Hz 아래 = LOW 곡선(코어 포화, 대칭 3차 위주),
//   4 kHz 위 = HIGH 곡선, 그 사이 = MID 곡선(약한 비대칭, 2차 + 3차). 경계는 1옥타브에 걸쳐 섞음
//   양 0..1 = 전체 트랜스포머의 구간별 양과 같은 뜻. 1차 ADAA + 선형 성분 정렬 (레이턴시 0)
//==============================================================================
class TransformerBand
{
public:
    void prepare (double sampleRate)
    {
        fs  = sampleRate > 0.0 ? sampleRate : 48000.0;
        dcR = std::exp (-2.0 * kPi * 5.0 / fs);
        curves = &transformerCurves();
        odd.prepare (fs);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s) { smoothingSeconds = std::max (0.0, s); applySmoothing(); }

    // characterHz: 성격을 고르는 주파수 (보통 밴드 주파수)
    void setTarget (double amount, double characterHz) noexcept
    {
        amt.target = clamp01 (amount);
        hzTarget   = std::max (1.0, characterHz);
    }

    void reset() noexcept
    {
        amt.snap();
        hzNow = hzTarget;
        clearState();
        samplesToNextUpdate = 0;
        updateDerived();
    }

    void clearState() noexcept { odd.reset(); even.reset(); dcX1 = dcY1 = 0.0; }

    bool isEngaged() const noexcept { return amt.target > 0.0 || cur > 0.0; }

    // 그 자리에서: x + 양 x (왜곡 성분)
    void process (float* x, int n) noexcept
    {
        int i = 0;
        while (i < n)
        {
            if (samplesToNextUpdate <= 0)
            {
                if (! amt.isSettled() || ! sameValue (hzNow, hzTarget))
                {
                    amt.step();
                    hzNow = hzTarget;
                    updateDerived();
                    double ho, Ho, he, He;                    // 곡선이 바뀌었으니 직전 입력의 값도 새 곡선으로
                    shapeAt (odd.lastInput(), ho, Ho, he, He);
                    odd.rebase (ho, Ho);
                    even.rebase (he, He);
                }
                samplesToNextUpdate = kChunk;
            }
            const int len = std::min (samplesToNextUpdate, n - i);

            if (cur > 0.0)
            {
                float* p = x + i;
                for (int k = 0; k < len; ++k)
                {
                    const double in = p[k];
                    double ho, Ho, he, He;
                    shapeAt (in, ho, Ho, he, He);
                    const auto sp   = odd.tickSplit (in, ho, Ho);   // 압축 + 홀수 배음 (정렬됨)
                    const double e  = even.tick (in, he, He);       // 짝수 배음 + DC
                    const double hp = e - dcX1 + dcR * dcY1;        // DC 블로커는 짝수 성분에만
                    dcX1 = e;
                    dcY1 = hp;
                    p[k] = (float) (in + cur * (sp.distortion + sp.slope * in + hp));
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

private:
    static constexpr int kChunk = 32;

    void shapeAt (double u, double& hO, double& HO, double& hE, double& HE) const noexcept
    {
        const auto& tc = *curves;
        const auto a = tc.at (u);
        double h, H;
        hO = HO = hE = HE = 0.0;
        if (wL > 0.0) { tc.eval (TransformerCurves::Low, a, h, H);  hO += wL * h;  HO += wL * H; }
        if (wH > 0.0) { tc.eval (TransformerCurves::High, a, h, H); hO += wH * h;  HO += wH * H; }
        if (wM > 0.0)
        {
            tc.eval (TransformerCurves::MidOdd, a, h, H);   hO += wM * h;  HO += wM * H;
            tc.eval (TransformerCurves::MidEven, a, h, H);  hE = wM * h;   HE = wM * H;
        }
    }

    void updateDerived() noexcept
    {
        cur = amt.current;
        const double a = clamp01 (0.5 + std::log2 (hzNow / TransformerStage::kXoverLowHz));    // 0 = LOW
        const double b = clamp01 (0.5 + std::log2 (hzNow / TransformerStage::kXoverHighHz));   // 1 = HIGH
        wL = 1.0 - a;
        wH = b;
        wM = std::max (0.0, a - b);
    }

    void applySmoothing() noexcept
    {
        amt.setTimeConstant (std::max (smoothingSeconds, 0.005), fs / kChunk);
        amt.eps = 1.0e-5;
    }

    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double cur = 0.0, hzNow = 1000.0, hzTarget = 1000.0, wL = 0.0, wM = 1.0, wH = 0.0;
    double dcX1 = 0.0, dcY1 = 0.0;
    const TransformerCurves* curves = &transformerCurves();
    AdaaAligned odd;
    Adaa1 even;
    Smoother amt;
    int samplesToNextUpdate = 0;
};

// 밴드 트랜스포머의 성격을 고르는 주파수: 컷은 남기는 쪽이 넓어서, 틸트는 전체라서 MID 성격
inline double transformerCharacterHz (BandType t, double freq) noexcept
{
    return (t == BandType::LowCut || t == BandType::HighCut || t == BandType::Tilt) ? 1000.0 : freq;
}

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
        case BandType::Tilt:      // 1차 틸트는 축이 나이퀴스트 위여도 아날로그 곡선 그대로 맞출 수 있음 (자르지 않음)
            d.tilt = true;
            d.tiltC = makeTilt (std::clamp (freq, (double) kFreqMin, (double) kFreqMax), g, fs);
            break;
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

//==============================================================================
//  밴드의 "영역": 그 밴드의 튜브 · 트랜스포머가 거는 주파수 부분
//   벨 · 노치 = 중심 둘레 (밴드패스, 중심 이득 1, 밴드 Q), 로우 셸프 · 하이 컷 = 그 아래 (로우패스),
//   하이 셸프 · 로우 컷 = 그 위 (하이패스), 틸트 = 전체
//   쓰는 쪽: 출력 = x + (아날로그(R x) - R x) -> 아날로그가 투명하면 원신호 그대로, 영역 밖은 손대지 않음
//==============================================================================
inline bool regionIsAll (BandType t) noexcept { return t == BandType::Tilt; }

inline SvfCoeffs makeRegion (BandType t, double freq, double q, double fs) noexcept
{
    const double f = clampFreq (std::clamp (freq, (double) kFreqMin, (double) kFreqMax), fs);
    switch (t)
    {
        case BandType::Bell:
        case BandType::Notch:
        {
            SvfCoeffs c;
            c.g  = std::tan (kPi * f / fs);
            c.k  = 1.0 / std::clamp (q, (double) kQMin, (double) kQMax);
            c.m0 = 0.0; c.m1 = c.k; c.m2 = 0.0;
            return finishSvf (c);
        }
        case BandType::LowShelf:
        case BandType::HighCut:   return makeLowpass (f, kButterworthQ, fs);
        case BandType::HighShelf:
        case BandType::LowCut:    return makeHighpass (f, kButterworthQ, fs);
        case BandType::Tilt:      break;
    }
    SvfCoeffs all;                    // 전체: 출력 = 입력
    all.g = std::tan (kPi * f / fs);
    all.k = 1.0;
    all.m0 = 1.0; all.m1 = 0.0; all.m2 = 0.0;
    return finishSvf (all);
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
//   - 아날로그 (v0.7): 이 밴드 영역 -> EF804S(이 밴드의 드라이브 · 포커스 · 믹스) -> 트랜스포머(이 밴드의 양)
//     엔진이 EQ를 다 건 신호에서 영역을 떼어 처리하고 "바뀐 만큼"만 더함 (밴드끼리 병렬, 밴드 페이드를 따라감)
//==============================================================================
class BandProcessor
{
public:
    static constexpr int    kSubBlockBase = 16;     // 48 kHz 기준 (0.33 ms)
    static constexpr double kFadeSeconds  = 0.008;
    static constexpr int    kMaxChunk     = 32;     // 엔진의 덩어리 길이 (아날로그용 페이드 기록)

    BandProcessor() { prepare (48000.0); }

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        subBlock = kSubBlockBase * std::max (1, (int) std::lround (fs / 48000.0));
        fadeStep = 1.0 / std::max (1.0, kFadeSeconds * fs);
        for (auto& t : analog.tube) t.prepare (fs);
        for (auto& x : analog.xf)   x.prepare (fs);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s)
    {
        smoothingSeconds = std::max (0.0, s);
        for (auto& t : analog.tube) t.setSmoothingSeconds (smoothingSeconds);
        for (auto& x : analog.xf)   x.setSmoothingSeconds (smoothingSeconds);
        applySmoothing();
    }

    // 블록마다. eqOn이 꺼지면 모든 밴드가 꺼진 것처럼 페이드아웃
    void setTarget (const BandParams& b, bool eqOn) noexcept
    {
        targetActive = b.isActive() && eqOn;
        for (auto& t : analog.tube)
            t.setTargets (targetActive && b.tube, b.drive, b.focus, b.tubeMix);
        for (auto& x : analog.xf)
            x.setTarget (targetActive && b.xfmr ? (double) b.xfAmount : 0.0, transformerCharacterHz (b.type, b.freq));
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
        for (auto& t : analog.tube) t.reset();
        for (auto& x : analog.xf)   x.reset();
        analog.valid = false;
        analog.live  = false;
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
        channelsFor (nc, doA, doB);

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
                if (traceIdx < kMaxChunk)
                    fadeTrace[(size_t) traceIdx++] = fade;

                if (doA) a[i + k] = (float) tick (d, st[0], (double) a[i + k]);
                if (doB) b[i + k] = (float) tick (d, st[1], (double) b[i + k]);
            }

            rampPos += len;
            subLeft -= len;
            i += len;
        }
    }

    //--------------------------------------------------------------------------
    //  아날로그 (이 밴드 영역의 튜브 · 트랜스포머)
    //   엔진의 32샘플 덩어리(절대 시간) 단위: 덩어리 처음에 beginChunk, 그 안에서는 위치(pos)로 샘플마다
    //   -> 호스트 블록이 덩어리를 어떻게 자르든 결과가 비트 단위로 같음
    //--------------------------------------------------------------------------
    void beginChunk() noexcept
    {
        traceIdx = 0;
        analog.tubeLive = analog.tube[0].isEngaged() || analog.tube[1].isEngaged();
        analog.xfLive   = analog.xf[0].isEngaged()   || analog.xf[1].isEngaged();
        analog.live = mode != Mode::Off && (analog.tubeLive || analog.xfLive);
        if (! analog.live)
        {
            analog.valid = false;
            return;
        }
        analog.from = analog.to;
        const bool same = analog.valid && sameValue (analog.fHz, sF.current) && sameValue (analog.qVal, sQ.current);
        if (! same)
        {
            analog.fHz  = sF.current;
            analog.qVal = sQ.current;
            analog.to = makeRegion (cfg.type, std::exp (sF.current), std::exp (sQ.current), fs);
        }
        if (! analog.valid)             // 새로 쓰기 시작 (또는 모양이 바뀐 뒤): 보간 없이, 상태 비우고
        {
            analog.from = analog.to;
            analog.region = {};
            analog.valid = true;
        }
        analog.moving = ! (sameValue (analog.from.a1, analog.to.a1) && sameValue (analog.from.a2, analog.to.a2)
                           && sameValue (analog.from.a3, analog.to.a3) && sameValue (analog.from.m0, analog.to.m0)
                           && sameValue (analog.from.m1, analog.to.m1) && sameValue (analog.from.m2, analog.to.m2));
    }

    bool analogLive() const noexcept { return analog.live; }

    // a, b = 이 그룹의 두 채널 (L·R 또는 M·S) = EQ를 다 건 신호, da, db 에 "바뀐 만큼"을 더함
    // pos = 덩어리 안에서의 시작 위치. 채널 규칙은 process와 같음
    void applyAnalog (const float* a, const float* b, float* da, float* db, int nc, int n, int pos) noexcept
    {
        if (! analog.live)
            return;
        bool doA, doB;
        channelsFor (nc, doA, doB);
        std::array<float, kMaxChunk> r {}, y {};
        const int len = std::min (n, kMaxChunk);

        auto run = [&] (const float* x, float* d, size_t ch)
        {
            const auto& f = analog.from;
            const auto& t = analog.to;
            if (analog.moving)
            {
                for (int k = 0; k < len; ++k)
                {
                    const int idx = std::min (pos + k, kMaxChunk - 1);
                    const double u = (double) (idx + 1) / (double) kMaxChunk;
                    SvfCoeffs c;
                    c.a1 = f.a1 + (t.a1 - f.a1) * u;  c.a2 = f.a2 + (t.a2 - f.a2) * u;  c.a3 = f.a3 + (t.a3 - f.a3) * u;
                    c.m0 = f.m0 + (t.m0 - f.m0) * u;  c.m1 = f.m1 + (t.m1 - f.m1) * u;  c.m2 = f.m2 + (t.m2 - f.m2) * u;
                    r[(size_t) k] = (float) svfTick (c, analog.region[ch], (double) x[k]);
                }
            }
            else
            {
                for (int k = 0; k < len; ++k)
                    r[(size_t) k] = (float) svfTick (t, analog.region[ch], (double) x[k]);
            }
            y = r;
            if (analog.tubeLive) analog.tube[ch].process (y.data(), len);
            if (analog.xfLive)   analog.xf[ch].process (y.data(), len);
            for (int k = 0; k < len; ++k)
            {
                const int idx = std::min (pos + k, kMaxChunk - 1);
                d[k] = (float) ((double) d[k] + fadeTrace[(size_t) idx] * ((double) y[(size_t) k] - (double) r[(size_t) k]));
            }
        };
        if (doA) run (a, da, 0);
        if (doB && b != nullptr) run (b, db, 1);
    }

private:
    enum class Mode { Off, FadingIn, On, FadingOut };

    struct Analog
    {
        bool live = false, tubeLive = false, xfLive = false, valid = false, moving = false;
        double fHz = 0.0, qVal = 0.0;     // to를 만든 설정 (로그 도메인)
        SvfCoeffs from, to;
        std::array<SvfState, 2> region {};
        std::array<TubeStage, 2> tube;
        std::array<TransformerBand, 2> xf;
    };

    void channelsFor (int nc, bool& doA, bool& doB) const noexcept
    {
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
    }

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
        analog.valid = false;                     // 모양 · 채널이 바뀌었을 수 있음 -> 영역 · 아날로그 상태도 새로
        for (auto& t : analog.tube) t.clearState();
        for (auto& x : analog.xf)   x.clearState();
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
    Analog analog;
    std::array<double, kMaxChunk> fadeTrace {};
    int traceIdx = 0;
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
//  엔진: 밴드 16개 -> 밴드 아날로그 -> EF804S(ALL) -> 트랜스포머(ALL) -> Mix -> Output (채널 1~2개)
//   32샘플(절대 시간 기준) 덩어리마다 밴드 상태 전환을 확인 -> 호스트 블록 크기와 상관없이 같은 결과
//   밴드 아날로그 (v0.7): EQ를 다 건 신호 x에서 밴드마다 자기 영역을 떼어 자기 튜브 · 트랜스포머로
//     출력 = x + Σ (아날로그_i(R_i x) - R_i x)   (밴드끼리 병렬, 밴드마다 드라이브 · 포커스 · 믹스 · 양이 다름)
//     M/S 밴드는 M 또는 S 신호에서 (그 채널에만 튜브), 모노는 Side 말고 전부 그 채널에서
//==============================================================================
class Engine
{
public:
    static constexpr int kChunk = BandProcessor::kMaxChunk;   // 32

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
        const double xa = p.xfAll ? 1.0 : 0.0;
        for (auto& x : xf)   x.setAmounts (xa * p.xfLow, xa * p.xfMid, xa * p.xfHigh);
        mix.target     = clamp01 (p.mix);
        outGain.target = dbToGain (std::clamp ((double) p.outputDb, -24.0, 24.0));
    }

    void reset() noexcept
    {
        for (auto& b : bands) b.reset();
        for (auto& t : tube)  t.reset();
        for (auto& x : xf)    x.reset();
        stageOn = { false, false };
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
                beginChunk();
            const int len = std::min (kChunk - pos, n - i);
            processChunk (L + i, R != nullptr ? R + i : nullptr, nc, len, pos);
            i += len;
            sampleCounter += (std::uint64_t) len;
        }
    }

    BandProcessor& getBand (int i) noexcept { return bands[(size_t) std::clamp (i, 0, kMaxBands - 1)]; }
    TubeStage& getTube (int c) noexcept { return tube[(size_t) std::clamp (c, 0, 1)]; }
    TransformerStage& getTransformer (int c) noexcept { return xf[(size_t) std::clamp (c, 0, 1)]; }

private:
    // 32샘플 덩어리 처음 (절대 시간): 밴드 상태 전환, 아날로그 · ALL 단계 사용 여부를 이 덩어리 동안 고정
    void beginChunk() noexcept
    {
        for (auto& b : bands)
        {
            b.updateState();
            b.beginChunk();
        }
        stageOn[0] = tube[0].isEngaged() || tube[1].isEngaged();
        stageOn[1] = xf[0].isEngaged()   || xf[1].isEngaged();
    }

    void processChunk (float* L, float* R, int nc, int n, int pos) noexcept
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

        applyBandAnalog (L, R, nc, n, pos);                       // 밴드마다의 튜브 · 트랜스포머
        if (stageOn[0]) { tube[0].process (L, n); if (R != nullptr) tube[1].process (R, n); }   // EF804S (ALL)
        if (stageOn[1]) { xf[0].process (L, n);   if (R != nullptr) xf[1].process (R, n); }     // 트랜스포머 (ALL)

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

    // 밴드 아날로그: 모두 EQ를 다 건 같은 신호에서 영역을 떼어(병렬) 바뀐 만큼을 모아 더함
    void applyBandAnalog (float* L, float* R, int nc, int n, int pos) noexcept
    {
        bool anyLR = false, anyMS = false;
        for (auto& b : bands)
            if (b.analogLive())
                (nc == 2 && isMsPlacement (b.placement()) ? anyMS : anyLR) = true;
        if (! anyLR && ! anyMS)
            return;

        float* dL = dBuf[0].data();
        float* dR = dBuf[1].data();
        std::fill (dL, dL + n, 0.0f);
        std::fill (dR, dR + n, 0.0f);

        if (anyLR)
            for (auto& b : bands)
                if (b.analogLive() && ! (nc == 2 && isMsPlacement (b.placement())))
                    b.applyAnalog (L, R, dL, dR, nc, n, pos);

        if (anyMS)
        {
            float* M  = msBuf[0].data();
            float* S  = msBuf[1].data();
            float* dM = msDelta[0].data();
            float* dS = msDelta[1].data();
            for (int k = 0; k < n; ++k)
            {
                M[k] = (float) (0.5 * ((double) L[k] + (double) R[k]));
                S[k] = (float) (0.5 * ((double) L[k] - (double) R[k]));
                dM[k] = dS[k] = 0.0f;
            }
            for (auto& b : bands)
                if (b.analogLive() && isMsPlacement (b.placement()))
                    b.applyAnalog (M, S, dM, dS, nc, n, pos);
            for (int k = 0; k < n; ++k)
            {
                dL[k] += dM[k] + dS[k];
                dR[k] += dM[k] - dS[k];
            }
        }

        for (int k = 0; k < n; ++k)
        {
            L[k] += dL[k];
            if (R != nullptr) R[k] += dR[k];
        }
    }

    void applySmoothing() noexcept
    {
        mix.setTimeConstant (smoothingSeconds, fs);      mix.eps = 1.0e-6;
        outGain.setTimeConstant (smoothingSeconds, fs);  outGain.eps = 1.0e-7;
    }

    double fs = 48000.0, smoothingSeconds = 0.015;
    std::array<bool, 2> stageOn { false, false };
    std::array<std::array<float, kChunk>, 2> dBuf {}, msBuf {}, msDelta {};
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
//   포함: 밴드, 트랜스포머(ALL)의 고역 셸프(선형 부분), Mix 블렌드
//   제외: 튜브 · 트랜스포머의 비선형 성분 (밴드 아날로그 포함, 기준 레벨에서 이득 1), Output
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
                                      -TransformerStage::kShelfMaxCutDb * (p.xfAll ? clamp01 (p.xfHigh) : 0.0), fs);
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
