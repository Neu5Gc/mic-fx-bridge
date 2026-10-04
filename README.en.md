# Mic FX Bridge

[Project overview](README.md)

A free Windows app for sending your microphone through up to eight VST3 effects to voice chat or streaming apps.

## Requirements

Windows 10/11 x64, 64-bit VST3 plugins, and a virtual audio cable such as [VB-CABLE](https://vb-audio.com/Cable/). Plugins and the cable are installed separately. The app is English-only.

## Quick start

1. Get the ZIP from [Releases](https://github.com/Neu5Gc/mic-fx-bridge/releases), extract it, and run `Mic FX Bridge.exe`.
2. Select your microphone under **WASAPI microphone input** and choose its input channel.
3. Select **CABLE Input** under **Virtual microphone output**.
4. Add your VST3 effects. **Run bridge** and **Auto-recover** are on by default on first launch; later launches restore saved choices.
5. Select **CABLE Output** as the microphone in Discord, OBS, or your other app.

## Settings

Changes save after **five seconds** without another edit and restore on startup. **Save settings** and **Load settings** handle the full device/FX configuration.

Settings stay in `%APPDATA%\MicVstBridge\MicVstBridge.settings`, even when you move the executable. `.bak`, `.startup-backup` and import recovery copies protect earlier settings. A manual snapshot is not updated by later autosaves. If a plugin does not report changes, close its editor or save manually. Failed saves are reported in the app.

## Audio controls

**Reconnect audio** reopens both selected devices. **Refresh device list** updates the selectors. Automatic recovery handles input and output independently.

**Run bridge** starts/stops audio; **Auto-recover** retries device failures while running. Both default to on. **Browse VSTs / Add / Replace** select effects. **Bypass** skips one effect; **Edit** opens its controls; **↑ / ↓** reorder; **Remove** deletes it from the chain. **Save settings / Load settings** save or restore a full device/FX snapshot. In the VST browser, **Rescan** refreshes results, **Add Folder** adds a search path, **Select File** picks a VST3 directly, **Load Selected VST** loads it and **Close** closes the browser.

## Meters

IN shows the selected input before effects; OUT shows the processed signal. Both display peak/RMS in dBFS. The bridge adds no automatic gain, EQ or limiter.

## Upgrading

Built-in microphone measurement and correction were removed in 0.4.4. Old correction settings are ignored, not converted to VST presets. Keep a separate copy of your old settings for rollback; do not run old and new versions together.

## Beta notes

This beta is unsigned; Windows may show a SmartScreen warning. **The [GitHub Releases page](https://github.com/Neu5Gc/mic-fx-bridge/releases) is the only official distribution channel.** Other sites and reuploads are unofficial. If downloaded elsewhere, compare the file's SHA256 with the checksum on that official release before running it. Obtain the reference checksum from GitHub, not from the other download site. Do not run a file whose hash differs.

The program is distributed as one Windows ZIP, with one published SHA256 for that ZIP. In PowerShell, run `Get-FileHash -Algorithm SHA256 -LiteralPath '.\Mic-FX-Bridge-0.4.5-beta.1-windows-x64-en.zip'` and compare all 64 characters with the official ZIP checksum. A match verifies identical bytes, not a security audit. Third-party plugins run inside the app and can crash it. Device/driver/plugin compatibility varies.

[Report a problem](https://github.com/Neu5Gc/mic-fx-bridge/issues) with the app/plugin versions and steps to reproduce. Do not attach private recordings or unredacted settings.

## Licence and privacy

No account, telemetry or cloud upload. The latest release's buildable source is available in the [official repository](https://github.com/Neu5Gc/mic-fx-bridge). Original material uses the [MIT License](LICENSE); JUCE and other component licences are included separately. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [PRIVACY.en.md](PRIVACY.en.md) and [SECURITY.md](SECURITY.md).

## Build from source

Install Git, CMake 3.22+ and Visual Studio 2022 Build Tools with Desktop development with C++ and a Windows SDK. Clone the official repository and run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Configuration Release` on a clean checkout. CMake fetches pinned JUCE 9.0.0 and applies the checked-in compatibility patches. The script performs a clean build and all automated tests, then writes `dist/0.4.5/en/Mic FX Bridge.exe`. JUCE's separate licence terms apply. This section is excluded from binary packages.
