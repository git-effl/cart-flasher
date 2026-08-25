# Cart-Flasher libncgc overlay

`vendor/libncgc` is pristine upstream `166dc42`. Before compilation,
`scripts/prepare_libncgc.sh` copies it to `generated/libncgc` and overlays
`files/`.

The overlay contains required BlocksDS build support, protocol fixes, and
controlled SPI support for Datel. Keep the submodule pristine. Add only
unavoidable core changes as clean files below `files/`.
