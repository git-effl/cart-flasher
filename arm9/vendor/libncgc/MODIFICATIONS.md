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
- 2026-08-19 -- `include/ncgc/ntrcard.h`, `include/ncgcpp/ntrcard.h`,
  `src/ntrcard.c`, `src/platform_ntrcommon.c`, `src/platform_ntr.c`,
  `src/platform_ctr.c`, `src/test.c`, `src/test.cpp`: exposed per-byte SPI
  transfers and a non-clocking chip-select release. Datel toggle-bit polling
  learns that the final status byte completed an operation only after receiving
  it, so releasing CS must not require clocking an undocumented extra byte.
  `spi_transact` now holds CS through the response-register read and
  `spi_end` leaves the HOLD bit set while disabling SPI, matching datelTool's
  Action Replay DS transaction shape.
