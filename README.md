# Pillow RZ EQ (v0.5)

Klangfilm(Siemens) **RZ062a/b** 진공관 EQ의 커브와 **Telefunken EF804S 튜브 · 구간별 출력 트랜스포머**를 담은 EQ 플러그인입니다.
v0.5부터는 FabFilter Pro-Q처럼 **자유 밴드 방식**입니다: 그래프에서 밴드를 원하는 만큼(최대 16개) 만들고, 점을 누르면 그 밴드의 설정이 아래 패널에 열려서 **그 밴드에만** 적용됩니다.
EF804S 튜브와 출력 트랜스포머는 오른쪽 **메인 패널**에서 신호 전체에 겁니다.
JUCE 8 / CMake 프로젝트이고 VST3 · AU · Standalone으로 빌드됩니다. (Cubase에서는 VST3 사용)

![UI](docs_ui.png)

![밴드 모양](docs_response_curves.png)

![튜브·트랜스포머 왜곡](docs_tube_transformer.png)

위 그래프는 전부 이 프로젝트의 DSP 코드를 실제로 돌려서 측정한 값이고, UI 그림도 실제 플러그인 화면을 렌더링한 것입니다.

---

## VST3 파일 만들기

### 방법 1 — GitHub에서 자동 빌드 (설치 불필요)
`main` 브랜치에 push 하면 GitHub의 Windows 서버가 MSVC로 빌드하고 DSP 테스트까지 돌립니다 (10분 안팎).
- 결과물: 저장소 → **Actions** → 최신 Build 실행 → 맨 아래 **Artifacts** → `PillowRZ-EQ-Windows-VST3`
- 같은 zip이 `builds` 브랜치에도 올라갑니다 (`PillowRZ-EQ-Windows-VST3.zip`, `PillowRZ-EQ-Windows-Standalone.zip`).
- 압축을 풀어 `Pillow RZ EQ.vst3` 폴더를 `C:\Program Files\Common Files\VST3` 에 넣고 Cubase에서 다시 스캔.

### 방법 2 — Windows PC에서 더블클릭
`build_windows.bat` 을 더블클릭하면 빌드 → VST3 폴더 설치까지 한 번에 합니다.
처음 한 번만 Visual Studio Build Tools, CMake, Git이 필요하고, 없으면 스크립트가 `winget` 설치 명령을 알려줍니다.
Mac이면 터미널에서 `bash build_mac.sh` (VST3 + AU).

---

## 그래프에서 다루기

| 조작 | 동작 |
|---|---|
| 빈 곳 **더블클릭** | 그 자리(주파수 · 게인)에 Bell 밴드 추가 |
| 빈 곳 **오른쪽 클릭** | 모양을 골라서 추가 (Bell · Low/High Shelf · Low/High Cut · Notch · Tilt) |
| 점 **클릭** | 그 밴드를 고름 → 아래 밴드 패널이 그 밴드로 바뀜 |
| 점 **드래그** | 좌우 = 주파수, 위아래 = 게인. 컷 · 노치는 위아래 = Q |
| **Shift** + 드래그 / 휠 | 10배 미세 조정 (도중에 눌러도 튀지 않음) |
| **Ctrl(Cmd)** + 드래그 | 위아래 = Q (게인은 그대로) |
| 점 위에서 **마우스 휠** | Q (점 위가 아니면 고른 밴드의 Q) |
| **Alt** + 클릭 | 밴드 켜기 / 끄기 |
| 점 **오른쪽 클릭** | 모양 · 기울기 · 적용 채널 · 켜기 · 게인 초기화 · 삭제 |
| **Delete** / Backspace | 고른 밴드 삭제 (패널 노브를 만진 뒤에도 됨. Space · Enter 같은 다른 키는 그대로 Cubase로) |
| 빈 곳 클릭 · Esc | 선택 해제 |
| 오른쪽 위 **ANALYZER** | 스펙트럼: PRE + POST (처리 전 선 + 처리 후 면) → POST → 끔 |
| 오른쪽 위 **RANGE** | 그래프 범위 ±12 → ±24 → ±6 dB |

- 그래프 곡선은 실제 처리와 **같은 계수**로 계산한 것이고, 지금 모드(ZERO LATENCY / HQ)의 실제 응답입니다.
- 흰 곡선이 전체 응답, 가는 색 곡선이 밴드 하나하나, 고른 밴드는 색 면으로 표시됩니다.
- L/R · M/S 밴드가 있으면 채널별 곡선이 따로 나옵니다 (LEFT / RIGHT 또는 MID / SIDE, 아래 범례). 점 옆 글자 `L R M S`는 그 밴드가 걸리는 채널.
- 분석기는 4.5 dB/oct 기울기를 준 값입니다 (보통 음악이 대략 평평하게 보이도록). 에디터를 닫으면 분석기 처리도 멈춥니다.

