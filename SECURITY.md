# Security Policy

## Supported versions

| Version | Supported          |
|---------|--------------------|
| 1.x     | :white_check_mark: |
| < 1.0   | :x:                |

## Reporting a vulnerability

Please **do not open a public issue** for security problems.

Report privately through GitHub Security Advisories:
<https://github.com/bekirhankarakurt/embedded-telemetry-daemon/security/advisories/new>

Include the affected version, a description of the issue, and steps or a packet capture to reproduce
it. You can expect an acknowledgement within 7 days and a fix or mitigation plan within 30 days.
Reporters are credited in the release notes unless they prefer to stay anonymous.

## Scope notes

- The UDP downlink is **unauthenticated and unencrypted**; the CRC-32 detects corruption, not
  tampering. Run it on trusted networks or inside a VPN/tunnel.
- The listener treats every datagram as untrusted input. Malformed or truncated packets must never
  crash it; reports of such crashes are in scope.
