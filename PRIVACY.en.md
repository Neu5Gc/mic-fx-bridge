# Privacy and local data

Mic FX Bridge itself has no account system, advertising, analytics, telemetry, automatic updater, or cloud upload. During normal bridge operation, microphone audio is streamed in real time to the selected WASAPI output device; the app does not record it to a file or transmit it over the network.

During a guided microphone measurement, the two raw captures—the omnidirectional reference microphone and the vocal microphone—are held temporarily in process memory for offline analysis. The app never deliberately writes this raw audio to disk or uploads it, and does not retain it after the measurement workflow completes or is reset, the measurement window closes, or the app exits. This does not mean that the memory is securely erased by overwriting it.

To retain settings and the effect chain across the product rename, the app continues to store the following locally in the legacy-compatible `%APPDATA%\MicVstBridge\MicVstBridge.settings` path and its protective `.bak` and `.startup-backup` files:

- Windows display names and channels for the selected input and output
- VST file paths, plugin class identifiers, order, and bypass state
- State data supplied by each plugin, represented as Base64
- Additional VST search roots and the last valid folder used
- Window dimensions and app preferences
- Microphone-correction enabled state, profile name, and microphone name
- Windows display names and channels of the microphone input and speaker output used for measurement, creation date, and sample rate
- Selected correction band and quality-approved accepted band, FDW cycles and maximum window, smoothing, and the 100% correction target
- Quality metadata such as the sensitivity/preamp normalization offset excluded from correction, filter-fit errors, and fitted point count
- Calibration file basename and SHA-256 hash when a calibration file was used
- Fitted attenuation-only filter frequency, gain, and Q values and filter count

Settings and backups do not contain raw measurement captures or calibration-file contents. They also do not store the calibration file's full path or the calibration file chooser's last folder. The selected full path is kept in memory only while that measurement window remains open; the correction profile records only the basename, without its path.

Absolute paths such as VST file paths and search roots may contain a Windows user name, and plugin state may include preset information. Device display names, user-entered profile and microphone names, and calibration filenames can also reveal identifying information if you put it there. Review or remove these names, paths, and plugin state before attaching a settings file to a bug report.

To erase all settings and backups, close the app and delete `%APPDATA%\MicVstBridge`. Third-party VST plugins loaded by the app may implement their own storage or network behaviour; consult each plugin vendor's privacy information separately.