## 밴드 패널 (고른 밴드 하나)

| 컨트롤 | 범위 | 비고 |
|---|---|---|
| SHAPE | Bell · Low Shelf · High Shelf · Low Cut · High Cut · Notch · Tilt | 바꿀 때 8 ms 페이드라 재생 중에도 클릭 없음 |
| FREQ | 10 Hz ~ 30 kHz | 값 글자를 눌러 `2.5k`, `120` 처럼 입력 가능 |
| GAIN | ±24 dB | Bell · 셸프 · Tilt (컷 · 노치는 없음) |
| Q | 0.1 ~ 24 (기본 1) | 셸프 · 컷은 **Q 1 = 출렁임 없는 기본 모양**(버터워스), 그 위는 굴곡/공진 |
| SLOPE | 6 · 12 · 18 · 24 · 36 · 48 · 72 · 96 dB/oct | 컷만 |
| PLACEMENT | STEREO · LEFT · RIGHT · MID · SIDE | 그 밴드가 걸리는 채널 |
| ON / REMOVE | | 끄면 설정은 남고 소리만 빠짐 / 밴드 삭제 |

- 위쪽 번호 칩 = 지금 쓰는 밴드들. 눌러서 바로 고를 수 있습니다.
- **원본 RZ062 소리를 그대로 내려면**: Low Shelf 160 Hz · High Shelf 2.5 kHz (원본 Bass 40 Hz · Treble 10 kHz 표기에 해당), Q 0.71, Bell 1.4 / 2 / 2.8 / 4 kHz Q 1, Tilt 650 Hz ±4 dB.
- Tilt: FREQ = 회전 축, GAIN + = 오른쪽(고역) 올리고 왼쪽(저역) 내림.
- 모노 트랙에서는 SIDE 밴드는 할 일이 없어서 꺼진 것처럼 보이고, 나머지(L · R · M 포함)는 그 한 채널에 걸립니다.

## 메인 패널 (신호 전체)

| 컨트롤 | 설명 |
|---|---|
| EQ | 밴드 전체 켜기 / 끄기 |
| DRIVE 스위치 · DRIVE | EF804S 튜브 단 켜기 / 들어가는 신호 크기 0 ~ 48 dB (레벨은 자동 보상) |
| FOCUS | 홀수 ↔ 짝수 배음. 0 = EF804S 단 그대로, −100 = 짝수 제거(대칭), +100 = 짝수 2배 |
| MIX | 원음 블렌드 (밴드 + 튜브 + 트랜스포머를 병렬로) |
| TRANSFORMER LOW | 200 Hz 아래: 코어 포화 — 큰 저음일수록 진해지는 3차 배음 위주 두께감 + 저음 압축 |
| TRANSFORMER MID | 200 Hz ~ 4 kHz: 은은한 밀도 — 약한 비대칭, 2차+3차 |
| TRANSFORMER HIGH | 4 kHz 위: 고역 포화 + 12 kHz 위를 최대 −1.5 dB 부드럽게 (그래프에 표시됨) |
| OUTPUT | ±18 dB |

신호 흐름 (ZERO LATENCY = 세션 레이트, HQ = 오버샘플된 레이트):

```
스테레오 · L · R 밴드 -> [M/S 인코드] Mid · Side 밴드 [디코드] -> EF804S -> 출력 트랜스포머 -> MIX(원음) -> OUTPUT
```

---

## ZERO LATENCY / HQ

| | ZERO LATENCY (기본) | HQ |
|---|---|---|
| 레이턴시 | **0 샘플** (모든 샘플레이트) | 44.1/48k 6 샘플(0.13 ms), 88.2/96k 4 샘플, 176.4k 이상 0 |
| 처리 | 세션 레이트 그대로 | 44.1/48k 4배, 88.2/96k 2배 오버샘플링 |
| 튜브·트랜스포머 안티에일리어싱 | ADAA (아래 설명) | 오버샘플링 + ADAA |
| 쓰임 | 녹음 모니터링, 트래킹, 평소 믹스 | 무거운 드라이브 + 고역이 많은 소스, 15 kHz 위 에어 밴드, 마스터링 |

