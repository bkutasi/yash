# Experimental memory-efficient Yash

This working tree is based on upstream commit
`0105ae707bce655034522372f8b2a20072819c28`. It carries the preserved
experimental compact prediction tree, pooled history entries, compressed
in-memory history, and the accompanying line-editor highlighting changes.
The native Yash history file format is unchanged. This is experimental, not
an upstream release.

## Build the measured configuration

On the Linux machine used for this checkout, GCC, make, zlib development
headers, and the terminfo development library were available. From a fresh
checkout:

```sh
./configure 'CC=gcc' \
  'CFLAGS=-std=c99 -Os -g -D_GNU_SOURCE -DYH_V5_POOL=1 -DYH_COLD_SHIFT=15 -DYH_COLD_CACHE=0 -DYH_COLD_ZLIB=1 -DYH_COLD_LEVEL=9' \
  'LDLIBS=-ltinfo -lz -lm'
make -j8 yash
./yash --norcfile -c 'printf "yash smoke: %s\n" "$((6 * 7))"'
```

The flags select 32 KiB zlib blocks, compression level 9, and no idle decoded
block cache. The project defaults select a different codec and cache, so
running `./configure && make` does **not** reproduce the measured settings.
Library names may differ on other systems. Changing flags in an existing
build requires a fresh build directory or rebuilding every affected object.

This build is dynamically linked against the host libraries. The benchmarked
binary was statically linked with musl in an Alpine container, so this recipe
does not claim binary reproducibility or the same memory usage.

## Focused synthetic checks

The following checks do not use personal history:

```sh
gcc -std=c99 -Os -g -DYH_COLD_SHIFT=15 -DYH_COLD_CACHE=0 \
  -DYH_COLD_ZLIB=1 -DYH_COLD_LEVEL=9 cold-check.c -lz -o /tmp/yash-cold-check
/tmp/yash-cold-check
gcc -std=c99 -Os -g yh-v5-pool-check.c -o /tmp/yash-pool-check
/tmp/yash-pool-check
gcc -std=c99 -Os -g -ffunction-sections -fdata-sections -Wl,--gc-sections \
  yh-indexed-check.c -o /tmp/yash-indexed-check
/tmp/yash-indexed-check
gcc -std=c99 -Os -g -ffunction-sections -fdata-sections -Wl,--gc-sections \
  yh-v4-check.c -o /tmp/yash-v4-check
/tmp/yash-v4-check
/tmp/yash-v4-check --boundary
```

The focused upstream integration cases are `history-y.tst`,
`history1-y.tst`, `history2-y.tst`, and `fc-y.tst`. The full suite was not
rerun for this checkout.

## Scope and limitations

The preserved experiment reported 233.28 MiB summed RSS for 100 shells with
23,337 history entries, versus approximately 335.6 MiB for its V5 baseline.
This is not a comparison with zsh; shared pages are counted repeatedly in
summed RSS, and real savings depend on workload. The experiment reported four
matching baseline/candidate regex failures on its musl test environment.
Its repeated prompt readiness was about 4.6 ms slower. The pool implementation
is restricted to 64-bit pointers and scans blocks when freeing entries.

Review all changes before publishing. Do not add history files, private
corpora, shell configuration, build output, or raw experiment artifacts.
