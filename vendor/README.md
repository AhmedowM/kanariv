# Vendor Directory -- Provenance Manifest

| File | Upstream URL | Version/Tag | Date | License |
|------|--------------|-------------|------|---------|
| sqlite3.c, sqlite3.h | https://sqlite.org | 3.53.4 | 2026-07 | Public Domain |
| blake3.c, blake3.h, blake3_impl.h, blake3_portable.c | https://github.com/BLAKE3-team/BLAKE3 | 1.8.7 | 2023-??-?? | Apache-2.0 / CC0 |
| miniaudio.h | https://github.com/mackron/miniaudio | 0.11.25 | 2026-03-04 | MIT |
| kissfft/kiss_fft.c, kiss_fft.h, kiss_fft_log.h, _kiss_fft_guts.h, kiss_fftr.c, kiss_fftr.h | https://github.com/mborgerding/kissfft | 131.1.0 | 2021-10-05 | BSD-3-Clause |

## Notes

- **sqlite3**: Amalgamation source from SQLite 3.53.4 (SHA256 `B1DD5D74...8A1DB28189`). Public Domain / Zero-clause BSD.
- **blake3**: BLAKE3 reference implementation v1.8.7. Only `blake3.c`, `blake3.h`, `blake3_impl.h`, and `blake3_portable.c` are vendored. The SIMD dispatch code (`blake3_dispatch.c`) was removed as this build forces portable-only via `BLAKE3_NO_AVX512`, `BLAKE3_NO_AVX2`, `BLAKE3_NO_SSE41`, `BLAKE3_NO_SSE2`, `BLAKE3_USE_NEON=0`. Licensed under Apache-2.0 OR CC0-1.0.
- **miniaudio**: Single-header audio library v0.11.25. Licensed under MIT.
- **kissfft**: KissFFT v131.1.0 (tag `131.1.0`), float build (`FIXED_POINT`/`USE_SIMD` unset). Only the core real/complex transforms are vendored; the `tools/` helpers are not. Built STATIC and linked PRIVATE into `kanariv::dsp`; no installed header includes it. Licensed under BSD-3-Clause (see `vendor/kissfft/COPYING`).