- 재생 중에도 바꿀 수 있습니다 (약 10 ms 페이드). 레이턴시가 바뀌는 설정이라 **자동화는 막아 두었습니다.** 바뀐 레이턴시는 호스트에 바로 알려서 Cubase의 지연 보상이 따라옵니다.
- **ADAA (역도함수 안티에일리어싱)**: 튜브·트랜스포머 곡선을 "샘플 사이를 직선으로 이은 구간의 평균"으로 계산해서 접혀 들어오는 배음을 줄입니다. 레이턴시 없음.
  예) EF804S Drive 36 dB, 6 kHz: −28 → −38 dBc · 트랜스포머 HIGH 100 %, 7 kHz: −50 → −69 dBc (48 kHz). 기본파 레벨·위상은 오버샘플링한 것과 0.01 dB 이내.
- **매칭 EQ 설계**: 보통의 쌍선형 변환은 44.1/48k에서 고역 곡선이 찌그러집니다. Bell · 셸프는 극점을 아날로그 그대로 옮기고 크기 응답을 DC · 기준 주파수 · 나이퀴스트에서 맞추는 설계(Vicanek 2016)를 쓰고,
  이 설계가 무리하는 경우(나이퀴스트 바로 아래의 깊은 컷, 아주 날카로운 셸프)는 아날로그 곡선과 여러 점에서 비교해서 더 가까운 쪽 설계로 자동으로 바꿉니다.
  - 보통 쓰는 범위(10 kHz 이하, Q 4 이하, ±15 dB): 20 Hz~20 kHz 전체에서 아날로그 곡선과 **최대 약 0.5 dB** (HQ는 0.06 dB)
  - 한계: 44.1/48k의 ZERO LATENCY에서 **12 kHz 위에 Q가 높은 밴드**를 두면 나이퀴스트(22/24 kHz) 근처 모양이 아날로그와 몇 dB까지 달라질 수 있습니다 (중심 주파수·게인은 정확). 그 대역을 정밀하게 다룰 때는 HQ를 쓰세요. 그래프는 실제 처리 곡선을 그대로 보여 줍니다.
  - 컷 · 노치는 세션 레이트에서도 차단점 / 중심이 정확합니다.

---

## 예전 세션 (v0.4 이하)

예전 프로젝트를 열면 **같은 곡선이 나오는 밴드로 자동 변환**됩니다 (테스트상 예전 곡선과 0.002 dB 이내).

| 예전 | v0.5 |
|---|---|
| 모듈의 Bass / Treble / Presence / Pivot | Low Shelf / High Shelf / Bell / Tilt 밴드 (0 dB인 것은 밴드를 만들지 않음) |
| LINK 켜짐 | STEREO 밴드 |
| LINK 꺼짐 + STEREO L/R | 모듈 1 → LEFT 밴드, 모듈 2 → RIGHT 밴드 |
| LINK 꺼짐 + MID/SIDE | 모듈 1 → MID 밴드, 모듈 2 → SIDE 밴드 |
| VINTAGE STEPS | 계단 값으로 맞춘 결과를 밴드에 넣음 (스위치는 없어짐) |
| 모듈별 DRIVE · LEVEL | 하나로: 링크면 모듈 1 값, 아니면 큰 DRIVE · 평균 LEVEL |
| EQ · DRIVE 스위치, FOCUS, MIX, 트랜스포머, ZERO LATENCY/HQ | 그대로 (자동화도 그대로 이어짐) |

- 없어진 것: 모듈 A/B 구분, LINK, STEREO/MID-SIDE 라우팅 스위치, VINTAGE STEPS — 전부 밴드별 모양 · 적용 채널로 대신합니다.
- 예전 모듈 컨트롤에 걸어 둔 **자동화**는 새 밴드로 옮겨지지 않습니다 (Drive · Output · Focus · Mix · 트랜스포머 · EQ/DRIVE 스위치 자동화는 유지).

---

## EF804S 튜브 모델

RZ062 안의 EF804S 증폭단을 **Koren 3극관 모델**로 풀어서 얻은 실제 전달곡선을 씁니다 (tanh 같은 일반 곡선이 아님).

