# Modifications

Vendored from [flashcart_core](https://github.com/ntrteam/flashcart_core) at
commit `03d464c` (tag `v1.1.0`). Changes by `@tasken`:

- 2026-07-12 -- `devices/ace3dsplus.cpp`: removed the post-init
  `rdid == 0xFFFFFF` failure guard, so Macronix "sleeping flash" clones (which
  report all-`FF` during init) are accepted. Added them to the description.
- 2026-07-12 -- `devices/*.cpp`: reworded the author and description strings to
  a consistent "Works with:" format. No behaviour change.
- 2026-07-12 -- Removed upstream `.github/`.
- 2026-07-23 -- `device.h`: wrapped the `BIT(n)` definition in `#ifndef BIT`, so
  a platform that already defines it keeps its own.
- 2026-07-26 -- `device.h`: `BIT(n)` is now spelled `(1u << (n))`, matching
  libnds's `ndstypes.h` exactly. An identical macro redefinition is legal and
  silent, so the collision with `<nds.h>` is now benign in any include order;
  the previous signed spelling warned whenever a `.cpp` reached `device.h`
  first. Unsigned is also correct on its own terms, since `1 << 31` is signed
  overflow.
- 2026-07-26 -- `devices/r4igold3ds.cpp`, `devices/r4isdhchk.cpp`: removed each
  file's duplicate local `#define BIT`, which shadowed the one in `device.h`
  that both already include above their first use. No behaviour change: every
  use is `BIT(0)`..`BIT(7)` on `uint8_t`, where signed and unsigned agree.
- 2026-08-01 -- `devices/r4isdhchk.cpp`: removed an unused ntrboot buffer-size
  local. This is a build-only warning fix with no executable-code change.
- 2026-08-18 -- `devices/ak2i.cpp`: `getMaxLength()` returns 2MB for HW-81,
  not 16MB. The chip is an SST 39VF1681, which is 16 megabit -- someone read
  that as megabytes. Dumps were 8x too big and a correctly trimmed 2MB image
  was rejected outright, so restores were unreliable
  ([ntrteam/flashcart_core#144](https://github.com/ntrteam/flashcart_core/issues/144),
  unanswered upstream). Old 16MB backups still restore fine. Confirmed on real
  HW-81 hardware by the reporter: a restore of differing contents now works in
  one pass, where 16MB flashes had needed repeating.
- 2026-08-19 -- `devices/datel.cpp`: added a Datel Slot-1 driver, ported from
  ApacheThunder and edo9300's GPL-3.0 datelTool. The driver uses AUXSPI
  transactions, compares each erase block before touching it, and verifies
  every programmed block by readback. EN29LV parts remain detection-only until
  their boot-block geometry is confirmed.
- 2026-08-19 -- `devices/datel.cpp`: SST39VF1681 is capped to 1 MiB. A
  read-only 2 MiB probe on ApacheThunder's Action Replay DS produced two
  byte-identical 1 MiB halves, confirming that this ASIC mirrors the physical
  chip's second MiB. The first MiB is now the supported backup/restore range.
- 2026-08-19 -- `devices/datel.cpp`: moved per-4 KiB read trace lines to
  DEBUG. Write verification deliberately rereads each changed block; logging
  those internal reads at INFO reopened the FAT log for every block and made
  restores needlessly slow.
- 2026-08-19 -- `devices/datel.cpp`: aligned driver-side write progress with
  StreamFlash's absolute `Writing flash` progress. The driver had been drawing
  a block-relative `Writing` indicator while StreamFlash alternated it with
  its chunk-relative label, producing visible bottom-screen flicker.
- 2026-08-21 -- `devices/ak2i.cpp`, `devices/dstt.cpp`, `devices/r4isdhc.cpp`,
  `devices/r4isdhchk.cpp`, `devices/r4igold3ds.cpp`, `flash_util.h`: handle
  every NTR card command result. Reads, initialization, erase/program
  operations, and busy polls now log and stop on transport failure instead of
  continuing with stale or uninitialized data. Cleanup-only commands log their
  failures; FlashUtil also propagates a failed page program instead of relying
  only on later readback verification.
- 2026-08-21 -- `devices/ace3dsplus.cpp`: added a recovery profile for the
  known shared 2 MiB R4iSDHC.hk Dual Core 2021 stock image. The archived image
  and real recovered cart both use `DEEPLABYRINT` / `ADLE` / `EB`,
  NTR-derived Blowfish state, seed `0x00`, Key1 ROMCNT `0x001808F8`, Key2
  ROMCNT `0x00416017`, and `RDID 1540EF`. The profile rejects any other flash
  capacity and disables ntrboot injection; backup and restore stay behind the
  ordinary write combo.
