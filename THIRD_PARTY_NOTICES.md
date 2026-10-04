# Third-party notices

Mic FX Bridge does not bundle VB-CABLE or any VST plugin. Users obtain and license those products separately.

The repository's original project code and artwork are licensed under the root MIT `LICENSE`. That licence does not apply to JUCE or any other third-party component identified here.

The application is built with [JUCE 9.0.0](https://github.com/juce-framework/JUCE/tree/9.0.0), which is dual-licensed under AGPLv3 and the JUCE licence. Official Mic FX Bridge binaries are built and distributed under the JUCE 9 Starter licence while the project owner remains eligible. Anyone else building or redistributing the application must independently comply with either JUCE's AGPLv3 terms or their own eligible JUCE licence; the project's MIT licence does not grant JUCE rights. See the [JUCE framework licence file](https://github.com/juce-framework/JUCE/blob/9.0.0/LICENSE.md) and [JUCE 9 licence terms](https://juce.com/legal/juce-9-licence/).

JUCE modules used by this application incorporate third-party components under their own terms, including:

- Steinberg VST3 SDK — MIT
- FLAC and Ogg Vorbis codec libraries — BSD-style licences
- pnglib and zlib — zlib licence
- Independent JPEG Group library — IJG licence
- HarfBuzz — old MIT licence
- SheenBidi — Apache License 2.0
- LunaSVG and PlutoVG — MIT licence

The release-packaging script copies the exact JUCE and relevant dependency licence files from the pinned JUCE 9.0.0 source tree into `THIRD_PARTY_LICENSES` inside each archive. This summary is not a replacement for those full texts.

VST® is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries. Use of the VST name or VST Compatible logo is governed separately from the VST3 SDK's MIT copyright licence.

Mic FX Bridge hosts VST3 only. VST2 hosting is not supported, and no VST2 SDK files are part of this repository or its release archives.
