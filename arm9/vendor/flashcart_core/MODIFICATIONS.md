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
