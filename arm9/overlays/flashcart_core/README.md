# Cart-Flasher flashcart_core overlay

`vendor/flashcart_core` is a pristine upstream submodule pinned to
[`03d464c`](https://github.com/ntrteam/flashcart_core/tree/03d464c8405cabe2f08395645b4b7c98dadc045d)
(`v1.1.0`). Before ARM9 compilation,
`scripts/prepare_flashcart_core.sh` copies it to `generated/flashcart_core`
and overlays the clean files in `files/`.

The overlay retains the required GPL-3.0 core changes: hardened command-result
handling, the AK2i HW81 capacity correction, Ace recovery profiles,
sleeping-flash support, and the Datel driver. Banner profiles, geometry, UI,
exports, and app policy are implemented in `source/banner_ops.cpp`, outside
the core overlay.

Keep the submodule pristine. Add unavoidable core changes as complete clean
files below `files/`, update `MODIFICATIONS.md`, and preserve upstream
licensing notices in the generated source.
