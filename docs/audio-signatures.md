# Bounded audio input signatures

The experimental service copies supported compressed AAC/MP3 audio; it does not transcode audio or reconfigure an existing output track. Each output owner accepts an immutable **input** signature before preparing mux parameters. It compares later reader-owned parameter snapshots and packet configuration against that signature, never against the muxer's converted codec parameters. The owner does not read the demuxer's mutable stream array.

| Input | Signature |
| --- | --- |
| AAC ADTS | Coded audio object type, core sample rate, channel configuration and its canonical AAC channel layout; bounded ASC configuration digest |
| AAC raw / ASC | Core object type/rate/layout parsed from ASC, including explicit HE-AAC/PS wrappers; SHA-256 of the complete configuration |
| MP3 | MPEG version, layer III, header sample rate, mono/stereo coded layout; bitrate, padding and stereo/joint-stereo choices may vary |

`AV_PKT_DATA_NEW_EXTRADATA` takes precedence over the snapshot's extradata. Configuration bytes are bounded to 64 KiB. The supported AAC parser accepts core object types Main/LC/SSR/LTP and explicit SBR/PS wrappers; channel configuration zero (PCE) and other configurations are rejected conservatively. Canonical channel configuration 3 means surround, not a generic three-channel default. Initial ADTS-to-ASC synthesis is equivalent to the same two-byte input ASC.

AAC core frequency/layout and the decoder's SBR/PS rendered output are different quantities. Stable SBR/PS metadata must not be compared to core ADTS/ASC fields as though the input had changed. Configuration detection observes input metadata, ADTS/MP3 headers and ASC changes; it does not decode every compressed audio payload or prove arbitrary in-band payload validity.

After a change, `audio_track_changed_unsupported` records its reason once for that track/output. That packet and subsequent packets on the rejected track increment `audio_unsupported`. Other supported audio tracks and video continue. No stream removal, output reopen or replacement track is attempted. Initial unsupported configurations use `audio_track_rejected`. Every retained channel layout is released at owner teardown.

`tests/audio_signature_test.c` covers bounded parsing, canonical layouts, configuration precedence, core/rendered separation and allowed MP3 header variation. `tests/audio_signature_integration.py` uses independently encoded AAC/MP3 phases on one MPEG-TS PID with continuous timestamps and a healthy audio sibling; it checks actual input/output payload order, one rejection event, video completion and queue/reference closure. A separate C fixture writes real FLV AAC sequence-header changes on one track with continuous audio time and reordered video PTS. Stable single/multiple-output HE-AAC uses FFmpeg's official FATE sample `aac/al_sbr_cm_48_2.mp4`, 199678 bytes, SHA-256 `7e7c0390c950eda21775d1a193e05ee703257bed85602884e7d30a8b3c65be90`. Set `NAKI_HE_AAC_FIXTURE` to an existing matching file for offline runs; otherwise that test retrieves the pinned public sample into its unique `out/` directory.

The nine real cases passed in `out/audio-signature-blcbaxis/COMPLETE.json`. That run predates the subsequent W08 candidate; combined candidate regression is recorded separately.
