# Mic FX Bridge

First public beta for Windows.

A free Windows app for sending your microphone through up to eight VST3 effects to Discord, OBS, and other voice apps.

**[Official downloads](https://github.com/Neu5Gc/mic-fx-bridge/releases)** · [Report a problem](https://github.com/Neu5Gc/mic-fx-bridge/issues)

This repository's **GitHub Releases page is the only official distribution channel** for Mic FX Bridge. Other download sites and reuploads are unofficial.

Windows 10/11 x64 · VST3 only · English UI. Install your plugins and a virtual audio cable such as [VB-CABLE](https://vb-audio.com/Cable/) separately.

## Quick start

1. Extract the ZIP and run `Mic FX Bridge.exe`.
2. Select your microphone as input and **CABLE Input** as output.
3. Add your VST3 effects and enable **Run bridge** / **Auto-recover**.
4. In your voice app, select **CABLE Output** as the microphone.

## Settings

Your devices and FX chain save automatically after five seconds without another edit and restore on startup. **Save settings** / **Load settings** let you keep separate snapshots. Settings live in `%APPDATA%\MicVstBridge`.

## Beta notes

- Unsigned beta: Windows may warn. Verify the SHA256 before running it.
- Plugins run inside the app; compatibility varies and a faulty plugin can crash it.

## Verify your download

If you downloaded the app elsewhere, **compare its SHA256 with the value on the official GitHub release before running it**. Get the reference `.sha256` file from GitHub, not from the other download site.

In PowerShell, from the folder containing the ZIP:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath '.\Mic-FX-Bridge-0.4.5-beta.1-windows-x64-en.zip'
```

Compare all 64 hexadecimal characters with the official `.sha256` file (letter case does not matter). The release notes also give the extracted EXE's hash. If the values differ, do not run the file; download it again from the official release. Matching hashes establish identical bytes, not a security audit.

## Build

This repository contains the source and build/test files for the latest release, without private development history. Install Git, CMake 3.22+ and Visual Studio 2022 Build Tools with **Desktop development with C++** and a Windows SDK, then:

```powershell
git clone https://github.com/Neu5Gc/mic-fx-bridge.git
cd mic-fx-bridge
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Configuration Release
```

CMake downloads pinned JUCE 9.0.0 and applies the included patches. The script performs a clean build and automated tests, then writes `dist/0.4.5/en/Mic FX Bridge.exe`. Keep the checkout clean; use `-ValidationOnly` for repeat checks without replacing a delivered version. See [the user guide](README.en.md) for more details.

No account, telemetry or cloud upload. [MIT License](LICENSE) applies to original project material; JUCE has separate licence terms. [Third-party notices](THIRD_PARTY_NOTICES.md) · [Privacy](PRIVACY.en.md) · [Security](SECURITY.md).

---

## 한국어 안내

이번 버전은 Mic FX Bridge의 첫 공개 베타입니다.

마이크에 최대 8개의 VST3 효과를 적용해 디스코드·OBS 등으로 보내는 무료 Windows 프로그램입니다. Windows 10/11 64비트용이며 앱 화면은 영어입니다. VST3 플러그인과 [VB-CABLE](https://vb-audio.com/Cable/) 같은 가상 오디오 케이블은 따로 설치합니다.

**[이 저장소의 Releases](https://github.com/Neu5Gc/mic-fx-bridge/releases)가 유일한 공식 배포처입니다.** 다른 사이트의 재배포는 비공식입니다.

1. 베타 ZIP의 압축을 풀고 `Mic FX Bridge.exe`를 실행합니다.
2. 입력에 마이크, 출력에 **CABLE Input**을 선택합니다.
3. VST3 효과를 추가하고 **Run bridge**와 **Auto-recover**를 켭니다.
4. 디스코드·OBS의 마이크를 **CABLE Output**으로 선택합니다.

설정은 마지막 수정 후 5초 동안 추가 변경이 없으면 자동 저장되며, 다음 실행 때 복원됩니다. **Save settings / Load settings**로 따로 저장·불러올 수도 있습니다.

**다른 곳에서 받은 파일은 실행 전에 위 방법으로 SHA256을 계산하고 공식 릴리스의 값과 64자리 전체를 대조하세요.** 비교 기준은 다른 배포 사이트가 아닌 공식 GitHub에서 받아야 합니다. 값이 다르면 실행하지 말고 공식 배포처에서 다시 받으세요. 서명되지 않은 베타라 Windows 경고가 나타날 수 있습니다. 최신 버전을 빌드할 소스와 방법도 위에 공개합니다.
