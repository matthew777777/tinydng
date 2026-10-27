# DNG writer metadata and RawLens integration

The writer supports the metadata transport used by RawLens's single-frame,
HDR+, linear RGB and float-CFA exporters. The extensions originated in RawLens's
vendored patch against TinyDNG `1f18169` and are now part of the public C11 API.
Camera2 metadata extraction, CFA coordinate transforms and lens-shading channel
selection remain the capture application's responsibility.

## TIFF/DNG fields

Set `tinydng_write_image.fields` and `field_count` to an array of
`tinydng_field { tag, type, count, data, size }`. Payloads must already be
serialized in `options.big_endian` byte order. The writer validates the TIFF
field type and exact `count * type_size` length, copies the payload at create,
and sorts the IFD. Duplicates, including automatically generated geometry,
compression and strip/tile tags, fail. Zero-initialize all public structs.

This carries ActiveArea, default crop, fractional black levels and repeat
geometry, white levels, all calibration/color/forward matrices and illuminants,
AsShotNeutral, NoiseProfile (DOUBLE), camera/lens identity, profiles, and other
DNG tags, including DNG 1.7 tags. There is no fixed metadata-tag whitelist.
Use `as_dng = 0` when supplying all DNG tags yourself; `as_dng = 1` also
emits the existing convenience tags from `raw`/`cfa`, which must not be repeated
in `fields`. NewSubfileType is emitted as TIFF LONG for ImageIO compatibility.

OpcodeList1 (51008), OpcodeList2 (51009) and OpcodeList3 (51022) are UNDEFINED
fields. Each contains the complete opcode list: big-endian count, followed by
opcode ID, minimum version, flags, payload length and payload for each opcode.
**Opcode data is always big-endian**, independently of TIFF byte order.
All opcodes, including unknown/future ones, are copied byte-for-byte; the writer
does not execute, reinterpret or validate opcode parameters. Lens shading is
represented by GainMap opcode 9, normally in OpcodeList2. This also preserves
warp, vignette, bad-pixel, mapping and row/column correction opcodes.

For example, with pre-serialized little-endian `noise_bytes` and a big-endian
`opcode_bytes` list:

```c
tinydng_field fields[] = {
    {51041, 12, 6, noise_bytes, 48},
    {51009, 7, opcode_size, opcode_bytes, opcode_size}
};
image.fields = fields;
image.field_count = sizeof(fields) / sizeof(fields[0]);
```

The current IFD limit is 64 total entries, with a combined 256 KiB extras
capacity. Larger metadata is rejected rather than silently truncated.

## EXIF and GPS

Set `exif_fields/exif_field_count` and `gps_fields/gps_field_count` to separate
field lists. The writer deep-copies them, sorts their directories, adds EXIF
(34665) / GPS (34853) pointers to IFD0 and resolves absolute payload offsets.
Use these for exposure, ISO, aperture, focal length, timestamps, GPS coordinates,
altitude, direction, processing method and other EXIF/GPS fields. Lists may
reuse tag numbers across directories. Duplicate tags within a directory and
recursive EXIF/GPS pointers are rejected. Empty lists omit the directories.
Each directory supports 128 fields; these directories currently require
classic TIFF, and are rejected for BigTIFF.

## Sequential output

`options.header_first = 1` writes the header, IFD and all metadata during
`tinydng_writer_create`, then appends strips in ascending index order.
Use a custom `tinydng_write_io` whose write callback accepts only the next
absolute offset to stream to a non-seekable output. Uncompressed classic TIFF
strips are required. `finish` verifies that all strips were written and releases
writer memory. Ordinary mode retains random strip/tile ordering and header
patching at finish. Both modes flush the built-in file sink on success.

## JPEG XL / DNG 1.7

Enable the optional encoder with an installed libjxl (0.8 or newer):

```sh
cmake -S . -B build -DTINYDNG_ENABLE_JXL=ON
cmake --build build -j4
```

CMake discovers libjxl and libjxl_threads through pkg-config, or reuses existing
`jxl`/`jxl_threads` targets in a parent build. For a custom install, set
`CMAKE_PREFIX_PATH` or `PKG_CONFIG_PATH`. Manual builds define `TINYDNG_HAVE_JXL`
and link those two libraries. No network download is performed. The default
build has no libjxl dependency; `tinydng_has_jxl_encoder()` reports availability.

Pass host-order RAW16 pixels through the ordinary writer API:

```c
tinydng_write_options options = {0};
options.compression = TINYDNG_COMPRESSION_JPEG_XL;
/* image describes unsigned 16-bit, single-channel pixels, as usual. */
tinydng_write_file(ctx, "output.dng", &image, &options, &error);
```

TinyDNG encodes losslessly, emits Compression 52546, and adds missing DNGVersion
and DNGBackwardVersion tags with 1.7.0.0. Explicit versions below 1.7 are rejected.
`jxl_effort` defaults to 3 (or set 1..9); `jxl_num_threads` defaults to automatic
(or set 1 for serial encoding). Both single-strip wrappers and streaming
`tinydng_writer_write_strip` accept pixels directly. Each strip becomes one JXL
codestream (with a container when libjxl requires it), including the final
partial strip. Compressed bytes drain through a
reusable 64 KiB staging buffer directly to the output sink. libjxl still owns
its frame and parallel-runner allocations, which are outside TinyDNG's tracked
memory cap. JXL decoding is not included in TinyDNG.

Existing callers can still set `precompressed = 1` and call
`tinydng_writer_write_compressed_strip`, or pass a single codestream in
`image.data/data_size` to the one-shot wrappers. This copy-through path works
without libjxl, preserves bytes verbatim, and leaves codestream validation to
the caller. Tiled and header-first JXL output remain unsupported. Without an
encoder, pixel input returns `TINYDNG_E_UNSUPPORTED` at writer creation.

Camera2 metadata conversion remains in the capture application. Generic metadata
serialization and JXL encoding do not imply interpretation of every DNG 1.7 tag.

## Verification

Both the root build and the standalone `tests/v3_test` build register 10 tests.
To verify the encoder with sanitizers (installed libjxl required):

```sh
cmake -S tests/v3_test -B /tmp/tinydng-jxl-tests \
  -DTINYDNG_ENABLE_JXL=ON -DTINYDNG_TEST_SANITIZE=ON
cmake --build /tmp/tinydng-jxl-tests -j4
ctest --test-dir /tmp/tinydng-jxl-tests --output-on-failure
```

The 2026-10-27 integration passed all 10 tests in default and encoder-enabled
builds, and all 10 with ASan/UBSan in the encoder-enabled build. RawLens passed
58 selected JVM tests, including the native JXL/GPS test without skips, its host
GainMap verifier, and an Android arm64 DNG library build. All 13 vendored base
files matched TinyDNG byte-for-byte after the source sync. These checks do not
replace fresh device captures and external RAW rendering.

`test_v3_writer_metadata` checks both TIFF byte orders, all three opcode lists,
GainMap/NoiseProfile parsing, EXIF/GPS offset resolution, exact pixel round trips,
sequential sink ordering, JXL codestream preservation/version tags, malformed
metadata cleanup and allocation-failure sweeps in both writer modes.

`test_v3_jxl` decodes produced codestreams independently through libjxl and
checks exact RAW16 samples, both TIFF byte orders, automatic DNG 1.7 tags,
multiple strips in reverse write order, partial final strips, threaded/serial
encoding, and behavior in a build without libjxl.
