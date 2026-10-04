# Privacy and local data

Mic FX Bridge itself has no account system, advertising, analytics, telemetry, automatic updater, or cloud upload. During normal bridge operation, microphone audio is streamed in real time to the selected WASAPI output device; the app does not record it to a file or transmit it over the network.

To retain settings and the effect chain across the product rename, the app continues to store the following locally in the legacy-compatible `%APPDATA%\MicVstBridge\MicVstBridge.settings` path and its protective `.bak` and `.startup-backup` files:

- Windows display names and channels for the selected input and output
- VST file paths, plugin class identifiers, order, and bypass state
- State data supplied by each plugin, represented as Base64
- Additional VST search roots and the last valid folder used
- App run/recovery options and input-channel preferences
Version 0.4.4 removes built-in microphone measurement and correction. Old retired fields are ignored and omitted from new settings and exports. Protective `.bak`, `.startup-backup` and import recovery copies may retain the exact older file, including its old profile metadata; this allows rollback. The app does not delete personal profile files. Those retained backups are local data and should also be reviewed before sharing.

Manual **Save settings** files contain the same full configuration at the location you select. Before **Load settings** replaces the current configuration, existing settings and backups are copied to a timestamped `settings-recovery` subfolder. Later autosaves update the canonical settings file, not a separately named manual snapshot. Existing `chain-recovery` folders and old chain exports are retained from earlier versions.

Absolute paths such as VST file paths and search roots may contain a Windows user name, and plugin state may include preset information. Device display names can also reveal identifying information. Review or remove these names, paths, and plugin state before attaching a settings file to a bug report.

To erase all settings and backups, close the app and delete `%APPDATA%\MicVstBridge`. Third-party VST plugins loaded by the app may implement their own storage or network behaviour; consult each plugin vendor's privacy information separately.