- **관 파라미터**: μ = 38 (EF804S 데이터시트의 3극관 결선 값). Ex·Kvb는 같은 계열 EF86의 Koren 모델을 따랐고, Kg1·Kp는 데이터시트의 "3극관 결선 RC 증폭 이득 31 (250 V, 100 kΩ)"이 정확히 나오도록 보정했습니다 (모델 결과 31.0, ra ≈ 23 kΩ, gm ≈ 1.7 mA/V @ 1 mA).
- **회로**: B+ 290 V, 플레이트 부하 220 kΩ, 바이패스 안 된 캐소드 저항(유휴 0.5 mA가 되도록 계산된 8.4 kΩ), 그리드 전류 소프트 클램프 — GroupDIY에 올라온 RZ062 실물 추정값 기반. 단 이득은 약 14배.
- **결과**: 약하게 걸면 2차 배음 위주, 그리드 전압이 커지면 컷오프와 그리드 전류 때문에 3차 배음이 주도 (그래프 왼쪽).
- **Drive 0 dB**는 풀스케일이 그리드 0.5 Vpk라 거의 깨끗하고, 30 dB 근처에서 본격적으로 걸립니다.
- **레벨 보상**: −12 dBFS 사인의 기본파 레벨이 Drive와 상관없이 같도록 자동 보정 (테스트상 0.00 dB 오차).
- 4,097점 asinh 워핑 테이블 + 3차 보간 (ADAA용 역도함수 테이블 포함), 회로를 직접 푼 값과 작은 신호에서 상대오차 1e-7 이하.

출력 트랜스포머의 배음은 구간별로 나눠 만들고, 압축은 합이 원신호와 똑같은 구간 분할에 걸어서 크로스오버 근처에서도 항상 "눌리는" 방향으로만 동작합니다.

---

## 빌드 (직접 명령으로)

처음 configure 할 때 JUCE 8.0.17을 GitHub에서 자동으로 받아옵니다. 이미 JUCE가 있으면 이 폴더 안에 `JUCE/`로 넣어두면 그걸 씁니다.

```bat
cmake -B build -A x64
cmake --build build --config Release --target PillowRZ_VST3
```
결과물: `build\PillowRZ_artefacts\Release\VST3\Pillow RZ EQ.vst3`

DSP 테스트 (JUCE 없이도 됨): `g++ -std=c++17 -O2 -ISource tests/CoreTests.cpp -o CoreTests && ./CoreTests`

---

## 구조

```
Source/dsp/RZ062Core.h     DSP 전부 (JUCE 비의존): 밴드 설계(매칭/쌍선형 자동 선택, 버터워스 컷), 밴드 처리(스무딩·페이드),
                           EF804S, 트랜스포머, ADAA, 엔진, 응답 곡선, 예전 세션 -> 밴드 변환
Source/PluginProcessor.*   파라미터 138개(전역 10 + 밴드 16 x 8), ZERO LATENCY / HQ 경로, 레이턴시 보고, 바이패스, 분석기 탭, 세션 변환
Source/AnalyzerTap.h       오디오 -> 화면 샘플 전달 (락 없음)
Source/GraphView.*         그래프: 밴드 점, 곡선, 분석기, 마우스 · 키보드 · 메뉴
Source/Panels.*            밴드 패널, 메인 패널
Source/UiCommon.*          노브, 룩앤필, 선택 버튼, 밴드 추가/삭제
Source/PluginEditor.*      화면 배치
tests/CoreTests.cpp        131개 체크
build_windows.bat          Windows 원클릭 빌드 + VST3 설치
build_mac.sh               macOS 빌드 + VST3/AU 설치
.github/workflows/         GitHub Actions 자동 빌드 (Windows VST3 + Standalone)
```

- **필터**: Cytomic(Andrew Simper) 선형 사다리꼴 SVF. 주파수 · 게인 · Q는 스무딩하면서 0.33 ms마다 계수를 다시 계산하고 그 사이는 샘플 단위로 보간 → 크게 돌려도 클릭 없음.
  모양 · 기울기 · 적용 채널 · 켜기/끄기 · 추가/삭제는 그 밴드의 효과만 8 ms 동안 줄였다 키우는 방식이라 원신호가 끊기지 않습니다.
- 밴드 상태 전환은 호스트 블록 크기와 상관없이 같은 샘플에서 일어납니다 (블록 1~1000 샘플에서 결과가 비트 단위로 같음).
- **트랜스포머·튜브**는 "왜곡 성분"만 만들어 원신호에 더하는 방식이라, 0이면 비트 단위로 완전히 투명합니다. 밴드가 없거나 게인 0이어도 비트 단위로 그대로.

---

## 원본 커브: 확인된 것과 추정한 것

