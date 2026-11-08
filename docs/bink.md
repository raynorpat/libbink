# Bink 1 format notes

This document describes the Bink 1 container and video bitstream as
implemented by libbink. It is a porting reference, not a formal
specification. RAD Game Tools never published a complete specification; the
description below follows FFmpeg's `libavformat/bink.c` and
`libavcodec/bink.c`, which are the authoritative behavior for the four
revisions supported here (`BIKf`, `BIKg`, `BIKh`, `BIKi`).

The MultimediaWiki Bink article was also used as background prose:
<https://wiki.multimedia.cx/index.php/Bink_Container> and
<https://wiki.multimedia.cx/index.php/Bink_Video>. Where the wiki and the
FFmpeg implementation disagree, libbink follows FFmpeg, because the
reference checksums in this repository were captured from VLC, which uses
FFmpeg's decoder.

## Attribution

The decoder is derived from:

- `libavcodec/bink.c`, copyright (c) 2006-2007 Konstantin Shishkov and
  (c) 2007-2008 Peter Ross
- `libavcodec/binkdsp.c`, copyright (c) 2009 Konstantin Shishkov
- `libavcodec/binkdata.h`, same authors
- `libavcodec/binkaudio.c`, copyright (c) 2007-2011 Peter Ross and
  (c) 2009 Daniel Verkamp

Those files are licensed under the GNU Lesser General Public License,
version 2.1 or later. libbink keeps the same license. See `LICENSE` and
`src/bink_dsp.c` for the retained copyright headers.

## Container layout

All multi-byte integers are little-endian. Offsets below are relative to
the start of the file.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Signature: `BIKf`, `BIKg`, `BIKh`, or `BIKi` |
| 4 | 4 | File size minus 8 |
| 8 | 4 | Frame count |
| 12 | 4 | Largest frame packet size |
| 16 | 4 | Reserved / unknown (skipped) |
| 20 | 4 | Width in pixels |
| 24 | 4 | Height in pixels |
| 28 | 4 | FPS numerator |
| 32 | 4 | FPS denominator |
| 36 | 4 | Video flags (`0x00100000` alpha, `0x00020000` gray) |
| 40 | 4 | Audio track count (0 to 7 supported here) |

The fourth signature byte is the codec revision. FFmpeg compares only the
low 24 bits against `BIK` and treats the high byte as the revision. Bink 2
files use a `KB2` signature and are not handled by this decoder. The legacy
`BIKb` and `BIKk` revisions use a different plane decoder and are rejected.

When the audio track count is non-zero, the header continues with:

| Field | Size | Notes |
| --- | --- | --- |
| Maximum decoded size, per track | `4 * tracks` | PCM buffer size for that track, in bytes |
| Sample rate, per track | `2` | Little-endian Hz |
| Flags, per track | `2` | `0x1000` = DCT, otherwise RDFT; `0x2000` = stereo |
| Track ID, per track | `4` | Skipped |

The maximum-decoded-size words are the track's output buffer. libbink reads
them and reports the rate, channel count, and 16-bit depth through
`bink_info_audio`. A payload that would write past that size fails the
track for the frame.

## Frame index and packets

Immediately after the header and audio descriptors is a frame index of
`frame_count` little-endian 32-bit entries. In each entry:

- bit 0 is the keyframe flag;
- bits 1..31 are the byte offset of the frame packet in the file.

The packet size is the difference between consecutive offsets; the last
packet ends at the file size. The `largest frame` header field is a size
bound, not an offset.

A frame packet contains one audio chunk per audio track, then the video
payload:

```
for each audio track:
    u32 chunk_size
    chunk_size bytes of audio
video payload
```

For revision `i`, the video payload begins with size words that the decoder
skips (they are not needed for decoding):

```
if alpha:  u32 alpha_plane_size
alpha plane (when present)
           u32 luma_plane_size
Y, U, V planes
```

For revisions `f`, `g`, and `h` there are no size words; the planes (and
alpha, when present) follow immediately after the audio chunks.

## Audio

Each enabled track is Bink Audio, ported from FFmpeg's
`libavcodec/binkaudio.c`. `flags & 0x1000` selects DCT. Otherwise the track
is RDFT. `flags & 0x2000` selects stereo; otherwise the track is mono.
Published samples are native-endian signed 16-bit. Container integers stay
little-endian on every host. On a big-endian host each sample is stored
with the high byte first.

The bytes after `chunk_size` still start with 32 bits. Those bits are the
reported decompressed size and are skipped. The remainder is one or more
transform blocks. Each block overlaps the next by one-sixteenth of the
transform window. `bink_first` and `bink_seek_keyframe` clear that overlap
before the landed frame. An empty chunk (`chunk_size == 0`) adds no samples
and no silence, and it leaves the overlap in place. A chunk the bit reader
cannot finish publishes nothing for that track and leaves the picture and
the other tracks running.

## Bitstream conventions

The video payload is a bitstream read least-significant-bit first. A 32-bit
boundary is restored after each plane: the next plane starts at the next
multiple of 32 bits from the start of the packet.

## Huffman trees

Most bundle data is Huffman-coded with one of sixteen built-in code tables.
Each tree starts with a 4-bit `vlc_num`:

- `0` means the identity mapping: code index `i` maps to symbol `i`.
- Otherwise, a one-bit flag follows.
  - Flag set: a 3-bit count, then that many 4-bit explicit symbols. The
    remaining symbols are filled in ascending order, skipping the explicit
    ones.
  - Flag clear: a 2-bit shuffle depth, then merge bits that recursively
    interleave two halves of a 16-entry symbol list.

