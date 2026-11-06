// =============================================================================
//  Pillow RZ EQ — DSP 코어 (JUCE 비의존, C++17 헤더 온리)
//
//  Klangfilm(Siemens) RZ062a/b 진공관 EQ의 커브를 기반으로 하고,
//  원본에는 없는 "밴드별 Q 조절"을 추가한 구현.
//
//  필터 구조: Andrew Simper(Cytomic)의 선형 사다리꼴(TPT) SVF
//   - 파라미터를 계속 바꿔도(자동화, Q 스윕) 안정적이고 지퍼 노이즈가 적음
//   - 사다리꼴 적분 = 쌍선형 변환이므로, GUI 응답 곡선을 "실제 처리와 똑같이"
//     해석적으로 계산할 수 있음 (ResponseModel)
//
//  JUCE 없이 컴파일되므로 tests/CoreTests.cpp 로 단독 검증 가능.
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace pillow::rz
{
//==============================================================================
//  하드웨어 기반 상수
//  - 게인 범위 / 기준 주파수 / 스텝: Audified·NEOLD 매뉴얼, GroupDIY 실측 (README 참고)
//  - 셸프 중간점과 프레즌스 기본 Q는 원본 부품값이 공개돼 있지 않아서,
//    문서상 동작(베이스 40 Hz~1 kHz, 트레블 1~10 kHz에 걸친 완만한 셸프)에 맞춘 추정치.
//    소리 취향에 맞게 이 값들만 바꿔도 전체 캐릭터가 바뀜.
//==============================================================================
inline constexpr double kPi = 3.14159265358979323846;

inline constexpr double kBassRefHz     = 40.0;     // 베이스 게인 표기 기준 주파수
inline constexpr double kTrebleRefHz   = 10000.0;  // 트레블 게인 표기 기준 주파수
inline constexpr double kBassShelfHz   = 160.0;    // 베이스 셸프 중간점(게인이 절반인 지점, 추정)
inline constexpr double kTrebleShelfHz = 2500.0;   // 650 Hz 축 대칭: 160 x 2500 = 40 x 10000
inline constexpr double kTiltPivotHz   = 650.0;    // A 모듈 틸트 회전 중심
inline constexpr std::array<double, 4> kPresenceHz { 1400.0, 2000.0, 2800.0, 4000.0 };

inline constexpr float kBassMinDb   = -20.0f, kBassMaxDb   = 12.0f;
inline constexpr float kTrebleMinDb = -18.0f, kTrebleMaxDb = 12.0f;
inline constexpr float kTiltMaxDb   = 4.0f;    // +-4 dB
inline constexpr float kPresMaxDb   = 4.8f;    // 0.8 dB x 6칸 (원본 눈금 0~6)
inline constexpr float kShelfStepDb = 2.0f;    // 베이스/트레블 원본 스텝
inline constexpr float kMidStepDb   = 0.8f;    // 틸트/프레즌스 원본 스텝

// Q 범위와 "빈티지" 기본값
//  - 셸프 Q 0.5 = 1차 셸프에 가까운 완만한 원본 느낌, 0.707 = 일반적인 2차 셸프,
//    1 이상 = 셸프 앞뒤로 범프/딥이 생기는 공진형(Pultec 같은 느낌)
inline constexpr float kShelfQMin = 0.3f, kShelfQMax = 2.0f, kVintageShelfQ = 0.5f;
inline constexpr float kPresQMin  = 0.3f, kPresQMax  = 6.0f, kVintagePresQ  = 1.0f;

// 부동소수 "정확히 같음" 비교 (-Wfloat-equal 경고 없이). 변경 감지용.
inline bool sameValue (double a, double b) noexcept { return ! (a < b) && ! (b < a); }

//==============================================================================
//  원본 스텝(디텐트) 양자화
//==============================================================================
inline float cleanZero (float v) noexcept { return std::abs (v) < 1.0e-4f ? 0.0f : v; }

inline float quantizeTreble (float db) noexcept
{
    const float q = std::round (db / kShelfStepDb) * kShelfStepDb;
    return cleanZero (std::clamp (q, kTrebleMinDb, kTrebleMaxDb));
}

// 원본 베이스 노브는 컷 방향 첫 칸이 -4 dB, 그 뒤로 2 dB 간격
// (+12 ... +2, 0, -4, -6 ... -20)
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

// UI 눈금용 디텐트 목록
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
    for (int i = -5; i <= 5; ++i) v.push_back (0.8 * i);
    return v;
}

