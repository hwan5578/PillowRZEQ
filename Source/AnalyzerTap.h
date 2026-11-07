#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cmath>
#include <vector>

namespace pillow
{
    //==========================================================================
    // 오디오 스레드 -> 화면(분석기)으로 샘플을 넘기는 통로 (락·할당 없음)
    //  - 오디오 스레드: push()  (꽉 차 있으면 넘치는 만큼 버림)
    //  - 메시지 스레드: pull()  (쌓인 걸 전부 가져감)
    //  - 버퍼 크기는 고정이라 prepareToPlay 와 화면이 동시에 돌아도 안전
    //==========================================================================
    class AnalyzerTap
    {
    public:
        static constexpr int kCapacity = 1 << 16;

        AnalyzerTap() : fifo (kCapacity), data ((size_t) kCapacity, 0.0f) {}

        // 스테레오면 (L + R) / 2, 모노면 그대로
        void push (const float* a, const float* b, int n) noexcept
        {
            if (n <= 0 || a == nullptr)
                return;

            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);
            auto copy = [&] (int dst, int src, int len)
            {
                float* d = data.data() + dst;
                for (int i = 0; i < len; ++i)
                {
                    const float v = b != nullptr ? 0.5f * (a[src + i] + b[src + i]) : a[src + i];
                    d[i] = std::isfinite (v) ? v : 0.0f;   // 입력에 NaN이 섞여도 분석기가 망가지지 않게
                }
            };
            if (n1 > 0) copy (s1, 0, n1);
            if (n2 > 0) copy (s2, n1, n2);
            fifo.finishedWrite (n1 + n2);
        }

        int pull (float* dest, int maxSamples) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (maxSamples, s1, n1, s2, n2);
            if (n1 > 0) std::copy (data.data() + s1, data.data() + s1 + n1, dest);
            if (n2 > 0) std::copy (data.data() + s2, data.data() + s2 + n2, dest + n1);
            fifo.finishedRead (n1 + n2);
            return n1 + n2;
        }

    private:
        juce::AbstractFifo fifo;
        std::vector<float> data;

        JUCE_DECLARE_NON_COPYABLE (AnalyzerTap)
    };
}
