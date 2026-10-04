# Mic FX Bridge

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
- Built-in mic measurement/correction was removed in 0.4.4. Back up old settings before upgrading; do not run old and new versions together.

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