inline std::vector<double> presenceDetents()
{
    std::vector<double> v;
    for (int i = 0; i <= 6; ++i) v.push_back (0.8 * i);
    return v;
}

//==============================================================================
//  파라미터
//==============================================================================
enum class Mode { A_Tilt = 0, B_Presence = 1 };

struct Params
{
    Mode  mode      = Mode::B_Presence;
    bool  stepped   = false;          // true = 원본처럼 계단식 게인
    float trebleDb  = 0.0f, trebleQ = kVintageShelfQ;
    float bassDb    = 0.0f, bassQ   = kVintageShelfQ;
    float tiltDb    = 0.0f;           // A 모드: + = 밝게(고역 업/저역 다운)
    int   presIndex = 2;              // B 모드: 0..3 -> 1.4 / 2 / 2.8 / 4 kHz
    float presDb    = 0.0f, presQ = kVintagePresQ;
    float tube      = 0.0f;           // 0..1
    float outputDb  = 0.0f;

    bool operator== (const Params& o) const noexcept
    {
        return mode == o.mode && stepped == o.stepped && presIndex == o.presIndex
            && sameValue (trebleDb, o.trebleDb) && sameValue (trebleQ, o.trebleQ)
            && sameValue (bassDb, o.bassDb)     && sameValue (bassQ, o.bassQ)
            && sameValue (tiltDb, o.tiltDb)
            && sameValue (presDb, o.presDb)     && sameValue (presQ, o.presQ)
            && sameValue (tube, o.tube)         && sameValue (outputDb, o.outputDb);
    }
    bool operator!= (const Params& o) const noexcept { return ! (*this == o); }
};

// 실제 DSP에 들어가는 값 (스텝 양자화 + 모드 반영 + 범위 제한)
struct Resolved
{
    double trebleDb, trebleQ, bassDb, bassQ, tiltDb, presHz, presDb, presQ, tube, outputDb;
};

inline Resolved resolve (const Params& p) noexcept
{
    const bool st   = p.stepped;
    const bool isB  = (p.mode == Mode::B_Presence);
    const int  pidx = std::clamp (p.presIndex, 0, 3);

    Resolved r {};
    r.trebleDb = st ? quantizeTreble (p.trebleDb) : std::clamp (p.trebleDb, kTrebleMinDb, kTrebleMaxDb);
    r.bassDb   = st ? quantizeBass (p.bassDb)     : std::clamp (p.bassDb, kBassMinDb, kBassMaxDb);
    r.trebleQ  = std::clamp (p.trebleQ, kShelfQMin, kShelfQMax);
    r.bassQ    = std::clamp (p.bassQ,   kShelfQMin, kShelfQMax);

    // 가운데 밴드: 모드에 따라 한쪽 게인을 0으로 (전환 시 스무딩으로 자연스럽게 크로스페이드)
    r.tiltDb = isB ? 0.0 : (st ? quantizeTilt (p.tiltDb) : std::clamp (p.tiltDb, -kTiltMaxDb, kTiltMaxDb));
    r.presDb = isB ? (st ? quantizePresence (p.presDb) : std::clamp (p.presDb, 0.0f, kPresMaxDb)) : 0.0;
    r.presHz = kPresenceHz[(size_t) pidx];
    r.presQ  = std::clamp (p.presQ, kPresQMin, kPresQMax);

    r.tube     = std::clamp (p.tube, 0.0f, 1.0f);
    r.outputDb = std::clamp (p.outputDb, -24.0f, 24.0f);
    return r;
}

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

//==============================================================================
//  Simper SVF (선형 사다리꼴)
//   출력 = m0*v0 + m1*v1(band) + m2*v2(low)
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

// 피크(벨): 중심 게인 = gainDb, Q는 RBJ 정의와 동일
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

// 2차 로우 셸프: fc = 게인이 절반(dB)인 중간점, Q가 경사/공진을 결정
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

