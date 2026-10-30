# Pooled and compressed history

History entries use a pool, and their text is stored in compressed in-memory
blocks. The native Yash history file format and command bytes are unchanged.
Accessing older entries can require decompression, trading some latency for a
smaller memory footprint. This is an experimental implementation.

The pool requires 64-bit pointers and scans allocated blocks when freeing
entries. Borrowed history text is released using the compiler's `cleanup`
attribute; GCC is tested. A compiler that ignores this attribute can leave
blocks pinned and must not be used without a compatible lifetime mechanism.
The default codec is a private LZSS implementation, not LZ4; compressed blocks
are not written to the history file.

## Build

On Linux with GCC, make, zlib headers, and the terminfo development library,
the fork's measured configuration can be built as follows:

```sh
./configure 'CC=gcc' \
  'CFLAGS=-std=c99 -Os -g -D_GNU_SOURCE -DYH_V5_POOL=1 -DYH_COLD_SHIFT=15 -DYH_COLD_CACHE=0 -DYH_COLD_ZLIB=1 -DYH_COLD_LEVEL=9' \
  'LDLIBS=-ltinfo -lz -lm'
make --jobs=8 yash
```

These flags select 32 KiB blocks, zlib compression level 9, and no idle
decoded-block cache. `YH_V5_POOL` is retained from the fork's build command;
the history pool is always enabled in this extraction. Defaults use a
different codec and cache. Change flags only in a fresh build or after
rebuilding all affected objects.

## Static musl build

With Docker and BuildKit, build the Alpine/musl binary and export its install
tree to a new directory outside the source tree:

```sh
out=$(mktemp -d /tmp/yash-musl-output.XXXXXXXX)
docker build -f Dockerfile.musl --output "type=local,dest=$out" .
"$out/usr/local/bin/yash" --noprofile --norcfile -c 'echo $((6 * 7))'
```

The Docker build uses the flags above, static linking, and disables NLS and
documentation. Its `.dockerignore` excludes host object files so they cannot
be reused in the container. Keep the output outside the source tree to avoid
overwriting source or existing build artifacts.

## Synthetic checks

These checks use generated data, not personal history:

```sh
gcc -std=c99 -Os -g -DYH_COLD_SHIFT=15 -DYH_COLD_CACHE=0 \
  -DYH_COLD_ZLIB=1 -DYH_COLD_LEVEL=9 cold-check.c -lz -o cold-check
./cold-check
gcc -std=c99 -Os -g yh-v5-pool-check.c -o yh-v5-pool-check
./yh-v5-pool-check
```

The upstream history integration cases are `history-y.tst`, `history1-y.tst`,
`history2-y.tst`, and `fc-y.tst` under `tests/`. The optional
`YASH_V5_AUDIT_FILE` environment variable causes aggregate storage statistics
to be written to the specified path with a `.cold` suffix. The combined fork's
separate predictor audit file is not included with this history-only feature.
