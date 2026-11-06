// =============================================================================
//  Pillow RZ EQ — DSP 코어 v0.2 (JUCE 비의존, C++17 헤더 온리)
//
//  Klangfilm(Siemens) RZ062a/b 진공관 EQ 커브 + 밴드별 Q,
//  Plugin Alliance(NEOLD) RZ062를 참고한 확장:
//   - 모듈 2개 (L/R 또는 Mid/Side, 링크), 모듈마다 A(Pivot) / B(Presence)
//   - 튜브: Telefunken EF804S(3극관 결선) Koren 모델로 계산한 실제 전달곡선
//           Drive(0~48 dB, 기준 레벨 보상), Focus(홀수 <-> 짝수 배음), Mix
//   - 출력 트랜스포머 색채: 저역 / 중역 / 고역 따로 조절
//
//  처리 순서 (모듈마다, 오버샘플된 레이트):
//    [L/R 또는 M/S 인코드] -> EQ -> EF804S -> 트랜스포머 -> Mix(원음) -> Level -> [디코드]
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
        b.treble = makeHighShelf (kTrebleShelfHz, std::exp (sTrebleQ.current), sTrebleDb.current, fs);
        b.bass   = makeLowShelf  (kBassShelfHz,   std::exp (sBassQ.current),   sBassDb.current,   fs);
        b.pres   = makeBell      (std::exp (sPresHz.current), std::exp (sPresQ.current), sPresDb.current, fs);
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
    double operator() (double v) const noexcept
    {
        const double w = std::asinh (v / kV0);
        const double pos = std::clamp ((w + wMax) * invDw, 1.0, (double) kSize - 3.0);
        const int i = (int) pos;
        const double t = pos - (double) i;
        const double p0 = table[(size_t) i - 1], p1 = table[(size_t) i];
        const double p2 = table[(size_t) i + 1], p3 = table[(size_t) i + 2];
        return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
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

    double cathodeResistor() const noexcept { return rk; }
    double idlePlateVoltage() const noexcept { return vp0; }
    double stageGain() const noexcept { return gain; }

private:
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

        // 3) asinh 워핑 격자에 T(v) 저장
        wMax  = std::asinh (kVmax / kV0);
        const double dw = 2.0 * wMax / (double) (kSize - 1);
        invDw = 1.0 / dw;
        for (int k = 0; k < kSize; ++k)
        {
            const double w = -wMax + dw * (double) k;
            const double v = (k == (kSize - 1) / 2) ? 0.0 : kV0 * std::sinh (w);
            const double vp = kBplus - stageCurrent (v, rk) * kRl;
            table[(size_t) k] = (vp0 - vp) / gain;
        }
        table[(size_t) (kSize - 1) / 2] = 0.0;
    }

    std::array<double, kSize> table {};
    double wMax = 1.0, invDw = 1.0, rk = 0.0, vp0 = 0.0, gain = 1.0;
};

// 프로세스 전체에서 한 번만 계산 (prepare 때 먼저 불러서 오디오 스레드에서 생성되지 않게 함)
inline const Ef804sCurve& ef804sCurve()
{
    static const Ef804sCurve curve;
    return curve;
}

//==============================================================================
//  튜브 스테이지 (채널 1개, 오버샘플된 레이트)
//   - Drive: 그리드에 들어가는 신호 크기 (0 dB = 풀스케일이 그리드 0.5 Vpk, 거의 깨끗)
//   - Focus: 전달곡선의 홀수 성분 + (1 + focus) x 짝수 성분
//   - 보상: -12 dBFS 사인의 기본파 레벨이 드라이브와 상관없이 같도록 정규화
//   - DC 블로커는 "왜곡 성분(y - x)"에만 -> 원신호 저역은 그대로
//   - Drive off면 완전 바이패스 (비트 단위 동일)
//==============================================================================
class TubeStage
{
public:
    static constexpr double kGridVoltsPerFullScale = 0.5;
    static constexpr double kRefAmp = 0.25;   // -12 dBFS

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