The built-in code lengths and codewords live in `src/bink_huffdata.h`
(`bink_tree_lens`, `bink_tree_bits`). Decoding looks up the next up-to-7
bits in the selected table and then applies the tree's symbol mapping. This
last step is easy to miss: the Huffman table yields an index, and the tree's
explicit/shuffled `syms` array maps that index to the actual bundle symbol
(FFmpeg's `GET_HUFF` macro).

## Bundles

Each plane is described by nine bundles, in this order:

| # | Bundle | Contents |
| --- | --- | --- |
| 0 | Block types | One byte per 8x8 block |
| 1 | Sub block types | One byte per scaled (16x16) block |
| 2 | Colors | Pixel values |
| 3 | Pattern | 8-bit two-color patterns |
| 4 | X offset | Signed 8-bit motion X |
| 5 | Y offset | Signed 8-bit motion Y |
| 6 | Intra DC | Signed 16-bit DCT DC values |
| 7 | Inter DC | Signed 16-bit DCT DC values |
| 8 | Run | Run lengths for pattern fills |

At the start of each plane all nine tree headers are read once. Then, for
every block row, the decoder reads a data block for each bundle. Bundle
length fields are computed from the plane width:

```
width = align(width, 8)
block_types.len     = log2((width >> 3) + 511) + 1
sub_block_types.len = log2((width >> 4) + 511) + 1
colors.len          = log2(bw * 64 + 511) + 1
pattern.len         = log2((bw << 3) + 511) + 1
x/y/intra/inter DC  = log2((width >> 3) + 511) + 1
run.len             = log2(bw * 48 + 511) + 1
```

A bundle data block begins with a count of that many bits. A zero count
disables further data for that bundle for the rest of the plane. Depending
on the bundle, the count is followed by a single repeated value, Huffman
symbols, signed values, or delta-coded values. libbink validates every
bundle write against its scratch buffer.

## Block types

The block type byte selects one of ten 8x8 block operations:

| Value | Name | Operation |
| --- | --- | --- |
| 0 | Skip | Copy the same block from the previous frame |
| 1 | Scaled | 16x16 block; the low 2x2 sub-blocks are described by sub block types |
| 2 | Motion | Copy an 8x8 reference block at the signaled X/Y offset |
| 3 | Run | Fill pixels in a pattern order, using run lengths |
| 4 | Residue | Motion copy, then add a sparse signed residue |
| 5 | Intra | DC plus IDCT, written directly |
| 6 | Fill | Fill the block with one color |
| 7 | Inter | Motion copy, then DC plus IDCT added to it |
| 8 | Pattern | Fill using two colors and an 8x8 bit pattern |
| 9 | Raw | Copy 64 literal color bytes |

Scaled blocks use the sub-block type with the same numeric values (0..9)
for each 2x2 group of 8x8 blocks. A scaled block on an odd row or column is
skipped: it is the second half of a 16x16 block already decoded.

Motion vectors are signed bytes taken from the X and Y bundles. A motion
copy reads from the previous frame at the same position plus the vector. On
the first frame there is no previous frame, so the reference is the frame
being decoded. libbink uses an overlap-safe row copy because source and
destination blocks can partially overlap.

## DCT coefficients

Intra and inter blocks start with a DC value from the DC bundle. The
remaining coefficients are coded as a series of masks:

1. A 4-bit value gives the number of refinement rounds.
2. A list of candidate coefficients (4, 24, 44, 1, 2, 3 initially) is
   scanned. Bits signal whether a candidate is refined, deferred, or coded.
3. Coded coefficients store a magnitude in the current round's bit width
   and a sign bit. The low round uses a single sign bit for ±1.
4. A final 4-bit value selects one of sixteen quantization matrices
   (`bink_intra_quant` or `bink_inter_quant`).

Coefficients are unquantized in place (`value * quant >> 11`), the DC is
scaled the same way, and an integer IDCT (from `binkdsp.c`) produces the
block. Intra blocks replace the destination; inter blocks add to the
motion-compensated pixels.

Residue blocks use a different sparse code: a 7-bit count limits the number
of nonzero coefficients, a 3-bit value selects the first mask, and each mask
round signals additions or new signed values. The resulting 8x8 block is
added to the motion-compensated pixels with byte wraparound.

## Version differences

- `f` and `g`: no per-plane size prefixes, and the bitstream plane order maps
  directly to Y, U, V.
- `h`: the second and third coded planes are swapped when stored into the
  output frame, so the output still exposes U as plane 1 and V as plane 2.
- `i`: 32-bit size prefixes precede the alpha and luma planes, and the color
  bundle no longer applies the older signed-to-unsigned conversion.
- `b` and `k`: rejected. They use FFmpeg's separate `binkb_decode_plane`
  path, which libbink does not implement.

libbink supports the four revisions produced by the test files and by
common Bink 1-era games. Files that use the old `BIKb` plane decoder must be
converted or decoded with a different tool.

## Relationship to the MultimediaWiki description

The MultimediaWiki prose is useful for orientation but is not precise enough
to decode files byte-for-byte. In particular, the wiki does not fully
describe:

- the exact per-bundle length formulas above;
- the two different tree-descriptor forms and the final `syms` mapping;
- the plane size prefixes and plane-order swap in revisions `h` and `i`;
- the precise coefficient-mask state machine and quantization tables.

For those details this document and the FFmpeg source are the reference.
The FNV-1a checksums in `tests/reference_checksums.h` pin the implementation
to VLC/FFmpeg output for the five sample files, which is the strongest
available correctness check.
