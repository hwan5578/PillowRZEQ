# Pillow RZ EQ

Klangfilm(Siemens) **RZ062a/b** 진공관 EQ의 커브를 기반으로, 원본에는 없는 **밴드별 Q 조절**을 더한 EQ 플러그인입니다.
JUCE 8 / CMake 프로젝트이고 VST3 · AU · Standalone으로 빌드됩니다. (Cubase에서는 VST3 사용)

![응답 곡선](docs_response_curves.png)

위 그래프는 그림용 계산이 아니라, 이 프로젝트의 DSP 코드에 임펄스를 넣어 측정한 값입니다.

---

## 컨트롤

| 컨트롤 | 범위 | 원본 RZ062 | 비고 |
|---|---|---|---|
| **TREBLE** | −18 ~ +12 dB | 10 kHz 기준, 2 dB 스텝 | 하이 셸프 |
| TREBLE **Q** | 0.3 ~ 2.0 (기본 0.5) | 없음 (고정 커브) | 0.5 = 원본에 가까운 완만한 셸프 |
| **BASS** | −20 ~ +12 dB | 40 Hz 기준, 2 dB 스텝 (컷 쪽 첫 칸은 −4 dB) | 로우 셸프 |
| BASS **Q** | 0.3 ~ 2.0 (기본 0.5) | 없음 | |
| **MODE** | A / B | A·B 모듈이 별개 제품 | A = 틸트, B = 프레즌스 |
| TILT (A) | ±4 dB | 650 Hz 축 회전, 0.8 dB 스텝 | 오른쪽 = 밝게(고역↑ 저역↓) |
| PRESENCE (B) | 0 ~ +4.8 dB | 0.8 dB × 6칸 | 부스트 전용 |
| PRESENCE FREQ | 1.4 / 2 / 2.8 / 4 kHz | 같음 | |
| PRESENCE **Q** | 0.3 ~ 6.0 (기본 1.0) | 없음 | |
| VINTAGE STEPS | on / off | 원본은 계단식 | on이면 게인이 원본 스텝에 붙음 (Q는 항상 연속) |
| TUBE | 0 ~ 100 % | 진공관 증폭단 색채 | 0 %면 완전 바이패스 |
| OUTPUT | ±18 dB | | |

### Q가 하는 일
- **셸프 Q** — 0.5는 원본처럼 넓고 완만한 경사, 0.707은 일반적인 2차 셸프, 1 이상은 경사가 가팔라지면서 셸프 앞뒤로 범프와 딥이 생깁니다(Pultec식 "부스트하면서 바로 위를 살짝 파는" 느낌). Q 2.0에서 +12 dB면 최대 약 +16.6 dB 범프 / −4.6 dB 딥.
- **프레즌스 Q** — +4.8 dB 기준 절반 게인 대역폭: Q 0.3 ≈ 3.7옥타브, 1.0 ≈ 1.4옥타브, 2.0 ≈ 0.7옥타브, 6.0 ≈ 0.24옥타브.

---

## VST3 파일 만들기 (가장 쉬운 방법)

### 방법 1 — Windows PC에서 더블클릭
`build_windows.bat` 을 더블클릭하면 빌드 → `C:\Program Files\Common Files\VST3` 설치까지 한 번에 합니다.
처음 한 번만 아래 3개가 필요하고, 없으면 스크립트가 설치 명령을 알려줍니다.