// 2차 하이 셸프
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
//  A 모듈 틸트: 1차 셸프를 650 Hz 중심으로 회전 (원본처럼 중심부 경사가 가장 가파름)
//   +T dB -> 고역 +T, 저역 -T, 650 Hz는 0 dB
//==============================================================================
struct TiltCoeffs { double g = 0.0, G1 = 0.0, lfMinus1 = 0.0, outGain = 1.0; };
struct OnePoleState { double s = 0.0; };

inline TiltCoeffs makeTilt (double pivotHz, double tiltDb, double fs) noexcept
{
    const double G  = std::pow (10.0, -2.0 * tiltDb / 20.0);   // 고역 대비 저역 게인
    const double fp = pivotHz / std::sqrt (G);                 // 극점 (중간점 = pivot)
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
//  1폴 스무더 (파라미터 변화 시 클릭 방지)
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
//  튜브 스테이지 (오버샘플링된 레이트에서 호출)
//   - 비대칭 tanh -> 짝수 배음 위주 (3극관 느낌)
//   - 소신호 게인을 1로 정규화 -> 드라이브를 올려도 작은 신호 레벨은 그대로
//   - "왜곡 성분(y - x)"에만 DC 블로커를 걸어 원신호 저역은 건드리지 않음
//   - amount = 0 이면 완전히 바이패스(비트 단위 동일)
//==============================================================================
class TubeStage
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        dc.assign ((size_t) std::max (1, numChannels), DcState {});
        dcR = std::exp (-2.0 * kPi * 5.0 / fs);
        amount.setTimeConstant (0.02, fs / kChunk);
        amount.eps = 1.0e-5;
        reset();
    }

    void reset() noexcept
    {
        amount.snap();
        for (auto& d : dc) d = DcState {};
        samplesToNextUpdate = 0;
        updateDerived();
    }

    void setAmount (double a) noexcept { amount.target = std::clamp (a, 0.0, 1.0); }

    void process (float* const* ch, int numCh, int numSamples) noexcept
    {
        numCh = std::min (numCh, (int) dc.size());
        int i = 0;
        while (i < numSamples)
        {
            if (samplesToNextUpdate <= 0)
            {
                if (! amount.isSettled())
                {
                    amount.step();
                    updateDerived();
                }
                samplesToNextUpdate = kChunk;
            }
            const int len = std::min (samplesToNextUpdate, numSamples - i);

            if (wet > 0.0)
            {
                for (int c = 0; c < numCh; ++c)
                {
                    auto& d  = dc[(size_t) c];
                    float* x = ch[c] + i;
                    for (int k = 0; k < len; ++k)
                    {
                        const double in = x[k];
                        const double y  = (std::tanh (in * drive + kBias) - tanhBias) * norm;
                        const double r  = y - in;                    // 왜곡 성분
                        const double hp = r - d.x1 + dcR * d.y1;     // DC 블로커 (5 Hz)
                        d.x1 = r;
                        d.y1 = hp;
                        x[k] = (float) (in + wet * hp);
                    }
                }
            }
            else
            {
                for (auto& d : dc) d = DcState {};
            }

            samplesToNextUpdate -= len;
            i += len;
        }
    }

    // 캘리브레이션 상수 (tests 의 THD 측정으로 맞춤, -6 dBFS 사인 기준
    //  amount 0.1 ≈ 0.3 %, 0.25 ≈ 1 %, 0.5 ≈ 2~3 %, 1.0 ≈ 10 % 내외)
    static constexpr double kBias      = 0.22;   // 비대칭 정도 (짝수 배음)
    static constexpr double kMinDrive  = 0.06;   // amount 0 근처의 드라이브 (거의 선형)
    static constexpr double kRangeDb   = 30.0;   // amount 0 -> 1 사이 드라이브 증가량
    static constexpr double kFadeIn    = 0.15;   // 처음 15% 구간에서 왜곡을 서서히 섞음

private:
    void updateDerived() noexcept
    {
        const double a = amount.current;
        wet      = std::min (1.0, a / kFadeIn);
        drive    = kMinDrive * std::pow (10.0, (a * kRangeDb) / 20.0);
        tanhBias = std::tanh (kBias);
        norm     = 1.0 / (drive * (1.0 - tanhBias * tanhBias));
    }

    struct DcState { double x1 = 0.0, y1 = 0.0; };
    static constexpr int kChunk = 32;

    double fs = 48000.0, dcR = 0.9999;
    double wet = 0.0, drive = 1.0, tanhBias = 0.0, norm = 1.0;
    Smoother amount;
    std::vector<DcState> dc;
    int samplesToNextUpdate = 0;
};