    void clearState() noexcept { dcX1 = dcY1 = 0.0; }

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
                    const double r  = shape (in) * norm - in;     // 왜곡 성분
                    const double hp = r - dcX1 + dcR * dcY1;      // DC 블로커 (5 Hz)
                    dcX1 = r;
                    dcY1 = hp;
                    p[k] = (float) (in + wetNow * hp);
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

    // 테스트용: 현재 설정의 정적 곡선 (보상 포함)
    double transfer (double x) const noexcept { return shape (x) * norm; }

private:
    static constexpr int kChunk = 32, kDfPoints = 32;

    double shape (double x) const noexcept
    {
        const double v  = x * scale;
        const double tp = (*curve) (v), tn = (*curve) (-v);
        return (0.5 * (tp - tn) + evenMul * 0.5 * (tp + tn)) * invScale;
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
            sum += shape (kRefAmp * sinTable[(size_t) k]) * sinTable[(size_t) k];
        const double df = (2.0 / (double) kDfPoints) * sum / kRefAmp;
        norm = df > 1.0e-9 ? 1.0 / df : 1.0;
    }

    const Ef804sCurve* curve = &ef804sCurve();
    std::array<double, kDfPoints> sinTable {};
    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double wetNow = 0.0, scale = kGridVoltsPerFullScale, invScale = 1.0 / kGridVoltsPerFullScale;
    double evenMul = 1.0, norm = 1.0, dcX1 = 0.0, dcY1 = 0.0;
    Smoother wet, drive, foc;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  출력 트랜스포머 색채 — 저역 / 중역 / 고역 따로
//   LR4 크로스오버(200 Hz, 4 kHz)로 나눈 각 구간에서 "왜곡 성분"만 만들어 원신호에 더함
//   -> 모든 양이 0이면 완전 바이패스, 크로스오버 위상이 원신호에 묻지 않음
//   LOW : 코어 포화 (대칭, 3차 위주, 큰 저음일수록 진해짐)
//   MID : 은은한 밀도 (약한 비대칭, 2차+3차)
//   HIGH: 고역 포화 + 12 kHz 위를 최대 -1.5 dB 부드럽게 (누설 인덕턴스/권선 용량 느낌)
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
        dcR  = std::exp (-2.0 * kPi * 5.0 / fs);
        midT = std::tanh (kMidBias);
        midNorm = 1.0 / (kMidDrive * (1.0 - midT * midT));
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
        dcX1 = dcY1 = 0.0;
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

                    const double r = curLow  * (std::tanh (kLowDrive * lo) / kLowDrive - lo)
                                   + curMid  * ((std::tanh (kMidDrive * mid + kMidBias) - midT) * midNorm - mid)
                                   + curHigh * (std::tanh (kHighDrive * hi) / kHighDrive - hi);

                    const double hp = r - dcX1 + dcR * dcY1;
                    dcX1 = r;
                    dcY1 = hp;

                    p[k] = (float) (svfTick (shelf, shelfState, in) + hp);
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
        shelf   = makeHighShelf (kShelfHz, kShelfQ, -kShelfMaxCutDb * curHigh, fs);
    }

    double fs = 48000.0, dcR = 0.9999, smoothingSeconds = 0.02;
    double midT = 0.0, midNorm = 1.0, curLow = 0.0, curMid = 0.0, curHigh = 0.0;
    double dcX1 = 0.0, dcY1 = 0.0;
    bool active = false;
    SvfCoeffs lpLo, hpLo, lpHi, hpHi, shelf;
    std::array<SvfState, 8> xs {};
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
//  오버샘플링 배율 (플러그인과 테스트가 같은 규칙 사용)
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
        treble = makeHighShelf (kTrebleShelfHz, r.trebleQ, r.trebleDb, fs);
        bass   = makeLowShelf  (kBassShelfHz,   r.bassQ,   r.bassDb,   fs);
        pres   = makeBell      (r.presHz, r.presQ, r.presDb, fs);
        tilt   = makeTilt      (kTiltPivotHz, r.tiltDb, fs);
        shelf  = makeHighShelf (TransformerStage::kShelfHz, TransformerStage::kShelfQ,
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
