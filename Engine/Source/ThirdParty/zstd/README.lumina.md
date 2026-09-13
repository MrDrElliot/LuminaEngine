# zstd

Upstream: https://github.com/facebook/zstd, release v1.5.7.

`zstd.c` is upstream's own single-file amalgamation, generated verbatim by their script and not
hand-edited. To refresh it, take the release tarball and run:

```
cd zstd-<version>/build/single_file_libs
python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstd.c zstd-in.c
```

then copy the resulting `zstd.c` here along with `lib/zstd.h`, `lib/zstd_errors.h` and `LICENSE`. Note the absence
of `-k zstd.h`, which would leave a `../zstd.h` include the flat layout here cannot satisfy; without it
the header is inlined into the amalgamation and the copy of `zstd.h` beside it serves the engine.

Two things the amalgamation bakes in are worth knowing. It sets `ZSTD_DISABLE_ASM`, because the
assembly Huffman decoder cannot be amalgamated and the build tool does not assemble `.S` files
anyway; decode is a little slower than an upstream build for that reason. It also defines
`ZSTD_MULTITHREAD`, which is inert at the default of zero workers, and left that way deliberately:
the package container already splits into chunks and compresses them through Task::ParallelFor, so
zstd's own threading would be competing with the engine's for the same cores.