```bat
winget install Kitware.CMake
winget install Git.Git
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

- 첫 빌드는 JUCE 다운로드 때문에 5~10분, 이후는 1~2분.
- 마지막 복사 단계에서 권한 오류가 나면 Cubase를 닫고 `build_windows.bat` 우클릭 → **관리자 권한으로 실행**.
- Cubase: 스튜디오 → VST 플러그인 관리자 → 다시 스캔 → "Pillow Audio / Pillow RZ EQ".

Mac이면 터미널에서 `bash build_mac.sh` (VST3 + AU 설치까지).

### 방법 2 — GitHub에서 자동 빌드 (설치 불필요)
`main` 브랜치에 push 하면 GitHub의 Windows 서버가 MSVC로 빌드하고 DSP 테스트까지 돌립니다 (10~15분).
- 결과물: 저장소 → **Actions** → 최신 Build 실행 → 맨 아래 **Artifacts** → `PillowRZ-EQ-Windows-VST3`
- 같은 zip이 `builds` 브랜치에도 올라갑니다 (`PillowRZ-EQ-Windows-VST3.zip`, `PillowRZ-EQ-Windows-Standalone.zip`).
- 압축을 풀어 `Pillow RZ EQ.vst3` 폴더를 `C:\Program Files\Common Files\VST3` 에 넣고 Cubase에서 다시 스캔.
- Standalone(.exe)은 DAW 없이 소리를 바로 확인할 때 씁니다.

---

## 빌드 (직접 명령으로)

처음 configure 할 때 JUCE 8.0.17을 GitHub에서 자동으로 받아옵니다 (몇 분 걸림, 한 번만).
이미 JUCE가 있으면 이 폴더 안에 `JUCE/`로 넣어두면 그걸 씁니다.

### Windows (Cubase)
필요: Visual Studio 2022 ("C++를 사용한 데스크톱 개발"), CMake 3.22+, Git

```bat
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

