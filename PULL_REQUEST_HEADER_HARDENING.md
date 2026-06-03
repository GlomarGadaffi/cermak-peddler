# Pull Request: Harden SIP header matching

This PR hardens header parsing in SipMessage::parse by normalizing header names before comparison (trim trailing whitespace, lowercase comparison), and supporting compact header forms. It improves compatibility with real-world SIP implementations that may use non-canonical capitalization or stray whitespace.

Changes:
- Replace matchHeader helper with a trim-and-lowercase comparison.
- Update callers to rely on normalized matching for via/from/to/cseq/contact/content-length.

Tests:
- Add unit tests verifying compact headers, odd capitalization, and whitespace handling (in test branch).

Closes: #65
