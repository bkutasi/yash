# Experimental memory-efficient Yash

This working tree is based on upstream commit
`0105ae707bce655034522372f8b2a20072819c28`. It carries the preserved
experimental compact prediction tree, pooled history entries, compressed
in-memory history, and the accompanying line-editor highlighting changes.
The native Yash history file format is unchanged. This is experimental, not
an upstream release.

## What the experiment changes

History entries are allocated from pools rather than as many separate
allocations. This reduces per-entry allocator overhead and can make a large
history take less memory. Older history text is kept in zlib-compressed
blocks; decompressing a block when it is needed trades some work and access
latency for a smaller in-memory history footprint. The configuration below
uses 32 KiB blocks, compression level 9, and no idle decoded-block cache, so
it favors a smaller working set over keeping decoded data readily available.

The compact predictor stores prediction data in a more compact tree, reducing
the memory used by the line editor's prediction structure. These are in-memory
representation changes only: Yash continues to read and write its native
history file format, so the on-disk format is unchanged.

## Build the measured configuration

On the Linux machine used for this checkout, GCC, make, zlib development
headers, and the terminfo development library were available. From a fresh
checkout:

```sh
./configure 'CC=gcc' \
  'CFLAGS=-std=c99 -Os -g -D_GNU_SOURCE -DYH_V5_POOL=1 -DYH_COLD_SHIFT=15 -DYH_COLD_CACHE=0 -DYH_COLD_ZLIB=1 -DYH_COLD_LEVEL=9' \
  'LDLIBS=-ltinfo -lz -lm'
make --jobs=8 yash
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

## Build a musl package tree with Docker

With Docker available, build from a clean checkout and export the files into a
new, empty local directory:

```sh
docker build --output type=local,dest=./yash-musl-build --file Dockerfile.musl .
```

The resulting binary is `./yash-musl-build/usr/local/bin/yash`, with support
data at `./yash-musl-build/usr/local/share/yash`. This only creates the
exported build tree: it does not install Yash on the host or change a login
shell. The binary expects support files under `/usr/local/share/yash` by
default; copying just the binary may leave completions or initialization
scripts unavailable. Package versions used by the Docker build are not pinned.

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
`history1-y.tst`, `history2-y.tst`, and `fc-y.tst`. In an isolated musl
validation of commit `fbc37d73`, the baseline and candidate full suites
matched: 20,254 passed, four known pattern-matching cases failed, and 53 were skipped.
The failure details matched as well. This is a baseline comparison, not a
claim that the full suite passed without errors.

## Scope and limitations

The original benchmark reported 233.28 MiB summed RSS for 100 shells with
23,337 history entries, versus approximately 335.6 MiB for its V5 baseline.
In a later rerun of the published source with the same workload, the V5
baseline used 336.37 MiB and the candidate averaged 233.26 MiB across two
runs, about 30.7% less. Baseline controls stayed stable. Summed PSS was
289.88 MiB for the baseline and 180.62 MiB for the candidate average. The
candidate's repeated-prompt p95 was 17.84 ms versus 11.69 ms for the initial
baseline.

This is a V5 comparison, not a comparison with zsh. Shared pages are counted
repeatedly in summed RSS, and actual memory savings depend on workload. The
benchmark's musl test environment had four matching baseline/candidate pattern
failures; these were accepted for the experiment, not shown to be fixed. The
latency cost and lower memory use describe this benchmark only, not a general
performance guarantee. The pool implementation is restricted to 64-bit
pointers and scans blocks when freeing entries. History access also relies on
the compiler's `cleanup` attribute to release borrowed blocks. GCC was tested;
compilers that discard that attribute can leave blocks pinned and must not be
used for this build without a compatible lifetime mechanism.

Review all changes before publishing. Do not add history files, private
corpora, shell configuration, build output, or raw experiment artifacts.
