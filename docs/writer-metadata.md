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

Set `compression = TINYDNG_COMPRESSION_JPEG_XL` (52546) and `precompressed = 1`
for caller-encoded JPEG XL codestreams containing 16-bit single-channel data.
Call `tinydng_writer_write_compressed_strip` once per strip, or pass the single
codestream as `image.data/data_size` to `tinydng_write_memory/write_file`.
Codestream bytes are preserved without endian conversion or raster-size checks.
The writer adds missing DNGVersion and DNGBackwardVersion tags with 1.7.0.0;
explicit versions below 1.7 are rejected. Tiled and header-first JXL modes are
rejected. A non-empty payload is required; codestream validity is caller-owned.

As in RawLens, JPEG XL encoding belongs to the caller (for example libjxl).
TinyDNG does not bundle a JPEG XL encoder or decoder. Its native writer codecs
remain uncompressed, LZW, PackBits and lossless JPEG (including the existing
arithmetic/restart options). This is metadata serialization and JXL copy-through
support, not a claim that every DNG 1.7 feature is interpreted or rendered.

## Verification

`test_v3_writer_metadata` checks both TIFF byte orders, all three opcode lists,
GainMap/NoiseProfile parsing, EXIF/GPS offset resolution, exact pixel round trips,
sequential sink ordering, JXL codestream preservation/version tags, malformed
metadata cleanup and allocation-failure sweeps in both writer modes.