//==============================================================================
//  오버샘플링 배율 (플러그인과 테스트가 같은 규칙을 쓰도록 여기에 둠)
//   EQ와 튜브를 모두 오버샘플된 레이트에서 돌려서
//   - 쌍선형 변환의 고역 찌그러짐(cramping) 없이 44.1 kHz에서도 아날로그와 같은 커브
//   - 튜브 왜곡의 에일리어싱 억제
//   0 = 1배, 1 = 2배, 2 = 4배
//==============================================================================
inline int oversamplingOrderFor (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)  return 2;
    if (sampleRate <= 100000.0) return 1;
    return 0;
}

inline double processingRateFor (double sampleRate) noexcept
{
    return sampleRate * (double) (1 << oversamplingOrderFor (sampleRate));
}

//==============================================================================
//  엔진: EQ -> 튜브 -> 출력 게인 (모두 같은 "처리 레이트", 보통 오버샘플된 레이트)
//
//  파라미터 변화는 스무딩 후 16샘플마다 목표 계수를 다시 계산하고,
//  그 사이는 샘플 단위로 계수를 선형 보간 -> 계수 계단에서 생기는 지퍼 노이즈 제거
//==============================================================================
class Engine
{
public:
    Engine() { setParams (Params {}); }

    void prepare (double processingRate, int numChannels)
    {
        fs = processingRate > 0.0 ? processingRate : 48000.0;
        channels.assign ((size_t) std::max (1, numChannels), ChannelState {});
        applySmoothingTimes();
        tube.prepare (fs, numChannels);
        reset();
    }

    double getProcessingRate() const noexcept { return fs; }

    // 0 이면 스무딩 없음 (테스트용)
    void setSmoothingMs (double ms)
    {
        smoothingSeconds = std::max (0.0, ms) * 0.001;
        applySmoothingTimes();
    }

    void setParams (const Params& p) noexcept
    {
        const Resolved r = resolve (p);
        setTarget (sTrebleDb, r.trebleDb);
        setTarget (sTrebleQ,  std::log (r.trebleQ));
        setTarget (sBassDb,   r.bassDb);
        setTarget (sBassQ,    std::log (r.bassQ));
        setTarget (sTiltDb,   r.tiltDb);
        setTarget (sPresDb,   r.presDb);
        setTarget (sPresHz,   std::log (r.presHz));
        setTarget (sPresQ,    std::log (r.presQ));
        outGain.target = dbToGain (r.outputDb);
        tube.setAmount (r.tube);
    }

    void reset() noexcept
    {
        for (auto* s : smoothers()) s->snap();
        outGain.snap();
        target  = computeBank();
        base    = target;
        ramping = false;
        dirty   = false;
        samplesToNextUpdate = 0;
        rampPos = 0;
        for (auto& c : channels) c = ChannelState {};
        tube.reset();
    }

    // 한 번에 전부 (플러그인은 이걸 오버샘플된 블록에 호출)
    void process (float* const* ch, int numCh, int numSamples) noexcept
    {
        processEQ (ch, numCh, numSamples);
        processTube (ch, numCh, numSamples);
        processOutput (ch, numCh, numSamples);
    }

    void processEQ (float* const* ch, int numCh, int numSamples) noexcept
    {
        numCh = std::min (numCh, (int) channels.size());
        int i = 0;
        while (i < numSamples)
        {
            if (samplesToNextUpdate <= 0)
                beginSubBlock();

            const int len = std::min (samplesToNextUpdate, numSamples - i);

            for (int c = 0; c < numCh; ++c)
            {
                auto& st = channels[(size_t) c];
                float* x = ch[c] + i;

                if (ramping)
                {
                    for (int k = 0; k < len; ++k)
                    {
                        const double t = (double) (rampPos + k + 1);
                        const SvfCoeffs cb = lerpSvf (base.bass,   step.bass,   t);
                        const SvfCoeffs ct = lerpSvf (base.treble, step.treble, t);
                        const SvfCoeffs cp = lerpSvf (base.pres,   step.pres,   t);
                        const TiltCoeffs cl = lerpTilt (base.tilt, step.tilt, t);
                        double v = x[k];
                        v = svfTick (cb, st.bass, v);
                        v = svfTick (ct, st.treble, v);
                        v = svfTick (cp, st.pres, v);
                        v = tiltTick (cl, st.tilt, v);
                        x[k] = (float) v;
                    }
                }
                else
                {
                    for (int k = 0; k < len; ++k)
                    {
                        double v = x[k];
                        v = svfTick (base.bass, st.bass, v);
                        v = svfTick (base.treble, st.treble, v);
                        v = svfTick (base.pres, st.pres, v);
                        v = tiltTick (base.tilt, st.tilt, v);
                        x[k] = (float) v;
                    }
                }
            }

            rampPos += len;
            samplesToNextUpdate -= len;
            i += len;
        }
    }

