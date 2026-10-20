Monocypher 4.0.3 (https://monocypher.org/download/monocypher-4.0.3.tar.gz, SHA-512 matches the
published .sha512). monocypher.c/h and optional/monocypher-ed25519.c/h unchanged; the latter is
standard RFC 8032 Ed25519 (the core library's crypto_eddsa_* uses BLAKE2b instead). CC0 or
BSD-2-Clause (LICENCE.md). Used for replay signatures.
