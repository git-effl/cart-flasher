# Modifications

Vendored from [libncgc](https://github.com/angelsl/libncgc) at commit
`166dc42`. Changes by `@tasken` (build files only; the C/C++ sources are
unmodified):

- 2026-07-16 -- `platform.ntr.make`: BlocksDS-only toolchain; removed the
  devkitARM fallback.
- 2026-07-16 -- `Makefile`: `clean` now also removes `lib/`.
