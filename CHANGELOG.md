# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

- Build system for GCC 9.2 and a MIPSPro compile gate
- IRIX compatibility layer covering libc gaps and the `_rld_new_interface` stub
- TCP and TLS transport, verified against api.github.com from IRIX 6.5.30m
- JSON parser with case-insensitive lookup, and a streaming writer
- HTTP/1.1 client with keep-alive, chunked decoding, redirects and clock-skew measurement
- Native actions/checkout handler using the git binary
- Job execution: run: steps, per-step status, and full logs via the results service
- Listener: OAuth tokens, agent session, long poll, message decrypt, v2 broker migration
- Runner registration and config persistence (`.runner`, `.credentials`, `.rsakey`)
- Crypto layer: base64 and base64url, AES-256-CBC message decryption, RSA-2048
  keygen and wire encoding, RS256 and PS256 signing, RSA-OAEP session key
  recovery, and the OAuth client assertion
