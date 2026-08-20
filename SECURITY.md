# Security policy

LibreEcho UI is public source for review and contribution. It is not a hosted
service and it is not a public Internet device-control endpoint.

## Report privately

Please do not publish credentials, exploit details, private device data, or
unredacted logs in an issue or pull request. Use GitHub's private vulnerability
reporting for the canonical LibreEcho project:

<https://github.com/aslater3/LibreEcho/security/advisories/new>

If private reporting is unavailable, contact the maintainer through the
canonical LibreEcho project rather than publishing sensitive details.

## In scope

Report issues involving:

- authentication, sessions, CSRF, Origin checks, or access control;
- OTA signature, rollback, update, or release-identity verification;
- arbitrary file/device writes, privilege handling, or shell injection;
- Wi-Fi credential, OAuth, API-token, signing-key, or diagnostic disclosure;
- unsafe public-release packaging or third-party provenance failures;
- network-service exposure that could affect a trusted-LAN deployment.

## Redaction requirements

Never include:

- Wi-Fi SSIDs or PSKs;
- passwords, API keys, OAuth tokens, bearer tokens, or signing keys;
- serial numbers, MAC addresses, private IPs, or local filesystem paths;
- owner-local firmware, image manifests, or unredacted device logs.

Use placeholders and describe the affected component, public commit/release,
reproduction steps, impact, and a sanitized log excerpt instead.

## Deployment boundary

The daemon defaults to loopback. LAN operation requires explicit authentication
and Origin configuration. Do not use `--allow-insecure-lan` for production. Image
construction, firmware, signing keys, Wi-Fi inputs, OTA publication, and target
acceptance are owned by the separate LibreEcho build/product repositories.

LibreEcho is experimental software for hardware that the user owns or is
authorized to modify. Security reports are project documentation and not legal
advice.
