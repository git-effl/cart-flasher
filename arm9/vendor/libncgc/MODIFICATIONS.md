# Modifications

Vendored from [libncgc](https://github.com/angelsl/libncgc) at commit
`166dc42`. Changes by `@tasken`:

- 2026-07-16 -- `platform.ntr.make`: BlocksDS-only toolchain; removed the
  devkitARM fallback.
- 2026-07-16 -- `Makefile`: `clean` now also removes `lib/`.
- 2026-08-01 -- `include/ncgc/ntrcard.h`, `src/platform_ntr.c`: added a
  correctly typed reset callback union member and used it directly. This
  removes an incompatible function-pointer cast without changing the callback
  representation or invocation.
