# Cart-Flasher flashcart_core overlay

`vendor/flashcart_core` is pristine upstream `v1.1.0`. Before ARM9
compilation, `scripts/prepare_flashcart_core.sh` copies it to
`generated/flashcart_core` and overlays `files/`.

The overlay contains required core fixes and drivers. Banner profiles and UI
stay in `source/banner_ops.cpp`.

Keep the submodule pristine. Add only unavoidable core changes as clean files
below `files/`.