**자료로 확인한 값** (Audified·NEOLD 매뉴얼, Plugin Alliance 제품 페이지, GroupDIY 실물 측정, 리뷰 기사)
- 베이스 40 Hz 기준 +12 / −20 dB, 트레블 10 kHz 기준 +12 / −18 dB, 원본 2 dB 스텝
- A 모듈 Pivot: 650 Hz 중심 ±4 dB / B 모듈 Presence: 1.4 / 2 / 2.8 / 4 kHz
- Presence 최대치: NEOLD는 1 dB × 6 = 6 dB, 실물 측정 자료는 0.8 dB × 6 = 4.8 dB
- EF804S: μ 38, 3극관 RC 증폭 이득 31 (데이터시트) / RZ062 안의 결선·전류는 GroupDIY 추정

**공개 자료가 없어 추정한 값** (전부 `Source/dsp/RZ062Core.h` 상수)
- 셸프 중간점 160 Hz / 2.5 kHz (650 Hz 축 대칭), 빈티지 셸프 Q 0.5(새 Q 기준 0.71), 프레즌스 기본 Q 1.0
- 트랜스포머 크로스오버 200 Hz / 4 kHz와 구간별 포화량 (100 %에서 −6 dBFS 기준 LOW 3.5 % · MID 1.4 % · HIGH 2.3 %)

---

## 바꾸고 싶을 때

| 하고 싶은 것 | 고칠 곳 (`RZ062Core.h`) |
|---|---|
| 밴드 수 · 범위 | `kMaxBands`, `kFreqMin/Max`, `kGainMax`, `kQMin/Max` (밴드 수를 바꾸면 파라미터 ID도 늘어남) |
| 컷 기울기 단계 | `kCutSlopes` |
| 모양 바뀔 때 페이드 길이 | `BandProcessor::kFadeSeconds` |
| 튜브가 걸리기 시작하는 지점 | `TubeStage::kGridVoltsPerFullScale` |
| EF804S 회로 (전압·부하·전류) | `Ef804sCurve::kBplus`, `kRl`, `kIdleA` |
| 트랜스포머 구간 경계·강도 | `TransformerStage::kXoverLowHz/HighHz`, `kLowDrive`, `kMidDrive`, `kHighDrive`, `kShelfMaxCutDb` |
| HQ 오버샘플링 배율 | `oversamplingOrderFor()` |

## 검증 상태
- DSP 코어: GCC·Clang·MSVC(GitHub)에서 테스트 131개 통과 — 밴드 모양(벨 중심 이득 0.0000 dB, 셸프 끝·중간점, 컷 차단점 −3.01 dB와 6~96 dB/oct 기울기, 노치 깊이, 틸트), 응답 곡선 vs 실제 처리(44.1k~192k, 0.002 dB), 적용 채널(L/R/M/S·모노, 다른 채널로 새는 양 0), Q 동작, 샘플레이트 간 일관성, 모양·채널·추가/삭제·EQ/DRIVE 전환 클릭, 무작위 자동화 스트레스, 블록 크기 독립성, EF804S 데이터시트 대조, 튜브 레벨 보상·Focus, 트랜스포머, ADAA, 극단적인 입력(표 끝·온셋·DC·NaN), 예전 세션 변환(무작위 80개, 0.002 dB). AddressSanitizer·UBSan에서도 오류 없음.
- 플러그인 코드: JUCE 8.0.17에 대고 GCC·Clang 컴파일 (엄격한 경고 옵션에서 에러·경고 0). 실제 프로세서·에디터로 확인: v0.1~v0.4 세션 변환(링크·L/R·M/S·스텝·Presence 4단계, 옛 항목 정리, 다시 불러와도 동일), 그래프 더블클릭 추가 · 드래그 · Shift 미세 · Ctrl 드래그 Q · 휠 Q · Alt 클릭 · Delete · 패널 연동을 실제 마우스 이벤트로, 16개 꽉 찼을 때, 모든 모양·채널 16밴드 + 모드 전환 + 블록 크기 1~1000, 모노 트랙. 따로 돌린 코드 리뷰에서 나온 문제(키보드 포커스, 파라미터 제스처 짝, 재생이 멈춘 상태의 레이턴시 보고, SLOPE 숫자 입력, 분석기 NaN, 창을 닫을 때 남는 메뉴)를 고친 뒤 실제 창(Xvfb)과 재현 코드로 다시 확인. Sanitizer 빌드로도 오류 없음.
- CPU (48 kHz 스테레오, ZERO LATENCY): 밴드 16개 고정 약 0.5 %, 16개를 동시에 자동화할 때 약 6 % (한 코어 기준).

배포할 계획이면: 제품명과 UI에는 "Klangfilm" 상표를 넣지 않고 "RZ062-style"이라고만 표기해 두었습니다.
