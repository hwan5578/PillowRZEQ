#pragma once
//==============================================================================
//  Pillow RZ EQ — 실수 FFT (JUCE 비의존, 배정밀도)
//   크기 N = 2^m. 복소 N/2 FFT(반복 radix-2, 단계별 회전 인자 표, 실수부 · 허수부 따로) + 실수 분리 후처리
//   forward: N개 실수 -> N/2+1개 복소 (re[], im[], 스케일 없음)
//   inverse: N/2+1개 복소 -> N개 실수 (1/N 스케일 포함 -> inverse(forward(x)) == x)
//   std::complex 곱셈은 컴파일러에 따라 느린 라이브러리 호출이 되므로 직접 계산
//==============================================================================

#include <cmath>
#include <vector>

namespace pillow::rz
{
class RealFft
{
public:
    void prepare (int size)
    {
        n = 4;
        while (n < size) n <<= 1;
        half = n / 2;

        const double pi = 3.14159265358979323846;
        // 단계별 회전 인자: 길이 len 단계의 j번째 = e^{-2 pi i j / len}, 단계마다 이어서 저장
        stageTw.clear();
        for (int len = 2; len <= half; len <<= 1)
            for (int j = 0; j < len / 2; ++j)
            {
                stageTw.push_back (std::cos (2.0 * pi * (double) j / (double) len));
                stageTw.push_back (-std::sin (2.0 * pi * (double) j / (double) len));
            }
        twRe.resize ((size_t) half + 1);
        twIm.resize ((size_t) half + 1);
        for (int k = 0; k <= half; ++k)
        {
            twRe[(size_t) k] = std::cos (2.0 * pi * (double) k / (double) n);
            twIm[(size_t) k] = -std::sin (2.0 * pi * (double) k / (double) n);
        }

        int logHalf = 0;
        while ((1 << logHalf) < half) ++logHalf;
        bitrev.resize ((size_t) half);
        for (int i = 0; i < half; ++i)
        {
            int r = 0;
            for (int b = 0; b < logHalf; ++b)
                if (i & (1 << b)) r |= 1 << (logHalf - 1 - b);
            bitrev[(size_t) i] = r;
        }
        wr.assign ((size_t) half, 0.0);
        wi.assign ((size_t) half, 0.0);
    }

    int size() const noexcept    { return n; }
    int numBins() const noexcept { return half + 1; }

    // in: n개 -> re, im: half+1개
    void forward (const double* in, double* re, double* im) noexcept
    {
        for (int k = 0; k < half; ++k)
        {
            const int r = bitrev[(size_t) k];
            wr[(size_t) r] = in[2 * k];
            wi[(size_t) r] = in[2 * k + 1];
        }
        transform (false);

        re[0]    = wr[0] + wi[0];  im[0]    = 0.0;
        re[half] = wr[0] - wi[0];  im[half] = 0.0;
        for (int k = 1; k < half; ++k)
        {
            const double ar = wr[(size_t) k], ai = wi[(size_t) k];
            const double br = wr[(size_t) (half - k)], bi = -wi[(size_t) (half - k)];   // conj
            const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
            const double dr = ar - br, di = ai - bi;
            const double orr = 0.5 * di, oi = -0.5 * dr;                                // -i/2 (a - b)
            const double tr = twRe[(size_t) k], ti = twIm[(size_t) k];
            re[k] = er + tr * orr - ti * oi;
            im[k] = ei + tr * oi + ti * orr;
        }
    }

    // re, im: half+1개 -> out: n개
    void inverse (const double* re, const double* im, double* out) noexcept
    {
        for (int k = 0; k < half; ++k)
        {
            const double ar = re[k], ai = im[k];
            const double br = re[half - k], bi = -im[half - k];                          // conj
            const double er = ar + br, ei = ai + bi;
            const double dr = ar - br, di = ai - bi;
            const double tr = twRe[(size_t) k], ti = -twIm[(size_t) k];                  // conj(W^k)
            const double orr = dr * tr - di * ti, oi = dr * ti + di * tr;
            const int r = bitrev[(size_t) k];
            wr[(size_t) r] = er - oi;                                                    // e + i o
            wi[(size_t) r] = ei + orr;
        }
        transform (true);
        const double s = 1.0 / (double) n;
        for (int k = 0; k < half; ++k)
        {
            out[2 * k]     = wr[(size_t) k] * s;
            out[2 * k + 1] = wi[(size_t) k] * s;
        }
    }

private:
    void transform (bool inverse) noexcept
    {
        const double sign = inverse ? -1.0 : 1.0;
        size_t twBase = 0;
        for (int len = 2; len <= half; len <<= 1)
        {
            const int h = len / 2;
            const double* t = stageTw.data() + twBase;
            for (int start = 0; start < half; start += len)
            {
                double* ar = wr.data() + start;
                double* ai = wi.data() + start;
                double* br = ar + h;
                double* bi = ai + h;
                for (int j = 0; j < h; ++j)
                {
                    const double cr = t[2 * j], ci = sign * t[2 * j + 1];
                    const double vr = br[j] * cr - bi[j] * ci;
                    const double vi = br[j] * ci + bi[j] * cr;
                    const double ur = ar[j], ui = ai[j];
                    ar[j] = ur + vr;  ai[j] = ui + vi;
                    br[j] = ur - vr;  bi[j] = ui - vi;
                }
            }
            twBase += 2 * (size_t) h;
        }
    }

    int n = 0, half = 0;
    std::vector<double> stageTw, twRe, twIm, wr, wi;
    std::vector<int> bitrev;
};
} // namespace pillow::rz