    void processTube (float* const* ch, int numCh, int numSamples) noexcept
    {
        tube.process (ch, numCh, numSamples);
    }

    void processOutput (float* const* ch, int numCh, int numSamples) noexcept
    {
        numCh = std::min (numCh, (int) channels.size());
        if (outGain.isSettled() && sameValue (outGain.current, 1.0))
            return;
        for (int i = 0; i < numSamples; ++i)
        {
            const double g = outGain.step();
            for (int c = 0; c < numCh; ++c)
                ch[c][i] = (float) (ch[c][i] * g);
        }
    }

private:
    static constexpr int kSubBlock = 16;   // 목표 계수 재계산 간격 (샘플)

    struct ChannelState { SvfState bass, treble, pres; OnePoleState tilt; };
    struct Bank { SvfCoeffs treble, bass, pres; TiltCoeffs tilt; };

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

    void applySmoothingTimes() noexcept
    {
        const double ctrlRate = fs / kSubBlock;
        for (auto* s : smoothers()) s->setTimeConstant (smoothingSeconds, ctrlRate);
        sTrebleDb.eps = sBassDb.eps = sTiltDb.eps = sPresDb.eps = 1.0e-4;   // dB
        sTrebleQ.eps = sBassQ.eps = sPresHz.eps = sPresQ.eps = 1.0e-6;      // log 도메인
        outGain.setTimeConstant (smoothingSeconds, fs);
        outGain.eps = 1.0e-7;
    }

    void setTarget (Smoother& s, double v) noexcept
    {
        if (! sameValue (s.target, v))
        {
            s.target = v;
            dirty = true;
        }
    }

    // 16샘플 구간 시작: 이전 목표를 기준으로 고정하고, 필요하면 다음 목표까지 램프 설정
    void beginSubBlock() noexcept
    {
        base = target;   // 직전 램프의 끝 = 정확한 목표 계수 (누적 오차 없음)
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

    double fs = 48000.0;
    double smoothingSeconds = 0.015;
    bool dirty = false, ramping = false;
    int samplesToNextUpdate = 0, rampPos = 0;

    Smoother sTrebleDb, sTrebleQ, sBassDb, sBassQ, sTiltDb, sPresDb, sPresHz, sPresQ, outGain;
    Bank base, step, target;
    std::vector<ChannelState> channels { ChannelState {} };
    TubeStage tube;
};

//==============================================================================
//  GUI 응답 곡선 계산 (실제 처리와 동일한 계수로 해석적으로 계산)
//==============================================================================
class ResponseModel
{
public:
    ResponseModel (const Params& p, double sampleRate) noexcept
        : fs (sampleRate > 0.0 ? sampleRate : 48000.0)
    {
        const Resolved r = resolve (p);
        treble = makeHighShelf (kTrebleShelfHz, r.trebleQ, r.trebleDb, fs);
        bass   = makeLowShelf  (kBassShelfHz,   r.bassQ,   r.bassDb,   fs);
        pres   = makeBell      (r.presHz, r.presQ, r.presDb, fs);
        tilt   = makeTilt      (kTiltPivotHz, r.tiltDb, fs);
    }

    // EQ 곡선만 (출력 게인/튜브 제외)
    double magnitudeDb (double freqHz) const noexcept
    {
        const auto h = svfResponse (treble, freqHz, fs) * svfResponse (bass, freqHz, fs)
                     * svfResponse (pres, freqHz, fs) * tiltResponse (tilt, freqHz, fs);
        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
    }

private:
    double fs;
    SvfCoeffs treble, bass, pres;
    TiltCoeffs tilt;
};

} // namespace pillow::rz
