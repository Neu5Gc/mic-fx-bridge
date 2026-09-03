# Mic FX Bridge

A resilient Windows WASAPI microphone VST3 host with automatic device recovery and VB-CABLE-compatible output.

[한국어](#한국어) · [English](#english)

## 한국어

Mic FX Bridge는 물리 마이크 입력에 VST3 효과 체인을 적용해 가상 마이크 장치로 보내는 Windows용 호스트입니다. 오디오 인터페이스나 가상 케이블이 순간적으로 끊겨도 입력과 출력을 독립적으로 다시 연결합니다.

이 저장소는 공식 바이너리 릴리스와 사용자 문서만 제공합니다. 애플리케이션 소스 코드는 이 저장소에 게시하지 않습니다.

### 다운로드

오른쪽의 **Releases**에서 최신 시험판을 받으세요.

- `windows-x64-ko.zip`: 한국어판
- `windows-x64-en.zip`: 영어판
- `.zip.sha256`: 다운로드한 ZIP의 SHA-256 확인값

ZIP을 풀고 `Mic FX Bridge.exe`를 실행합니다. 이 베타는 코드 서명이 없으므로 Windows SmartScreen 경고가 나타날 수 있습니다. 반드시 이 공식 저장소에서 받고, 필요하면 PowerShell에서 다음처럼 해시를 대조하세요.

```powershell
Get-FileHash '.\Mic-FX-Bridge-0.4.0-beta.1-windows-x64-ko.zip' -Algorithm SHA256
```

### 주요 기능

- Windows 10/11 x64, WASAPI shared mode
- 최대 8개의 64비트 VST3 체인과 플러그인 상태 저장
- 입력·출력 장치의 독립적인 자동 복구
- VB-CABLE 같은 가상 오디오 케이블로 마이크 출력 공급
- VST 탐색, 마지막 폴더 기억, 순서 변경과 bypass
- IN/OUT peak 및 약 300 ms RMS dBFS 미터
- 보정된 무지향성 기준 마이크와 보컬 마이크의 2회 측정으로 내장 플랫 보정 프로필 생성

### 마이크 플랫 보정

기준 마이크 1회와 보컬 마이크 1회를 같은 캡슐 위치와 고정된 스피커 레벨에서 측정합니다. 보정 대역은 선택할 수 있고, FDW는 기본 5 cycles이며 5–30 범위에서 조절할 수 있습니다.

마이크 감도나 프리앰프 gain처럼 주파수와 무관한 상수 레벨 차이는 정규화해 보정하지 않습니다. 보정은 초과 응답 구간만 최대 9 dB까지 감쇄하고 부족한 대역은 boost하지 않으며, 전역 gain이나 pre-trim도 추가하지 않습니다. EQ는 limiter가 아니므로 절대 peak 상한이 필요하면 VST 체인 마지막에 limiter를 유지하세요.

### 라이선스와 데이터

Mic FX Bridge 자체 코드와 아이콘에는 [MIT License](LICENSE)가 적용됩니다. JUCE와 제3자 구성요소에는 각각의 라이선스가 적용되며, 해당 고지는 배포 ZIP에 포함됩니다. 앱에는 계정·광고·원격 측정·자동 업데이트·클라우드 업로드가 없습니다.

자세한 내용은 [개인정보 안내](PRIVACY.ko.md)와 [보안 정책](SECURITY.md)을 확인하세요.

## English

Mic FX Bridge applies a VST3 effect chain to a physical microphone and sends the processed signal to a virtual microphone device. Input and output endpoints recover independently after transient audio-interface or virtual-cable disconnects.

This repository contains official binary releases and end-user documentation only. The application source code is not published here.

### Download

Download the latest prerelease from **Releases**.

- `windows-x64-ko.zip`: Korean edition
- `windows-x64-en.zip`: English edition
- `.zip.sha256`: SHA-256 value for checking the downloaded ZIP

Extract the ZIP and run `Mic FX Bridge.exe`. This beta is not code-signed, so Windows SmartScreen may warn. Download only from this official repository and compare the hash when needed.

### Highlights

- Windows 10/11 x64 using WASAPI shared mode
- Up to eight 64-bit VST3 effects with saved plugin state
- Independent input/output automatic recovery
- VB-CABLE-compatible virtual microphone output
- Searchable VST browser with remembered folders, ordering, and bypass
- IN/OUT peak and approximately 300 ms RMS meters in dBFS
- Built-in correction profile from two measurements: calibrated reference, then vocal microphone

### Microphone correction

Measure the reference microphone once and the vocal microphone once at the same capsule position without changing speaker level. The correction band is selectable. FDW defaults to 5 cycles and can be adjusted from 5–30.

A frequency-independent sensitivity or preamp-gain offset is normalized out rather than corrected. The filter attenuates excess-response regions by up to 9 dB, never boosts deficient bands, and adds no global gain or pre-trim. EQ is not a limiter; retain a final limiter when an absolute peak ceiling is required.

### Licensing and data

Mic FX Bridge original code and artwork are covered by the [MIT License](LICENSE). JUCE and third-party components retain their own licences, with applicable notices bundled in each release ZIP. The app has no account, advertising, telemetry, automatic updater, or cloud upload.

See [Privacy](PRIVACY.en.md) and [Security](SECURITY.md) for details.
