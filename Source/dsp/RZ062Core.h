// =============================================================================
//  Pillow RZ EQ — DSP 코어 v0.3 (JUCE 비의존, C++17 헤더 온리)
//
//  Klangfilm(Siemens) RZ062a/b 진공관 EQ 커브 + 밴드별 Q,
//  Plugin Alliance(NEOLD) RZ062를 참고한 확장:
//   - 모듈 2개 (L/R 또는 Mid/Side, 링크), 모듈마다 A(Pivot) / B(Presence)
//   - 튜브: Telefunken EF804S(3극관 결선) Koren 모델로 계산한 실제 전달곡선
//           Drive(0~48 dB, 기준 레벨 보상), Focus(홀수 <-> 짝수 배음), Mix
//   - 출력 트랜스포머 색채: 저역 / 중역 / 고역 따로 조절
//
//  처리 순서 (모듈마다):
//    [L/R 또는 M/S 인코드] -> EQ -> EF804S -> 트랜스포머 -> Mix(원음) -> Level -> [디코드]
//  ZERO LATENCY 모드는 세션 레이트에서 그대로(비선형은 ADAA), HQ 모드는 오버샘플된 레이트에서 처리
//
//  필터는 Cytomic(Andrew Simper) 선형 사다리꼴 SVF. 사다리꼴 적분 = 쌍선형 변환이라
//  GUI 응답 곡선을 실제 처리와 같은 계수로 정확히 계산할 수 있음 (ResponseModel).
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace pillow::rz
{
inline constexpr double kPi = 3.14159265358979323846;

//==============================================================================
//  EQ 상수 (Audified·NEOLD 매뉴얼, GroupDIY 실측; 추정치는 README에 표시)
//==============================================================================
inline constexpr double kBassRefHz     = 40.0;
inline constexpr double kTrebleRefHz   = 10000.0;
inline constexpr double kBassShelfHz   = 160.0;    // 셸프 중간점 (추정)
inline constexpr double kTrebleShelfHz = 2500.0;   // 650 Hz 축 대칭: 160 x 2500 = 40 x 10000
inline constexpr double kTiltPivotHz   = 650.0;
inline constexpr std::array<double, 4> kPresenceHz { 1400.0, 2000.0, 2800.0, 4000.0 };

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

inline std::vector<double> trebleDetents()
{
    std::vector<double> v;
    for (int i = -9; i <= 6; ++i) v.push_back (2.0 * i);
    return v;
}

inline std::vector<double> bassDetents()
{
    std::vector<double> v;
    for (int i = -10; i <= -2; ++i) v.push_back (2.0 * i);   // -20 ... -4
    for (int i = 0; i <= 6; ++i)    v.push_back (2.0 * i);   // 0 ... +12
    return v;
}

inline std::vector<double> tiltDetents()
{
    std::vector<double> v;
    for (int i = -4; i <= 4; ++i) v.push_back ((double) i * kMidStepDb);
    return v;
}

inline std::vector<double> presenceDetents()
{
    std::vector<double> v;
    for (int i = 0; i <= 6; ++i) v.push_back ((double) i * kMidStepDb);
    return v;
}

//==============================================================================
//  파라미터
//==============================================================================
enum class Mode    { A_Tilt = 0, B_Presence = 1 };   // A = Pivot(틸트), B = Presence
enum class Routing { Stereo = 0, MidSide = 1 };

// 모듈 하나(= 원본 RZ062 한 대)의 설정
struct ModuleParams
{
    Mode  mode      = Mode::B_Presence;
    float trebleDb  = 0.0f, trebleQ = kVintageShelfQ;
    float bassDb    = 0.0f, bassQ   = kVintageShelfQ;
    float tiltDb    = 0.0f;           // + = 밝게 (고역 업 / 저역 다운)
    int   presIndex = 2;              // 0..3 -> 1.4 / 2 / 2.8 / 4 kHz
    float presDb    = 0.0f, presQ = kVintagePresQ;
    float driveDb   = 0.0f;           // EF804S 드라이브 0..48 dB
    float outputDb  = 0.0f;           // 모듈 출력 레벨

    bool operator== (const ModuleParams& o) const noexcept
    {
        return mode == o.mode && presIndex == o.presIndex
            && sameValue (trebleDb, o.trebleDb) && sameValue (trebleQ, o.trebleQ)
            && sameValue (bassDb, o.bassDb)     && sameValue (bassQ, o.bassQ)
            && sameValue (tiltDb, o.tiltDb)
            && sameValue (presDb, o.presDb)     && sameValue (presQ, o.presQ)
            && sameValue (driveDb, o.driveDb)   && sameValue (outputDb, o.outputDb);
    }
    bool operator!= (const ModuleParams& o) const noexcept { return ! (*this == o); }
};

struct Params
{
    Routing routing = Routing::Stereo;
    bool  link    = true;     // 켜면 모듈 2가 모듈 1 설정을 그대로 따름
    bool  stepped = false;    // 원본처럼 계단식 게인
    bool  eqOn    = true;
    bool  driveOn = false;
    float focus   = 0.0f;     // -1 = 홀수 배음(대칭)  0 = EF804S 그대로  +1 = 짝수 배음 2배
    float mix     = 1.0f;     // 0..1 (원음 블렌드)
    float xfLow = 0.0f, xfMid = 0.0f, xfHigh = 0.0f;   // 트랜스포머 색채 0..1 (구간별)
    std::array<ModuleParams, 2> mod {};

    const ModuleParams& effective (int m) const noexcept { return (link || m <= 0) ? mod[0] : mod[1]; }

    bool operator== (const Params& o) const noexcept
    {
        return routing == o.routing && link == o.link && stepped == o.stepped
            && eqOn == o.eqOn && driveOn == o.driveOn
            && sameValue (focus, o.focus) && sameValue (mix, o.mix)
            && sameValue (xfLow, o.xfLow) && sameValue (xfMid, o.xfMid) && sameValue (xfHigh, o.xfHigh)
            && mod[0] == o.mod[0] && mod[1] == o.mod[1];
    }
    bool operator!= (const Params& o) const noexcept { return ! (*this == o); }
};

// 모듈에 실제로 들어가는 값 (링크 · 스텝 · 모드 · EQ on/off 반영)
struct ResolvedModule
{
    double trebleDb, trebleQ, bassDb, bassQ, tiltDb, presHz, presDb, presQ, driveDb, outputDb;
};

inline ResolvedModule resolveModule (const Params& p, int module) noexcept
{
    const ModuleParams& m = p.effective (module);
    const bool st  = p.stepped;
    const bool isB = (m.mode == Mode::B_Presence);
    const bool eq  = p.eqOn;

    ResolvedModule r {};
    r.trebleDb = ! eq ? 0.0 : (st ? quantizeTreble (m.trebleDb) : std::clamp (m.trebleDb, kTrebleMinDb, kTrebleMaxDb));
    r.bassDb   = ! eq ? 0.0 : (st ? quantizeBass (m.bassDb)     : std::clamp (m.bassDb, kBassMinDb, kBassMaxDb));
    r.trebleQ  = std::clamp (m.trebleQ, kShelfQMin, kShelfQMax);
    r.bassQ    = std::clamp (m.bassQ,   kShelfQMin, kShelfQMax);

    // 가운데 밴드: 모드에 따라 한쪽 게인을 0으로 (스무딩으로 자연스럽게 크로스페이드)
    r.tiltDb = (! eq || isB) ? 0.0 : (st ? quantizeTilt (m.tiltDb) : std::clamp (m.tiltDb, -kTiltMaxDb, kTiltMaxDb));
    r.presDb = (! eq || ! isB) ? 0.0 : (st ? quantizePresence (m.presDb) : std::clamp (m.presDb, 0.0f, kPresMaxDb));
    r.presHz = kPresenceHz[(size_t) std::clamp (m.presIndex, 0, 3)];
    r.presQ  = std::clamp (m.presQ, kPresQMin, kPresQMax);

    r.driveDb  = std::clamp (m.driveDb, 0.0f, kDriveMaxDb);
    r.outputDb = std::clamp (m.outputDb, -24.0f, 24.0f);
    return r;
}

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

// 벨 (makeBell과 같은 아날로그 원형: (s^2 + s A/Q + 1) / (s^2 + s /(Q A) + 1))
inline SvfCoeffs makeBellMatched (double fc, double q, double gainDb, double fs) noexcept
{
    fc = clampFreq (fc, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0T = 2.0 * kPi * fc / fs;
    double a1, a2;
    mapRoots (w0T, 1.0 / (2.0 * q * A), a1, a2);
    if (sameValue (gainDb, 0.0)) return identitySvf (a1, a2);   // EQ off = 비트 단위 동일

    const double W = 0.5 * fs / fc, re = 1.0 - W * W, nu = A * W / q, de = W / (q * A);
    const double hN = (re * re + nu * nu) / (re * re + de * de);
    return matchedSvf (a1, a2, hN, w0T, A * A * A * A);
}

// 하이 셸프 (makeHighShelf와 같은 원형: A (A s^2 + s sqrt(A)/Q + 1) / (s^2 + s sqrt(A)/Q + A))
inline SvfCoeffs makeHighShelfMatched (double fc, double q, double gainDb, double fs) noexcept
{
    fc = clampFreq (fc, fs);
    const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
    const double wcT = 2.0 * kPi * fc / fs;
    if (std::max (wcT * sA, wcT / sA) > 0.9 * kPi)   // 근이 나이퀴스트 근처/위 (아주 낮은 레이트) -> 쌍선형
        return makeHighShelf (fc, q, gainDb, fs);
    double a1, a2;
    mapRoots (wcT * sA, 0.5 / q, a1, a2);             // 극점: wc sqrt(A)
    if (sameValue (gainDb, 0.0)) return identitySvf (a1, a2);

    auto mag2 = [&] (double W) {                       // 아날로그 |H(jW)|^2, W = f / fc
        const double r = sA * W / q, n = 1.0 - A * W * W, d = A - W * W;
        return A * A * (n * n + r * r) / (d * d + r * r);
    };
    const double Wz = 1.0 / sA;                         // 영점 주파수 (셸프 굴곡이 있는 곳)
    return matchedSvf (a1, a2, mag2 (0.5 * fs / fc), wcT * Wz, mag2 (Wz));
}

//==============================================================================
//  A 모듈 Pivot(틸트): 1차 셸프를 650 Hz 중심으로 회전
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
//  EQ 섹션 (채널 1개)
//   16샘플마다 스무딩된 목표 계수를 다시 계산하고, 그 사이는 샘플 단위로 선형 보간
//==============================================================================
class EqSection
{
public:
    EqSection() { setTargets (resolveModule (Params {}, 0)); reset(); }

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s) { smoothingSeconds = std::max (0.0, s); applySmoothing(); }

    void setTargets (const ResolvedModule& r) noexcept
    {
        setTarget (sTrebleDb, r.trebleDb);
        setTarget (sTrebleQ,  std::log (r.trebleQ));
        setTarget (sBassDb,   r.bassDb);
        setTarget (sBassQ,    std::log (r.bassQ));
        setTarget (sTiltDb,   r.tiltDb);
        setTarget (sPresDb,   r.presDb);
        setTarget (sPresHz,   std::log (r.presHz));
        setTarget (sPresQ,    std::log (r.presQ));
    }

    void reset() noexcept
    {
        for (auto* s : smoothers()) s->snap();
        target  = computeBank();
        base    = target;
        ramping = false;
        dirty   = false;
        samplesToNextUpdate = 0;
        rampPos = 0;
        clearState();
    }

    void clearState() noexcept { st = State {}; }

    void process (float* x, int n) noexcept
    {
        int i = 0;
        while (i < n)
        {
            if (samplesToNextUpdate <= 0)
                beginSubBlock();

            const int len = std::min (samplesToNextUpdate, n - i);
            float* p = x + i;

            if (ramping)
            {
                for (int k = 0; k < len; ++k)
                {
                    const double t = (double) (rampPos + k + 1);
                    double v = p[k];
                    v = svfTick (lerpSvf (base.bass,   step.bass,   t), st.bass,   v);
                    v = svfTick (lerpSvf (base.treble, step.treble, t), st.treble, v);
                    v = svfTick (lerpSvf (base.pres,   step.pres,   t), st.pres,   v);
                    v = tiltTick (lerpTilt (base.tilt, step.tilt,   t), st.tilt,   v);
                    p[k] = (float) v;
                }
            }
            else
            {
                for (int k = 0; k < len; ++k)
                {
                    double v = p[k];
                    v = svfTick (base.bass,   st.bass,   v);
                    v = svfTick (base.treble, st.treble, v);
                    v = svfTick (base.pres,   st.pres,   v);
                    v = tiltTick (base.tilt,  st.tilt,   v);
                    p[k] = (float) v;
                }
            }

            rampPos += len;
            samplesToNextUpdate -= len;
            i += len;
        }
    }

private:
    static constexpr int kSubBlock = 16;

    struct State { SvfState bass, treble, pres; OnePoleState tilt; };
    struct Bank  { SvfCoeffs treble, bass, pres; TiltCoeffs tilt; };

    static SvfCoeffs diffSvf (const SvfCoeffs& to, const SvfCoeffs& from, double n) noexcept
    {
        SvfCoeffs d;
        d.a1 = (to.a1 - from.a1) / n;  d.a2 = (to.a2 - from.a2) / n;  d.a3 = (to.a3 - from.a3) / n;
        d.m0 = (to.m0 - from.m0) / n;  d.m1 = (to.m1 - from.m1) / n;  d.m2 = (to.m2 - from.m2) / n;
        return d;
    }
    static SvfCoeffs lerpSvf (const SvfCoeffs& b, const SvfCoeffs& d, double t) noexcept
    {
        SvfCoeffs c;
        c.a1 = b.a1 + d.a1 * t;  c.a2 = b.a2 + d.a2 * t;  c.a3 = b.a3 + d.a3 * t;
        c.m0 = b.m0 + d.m0 * t;  c.m1 = b.m1 + d.m1 * t;  c.m2 = b.m2 + d.m2 * t;
        return c;
    }
    static TiltCoeffs diffTilt (const TiltCoeffs& to, const TiltCoeffs& from, double n) noexcept
    {
        TiltCoeffs d;
        d.G1 = (to.G1 - from.G1) / n;
        d.lfMinus1 = (to.lfMinus1 - from.lfMinus1) / n;
        d.outGain = (to.outGain - from.outGain) / n;
        return d;
    }
    static TiltCoeffs lerpTilt (const TiltCoeffs& b, const TiltCoeffs& d, double t) noexcept
    {
        TiltCoeffs c;
        c.G1 = b.G1 + d.G1 * t;
        c.lfMinus1 = b.lfMinus1 + d.lfMinus1 * t;
        c.outGain = b.outGain + d.outGain * t;
        return c;
    }

    std::array<Smoother*, 8> smoothers() noexcept
    {
        return { &sTrebleDb, &sTrebleQ, &sBassDb, &sBassQ, &sTiltDb, &sPresDb, &sPresHz, &sPresQ };
    }

    void applySmoothing() noexcept
    {
        const double ctrlRate = fs / kSubBlock;
        for (auto* s : smoothers()) s->setTimeConstant (smoothingSeconds, ctrlRate);
        sTrebleDb.eps = sBassDb.eps = sTiltDb.eps = sPresDb.eps = 1.0e-4;   // dB
        sTrebleQ.eps = sBassQ.eps = sPresHz.eps = sPresQ.eps = 1.0e-6;      // log 도메인
    }

    void setTarget (Smoother& s, double v) noexcept
    {
        if (! sameValue (s.target, v))
        {
            s.target = v;
            dirty = true;
        }
    }

    void beginSubBlock() noexcept
    {
        base = target;
        if (dirty)
        {
            bool settled = true;
            for (auto* s : smoothers())
            {
                s->step();
                settled = settled && s->isSettled();
            }
            target = computeBank();
            const double n = (double) kSubBlock;
            step.treble = diffSvf (target.treble, base.treble, n);
            step.bass   = diffSvf (target.bass,   base.bass,   n);
            step.pres   = diffSvf (target.pres,   base.pres,   n);
            step.tilt   = diffTilt (target.tilt,  base.tilt,   n);
            ramping = true;
            dirty = ! settled;
        }
        else
        {
            ramping = false;
        }
        rampPos = 0;
        samplesToNextUpdate = kSubBlock;
    }

    Bank computeBank() const noexcept
    {
        Bank b;
        b.treble = makeHighShelfMatched (kTrebleShelfHz, std::exp (sTrebleQ.current), sTrebleDb.current, fs);
        b.bass   = makeLowShelf  (kBassShelfHz,   std::exp (sBassQ.current),   sBassDb.current,   fs);
        b.pres   = makeBellMatched (std::exp (sPresHz.current), std::exp (sPresQ.current), sPresDb.current, fs);
        b.tilt   = makeTilt      (kTiltPivotHz, sTiltDb.current, fs);
        return b;
    }

    double fs = 48000.0, smoothingSeconds = 0.015;
    bool dirty = false, ramping = false;
    int samplesToNextUpdate = 0, rampPos = 0;
    Smoother sTrebleDb, sTrebleQ, sBassDb, sBassQ, sTiltDb, sPresDb, sPresHz, sPresQ;
    Bank base, step, target;
    State st;
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

    // T(v) - v. 범위 밖은 포화(평탄)로 연장
    double dev (double v) const noexcept
    {
        if (v >  kVmax) return tEndPos - v;
        if (v < -kVmax) return tEndNeg - v;
        return lookup (devTable, v);
    }

    // 적분 0..v (T(u) - u) du. 범위 밖은 평탄 곡선의 적분으로 연장
    double psi (double v) const noexcept
    {
        if (v >  kVmax) return psiEndPos + tEndPos * (v - kVmax) - 0.5 * (v * v - kVmax * kVmax);
        if (v < -kVmax) return psiEndNeg + tEndNeg * (v + kVmax) - 0.5 * (v * v - kVmax * kVmax);
        return lookup (psiTable, v);
    }

    // dev(v), dev(-v), psi(v), psi(-v)를 한 번에 (오디오 루프용: asinh 1번)
    void evalPair (double v, double& devPos, double& devNeg, double& psiPos, double& psiNeg) const noexcept
    {
        if (std::abs (v) > kVmax)
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

        tEndPos   =  kVmax + devTable[(size_t) kSize - 1];
        tEndNeg   = -kVmax + devTable[0];
        psiEndPos = psiTable[(size_t) kSize - 1];
        psiEndNeg = psiTable[0];
    }

    Table devTable {}, psiTable {};
    double wMax = 1.0, invDw = 1.0, rk = 0.0, vp0 = 0.0, gain = 1.0;
    double tEndPos = 0.0, tEndNeg = 0.0, psiEndPos = 0.0, psiEndNeg = 0.0;
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
    static constexpr double kSlowSeconds = 0.040;   // 원신호에 직접 거는 N은 한 번 더 평균 (저역 리플 -> DC 방지)

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

//==============================================================================
//  튜브 스테이지 (채널 1개)
//   - Drive: 그리드에 들어가는 신호 크기 (0 dB = 풀스케일이 그리드 0.5 Vpk, 거의 깨끗)
//   - Focus: 전달곡선의 홀수 성분 + (1 + focus) x 짝수 성분
//   - 보상: -12 dBFS 사인의 기본파 레벨이 드라이브와 상관없이 같도록 정규화
//   - 1차 ADAA + 선형 성분 정렬(AdaaAligned): 오버샘플링 없이도 에일리어싱을 크게 줄임 -> 레이턴시 0
//     원신호는 그대로 두고 "왜곡 성분(y - x)"에만 ADAA와 DC 블로커를 적용
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
        adaa.prepare (fs);
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

    void clearState() noexcept { dcX1 = dcY1 = 0.0; adaa.reset(); }

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
                    double h, H;                   // 곡선이 바뀌었으니 직전 입력의 값도 새 곡선으로
                    shapeAt (adaa.lastInput(), h, H);
                    adaa.rebase (h, H);
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
                    double h, H;
                    shapeAt (in, h, H);
                    const auto sp = adaa.tickSplit (in, h, H);

                    // 순수 왜곡(배음 + 비대칭 DC)만 DC 블로커(5 Hz)로. 선형 성분(압축 + 레벨 보상)은 그대로
                    // -> 무거운 드라이브에서도 저역 압축의 위상이 틀어지지 않음
                    const double d  = norm * sp.distortion;
                    const double hp = d - dcX1 + dcR * dcY1;
                    dcX1 = d;
                    dcY1 = hp;
                    const double lin = norm * (1.0 + sp.slope) - 1.0;
                    p[k] = (float) (in + wetNow * (hp + lin * in));
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

    // h(x) = shape(x) - x 와 H(x) = 적분 0..x h(u) du 를 한 번에
    void shapeAt (double x, double& h, double& H) const noexcept
    {
        double dp, dn, pp, pn;
        curve->evalPair (x * scale, dp, dn, pp, pn);
        h = (0.5 * (dp - dn) + evenMul * 0.5 * (dp + dn)) * invScale;
        H = (0.5 * (pp + pn) + evenMul * 0.5 * (pp - pn)) * invScale * invScale;
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
    AdaaAligned adaa;
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

                    const auto sl = bands[0].tickSplit (lo,  hLow (lo),   HLow (lo));
                    const auto sm = bands[1].tickSplit (mid, hMid (mid),  HMid (mid));
                    const auto sh = bands[2].tickSplit (hi,  hHigh (hi),  HHigh (hi));

                    const double d  = curLow * sl.distortion + curMid * sm.distortion + curHigh * sh.distortion;
                    const double hp = d - dcX1 + dcR * dcY1;     // DC 블로커는 순수 왜곡에만
                    dcX1 = d;
                    dcY1 = hp;
                    const double lin = curLow * sl.slope * c1 + curMid * sm.slope * c2 + curHigh * sh.slope * c3;

                    p[k] = (float) (svfTick (shelf, shelfState, in) + hp + lin);
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
    SvfState shelfState;
    Smoother aLow, aMid, aHigh;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  모듈 체인 (채널 1개): EQ -> EF804S -> 트랜스포머 -> Mix -> Level
//==============================================================================
class ModuleChain
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        eq.prepare (fs);
        tube.prepare (fs);
        xf.prepare (fs);
        dry.assign ((size_t) std::max (1, maxBlock), 0.0f);
        applySmoothing();
        reset();
    }

    void setSmoothingSeconds (double s)
    {
        smoothingSeconds = std::max (0.0, s);
        eq.setSmoothingSeconds (s);
        tube.setSmoothingSeconds (s);
        xf.setSmoothingSeconds (s);
        applySmoothing();
    }

    void setTargets (const ResolvedModule& r, const Params& p) noexcept
    {
        eq.setTargets (r);
        tube.setTargets (p.driveOn, r.driveDb, p.focus);
        xf.setAmounts (p.xfLow, p.xfMid, p.xfHigh);
        mix.target = clamp01 (p.mix);
        outGain.target = dbToGain (r.outputDb);
    }

    void reset() noexcept
    {
        eq.reset();
        tube.reset();
        xf.reset();
        mix.snap();
        outGain.snap();
    }

    void clearState() noexcept
    {
        eq.clearState();
        tube.clearState();
        xf.clearState();
    }

    void process (float* x, int n) noexcept
    {
        const int cap = (int) dry.size();
        int i = 0;
        while (i < n)
        {
            const int len = std::min (cap, n - i);
            float* p = x + i;

            const bool useMix = ! (mix.isSettled() && sameValue (mix.current, 1.0));
            if (useMix)
                std::copy (p, p + len, dry.begin());

            eq.process (p, len);
            tube.process (p, len);
            xf.process (p, len);

            if (useMix)
                for (int k = 0; k < len; ++k)
                {
                    const double m = mix.step();
                    p[k] = (float) ((double) dry[(size_t) k] + m * ((double) p[k] - (double) dry[(size_t) k]));
                }

            if (! (outGain.isSettled() && sameValue (outGain.current, 1.0)))
                for (int k = 0; k < len; ++k)
                    p[k] = (float) ((double) p[k] * outGain.step());

            i += len;
        }
    }

    // 테스트용 접근
    EqSection& getEq() noexcept { return eq; }
    TubeStage& getTube() noexcept { return tube; }
    TransformerStage& getTransformer() noexcept { return xf; }

private:
    void applySmoothing() noexcept
    {
        mix.setTimeConstant (smoothingSeconds, fs);      mix.eps = 1.0e-6;
        outGain.setTimeConstant (smoothingSeconds, fs);  outGain.eps = 1.0e-7;
    }

    double fs = 48000.0, smoothingSeconds = 0.015;
    EqSection eq;
    TubeStage tube;
    TransformerStage xf;
    Smoother mix, outGain;
    std::vector<float> dry = std::vector<float> (512, 0.0f);
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
//  엔진: 라우팅(L/R, M/S) + 모듈 2개
//   라우팅 전환은 5 ms 페이드아웃 -> 전환(필터 상태 초기화) -> 5 ms 페이드인
//==============================================================================
class Engine
{
public:
    static constexpr double kDuckSeconds = 0.005;

    Engine() { setParams (Params {}); }

    void prepare (double processingRate, int numChannels, int maxBlock)
    {
        fs = processingRate > 0.0 ? processingRate : 48000.0;
        numCh = std::clamp (numChannels, 1, 2);
        for (auto& m : modules) m.prepare (fs, maxBlock);
        duckStep = 1.0 / std::max (1.0, kDuckSeconds * fs);
        reset();
    }

    double getProcessingRate() const noexcept { return fs; }

    void setSmoothingMs (double ms)
    {
        for (auto& m : modules) m.setSmoothingSeconds (std::max (0.0, ms) * 0.001);
    }

    void setParams (const Params& p) noexcept
    {
        modules[0].setTargets (resolveModule (p, 0), p);
        modules[1].setTargets (resolveModule (p, 1), p);
        targetRouting = p.routing;
    }

    void reset() noexcept
    {
        for (auto& m : modules) m.reset();
        currentRouting = targetRouting;
        duckGain = 1.0;
    }

    void process (float* const* ch, int numChannels, int n) noexcept
    {
        const int nc = std::min (numChannels, numCh);
        if (nc <= 0 || n <= 0)
            return;

        if (nc == 1)
        {
            modules[0].process (ch[0], n);
            return;
        }

        int i = 0;
        while (i < n)
        {
            int len = n - i, dir = 0;

            if (currentRouting != targetRouting)
            {
                if (duckGain <= 0.0)
                {
                    currentRouting = targetRouting;   // 무음 상태에서 전환
                    for (auto& m : modules) m.clearState();
                    continue;
                }
                len = std::min (len, std::max (1, (int) std::ceil (duckGain / duckStep - 1.0e-9)));
                dir = -1;
            }
            else if (duckGain < 1.0)
            {
                len = std::min (len, std::max (1, (int) std::ceil ((1.0 - duckGain) / duckStep - 1.0e-9)));
                dir = +1;
            }

            processSpan (ch[0] + i, ch[1] + i, len, dir);
            i += len;
        }
    }

    ModuleChain& getModule (int m) noexcept { return modules[(size_t) std::clamp (m, 0, 1)]; }

private:
    void processSpan (float* L, float* R, int n, int dir) noexcept
    {
        const bool ms = (currentRouting == Routing::MidSide);
        if (ms)
            for (int k = 0; k < n; ++k)
            {
                const double l = L[k], r = R[k];
                L[k] = (float) (0.5 * (l + r));
                R[k] = (float) (0.5 * (l - r));
            }

        modules[0].process (L, n);
        modules[1].process (R, n);

        if (ms)
            for (int k = 0; k < n; ++k)
            {
                const double m = L[k], s = R[k];
                L[k] = (float) (m + s);
                R[k] = (float) (m - s);
            }

        if (dir != 0)
            for (int k = 0; k < n; ++k)
            {
                duckGain = clamp01 (duckGain + (double) dir * duckStep);
                L[k] = (float) ((double) L[k] * duckGain);
                R[k] = (float) ((double) R[k] * duckGain);
            }
    }

    double fs = 48000.0, duckGain = 1.0, duckStep = 0.001;
    int numCh = 2;
    Routing currentRouting = Routing::Stereo, targetRouting = Routing::Stereo;
    std::array<ModuleChain, 2> modules;
};

//==============================================================================
//  GUI 응답 곡선 (실제 처리와 같은 계수로 계산)
//   포함: EQ, 트랜스포머 고역 셸프(선형 부분), Mix 블렌드
//   제외: 튜브/트랜스포머 비선형 성분(기준 레벨에서 이득 1), 모듈 Level
//==============================================================================
class ResponseModel
{
public:
    ResponseModel (const Params& p, int module, double sampleRate) noexcept
        : fs (sampleRate > 0.0 ? sampleRate : 48000.0)
    {
        const ResolvedModule r = resolveModule (p, module);
        treble = makeHighShelfMatched (kTrebleShelfHz, r.trebleQ, r.trebleDb, fs);
        bass   = makeLowShelf  (kBassShelfHz,   r.bassQ,   r.bassDb,   fs);
        pres   = makeBellMatched (r.presHz, r.presQ, r.presDb, fs);
        tilt   = makeTilt      (kTiltPivotHz, r.tiltDb, fs);
        shelf  = makeHighShelfMatched (TransformerStage::kShelfHz, TransformerStage::kShelfQ,
                                -TransformerStage::kShelfMaxCutDb * clamp01 (p.xfHigh), fs);
        mix    = clamp01 (p.mix);
    }

    double magnitudeDb (double freqHz) const noexcept
    {
        const auto wet = svfResponse (treble, freqHz, fs) * svfResponse (bass, freqHz, fs)
                       * svfResponse (pres, freqHz, fs) * tiltResponse (tilt, freqHz, fs)
                       * svfResponse (shelf, freqHz, fs);
        const auto h = (1.0 - mix) + mix * wet;
        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
    }

private:
    double fs, mix = 1.0;
    SvfCoeffs treble, bass, pres, shelf;
    TiltCoeffs tilt;
};

} // namespace pillow::rz