결과물: `build\PillowRZ_artefacts\Release\VST3\Pillow RZ EQ.vst3`
→ `C:\Program Files\Common Files\VST3\` 에 복사 → Cubase **플러그인 관리자**에서 다시 스캔.
제조사는 "Pillow Audio"로 표시됩니다.

### macOS
```bash
cmake -B build -G Xcode
cmake --build build --config Release
```
결과물: `build/PillowRZ_artefacts/Release/VST3/`, `.../AU/`
→ `~/Library/Audio/Plug-Ins/VST3/`, `~/Library/Audio/Plug-Ins/Components/`

### DSP 테스트 (JUCE 없이도 됨)
```bash
cmake --build build --config Release --target PillowRZCoreTests
ctest --test-dir build -C Release --output-on-failure
```
또는 `g++ -std=c++17 -O2 -ISource tests/CoreTests.cpp -o CoreTests && ./CoreTests`

---

## 구조

```
build_windows.bat          Windows 원클릭 빌드 + VST3 설치
build_mac.sh               macOS 빌드 + VST3/AU 설치
.github/workflows/         GitHub Actions 자동 빌드 (Windows VST3 + Standalone)
Source/dsp/RZ062Core.h     DSP 전부 (JUCE 비의존): 필터, 스무딩, 튜브, 응답 곡선 계산
Source/PluginProcessor.*   파라미터, 오버샘플링, 레이턴시
Source/PluginEditor.*      UI: 노브, A/B 스위치, 실시간 응답 곡선
tests/CoreTests.cpp        81개 체크 (응답·Q·클릭·안정성·튜브 THD)
```

- **필터**: Cytomic(Andrew Simper) 선형 사다리꼴 SVF. Q·주파수를 자동화해도 안정적입니다.
- **오버샘플링**: EQ와 튜브를 둘 다 44.1/48k에서 4배, 88.2/96k에서 2배로 처리합니다. 덕분에 고역 커브가 세션 샘플레이트와 상관없이 아날로그 기준 ±0.05 dB 안에 들어옵니다 (오버샘플링 없이 44.1k면 최대 0.77 dB 어긋남). 대신 IIR 하프밴드 필터 지연이 몇 샘플 생기고, 호스트에 레이턴시로 보고됩니다.
- **파라미터 변화**: 15 ms 스무딩 + 샘플 단위 계수 보간. 베이스 +12 → −20 dB 같은 큰 점프도 클릭 없이 넘어갑니다.
- **응답 곡선 UI**: 실제 처리에 쓰는 계수로 계산해서, 화면 곡선과 실제 소리가 0.00001 dB 이내로 같습니다.
- **바이패스**: Cubase 인서트 바이패스 때도 오버샘플링 필터를 그대로 통과시켜, 켜고 끌 때 지연이 어긋나 생기는 클릭·콤필터가 없습니다.

### 검증 상태
- DSP 코어: GCC·Clang에서 테스트 81개 통과.
- 플러그인 코드: JUCE 8.0.17 소스에 대고 GCC·Clang으로 컴파일 확인 (JUCE 권장 경고 옵션에서 에러·경고 0).
- 아직 못 해본 것: MSVC 빌드, 실제 DAW에서 로드. Windows에서 빌드 에러가 나면 로그를 그대로 보내 주세요.

---

## 원본 커브: 확인된 것과 추정한 것

**자료로 확인한 값** (Audified·NEOLD 매뉴얼, GroupDIY 실물 측정, 리뷰 기사)
- 베이스 40 Hz 기준 +12 / −20 dB, 트레블 10 kHz 기준 +12 / −18 dB, 원본 2 dB 스텝
- A 모듈 틸트: 650 Hz 중심 ±4 dB (100 Hz +4 / 10 kHz −4 쪽으로 기울어짐)
- B 모듈 프레즌스: 1.4 / 2 / 2.8 / 4 kHz, 0.8 dB × 6칸 = 최대 4.8 dB
  - NEOLD는 1 dB × 6 = 6 dB로 구현했습니다. 6 dB가 좋으면 `kPresMaxDb`와 `kMidStepDb`만 바꾸면 됩니다.

**공개 자료가 없어 추정한 값** (전부 `Source/dsp/RZ062Core.h` 상단 상수)
- 셸프 중간점 `kBassShelfHz = 160`, `kTrebleShelfHz = 2500` — 40 Hz와 10 kHz가 약 632 Hz를 기준으로 대칭이라는 점(틸트 축 650 Hz와 거의 같음)에 맞춰 두 셸프를 거울 대칭으로 설계했습니다. 베이스 +12면 40 Hz에서 +11.3 dB, 1 kHz에서 +0.3 dB.
- 빈티지 셸프 Q 0.5, 프레즌스 기본 Q 1.0 — 원본 회로의 부품값이 공개되지 않아 문서상 동작에 맞춘 값입니다. 실기나 다른 에뮬레이션과 비교해서 귀로 맞추는 걸 권합니다.
- 튜브: 회로 모델이 아니라 비대칭 tanh 셰이퍼입니다. −6 dBFS 1 kHz 사인 기준 THD: 10 % → 0.3 %, 25 % → 0.8 %, 50 % → 1.8 %, 75 % → 4.2 %, 100 % → 9.4 %. 낮은 설정에서는 2차 배음 위주.

---

## 바꾸고 싶을 때

| 하고 싶은 것 | 고칠 곳 |
|---|---|
| 셸프 위치/성격 | `kBassShelfHz`, `kTrebleShelfHz`, `kVintageShelfQ` |
| 프레즌스 최대치·스텝 | `kPresMaxDb`, `kMidStepDb` |
| Q 범위 | `kShelfQMin/Max`, `kPresQMin/Max` (+ `PluginProcessor.cpp`의 skew 중심) |
| 튜브 성격 | `TubeStage`의 `kBias`(짝수 배음), `kMinDrive`, `kRangeDb` |
| 다른 Pillow 플러그인과 제조사 코드 통일 | `CMakeLists.txt`의 `PLUGIN_MANUFACTURER_CODE` |

배포할 계획이면: 제품명과 UI에는 "Klangfilm" 상표를 넣지 않고 "RZ062-style"이라고만 표기해 두었습니다. 마케팅 문구에서도 상표 사용은 조심하는 게 좋습니다.
