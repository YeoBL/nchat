# Public newsletter response fixture

`newsletter-metadata-public.hex` is the hexadecimal representation of the public
686-byte `fulltest/file.argo` sample from [beeper/argo-go v1.1.2](https://github.com/beeper/argo-go/blob/v1.1.2/fulltest/file.argo).
It is an existing channel-metadata response, not a frame produced by this fork's
encoder and not data from the user's account. Tests use the actual bundled
WhatsApp `NewsletterMetadata` wire schema to decode it.

Source license: MIT, copyright 2025 Automattic Inc.; the upstream notice is
preserved in `argo-go-LICENSE`.
