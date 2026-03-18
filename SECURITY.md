# Security Policy

## Supported Versions

ESPClaw currently treats these versions as supported for security fixes:

| Version | Supported |
| --- | --- |
| `main` | Yes |
| Latest tagged release | Yes |
| Older releases | Best effort only |

## Reporting A Vulnerability

Do not open a public GitHub issue for security-sensitive problems.

Please report:

- authentication or token leakage
- OTA trust or update bypass issues
- remote code execution or arbitrary file-write paths
- unsafe default exposure of device control surfaces
- secrets persisted in logs or workspace files

Recommended report format:

- affected version or commit
- affected board/profile
- exact trigger steps
- impact
- whether the issue requires local network access, physical access, or only chat/channel access

If you already disclosed a secret while testing:

1. rotate the secret first
2. say it was rotated in the report

## Hardening Notes

ESPClaw interacts with external providers and tokens. Review [docs/security-and-privacy.md](docs/security-and-privacy.md) before deploying outside a trusted environment.
