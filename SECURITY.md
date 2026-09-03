# Security policy

## Supported code

Security fixes are applied to the current default branch and the latest published release. Older preview binaries are not maintained.

## Trust boundary

Only load VST modules obtained from vendors or authors you trust. A VST is native executable code loaded in the Mic FX Bridge process and receives live audio. Plugin discovery reads filenames without loading modules, but selecting or restoring a plugin loads it in-process. The current host does not sandbox plugin crashes, file access, or network access.

Mic FX Bridge does not request administrator privileges and does not need them for ordinary use. Treat any release that unexpectedly requests elevation as suspicious. Verify the published SHA-256 checksum before running an unsigned build.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on the [Mic FX Bridge repository](https://github.com/Neu5Gc/mic-fx-bridge/security) when it is enabled. If the private form is unavailable, open a public issue containing only a request for a private contact route; do not disclose the vulnerability there. Include the affected version, a minimal reproduction, expected impact, and whether an untrusted plugin is required. Do not include microphone recordings, unredacted settings, licence keys, or proprietary plugin binaries.

Please avoid opening a public issue for a vulnerability until a fix or mitigation is available.

