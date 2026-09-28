# AV1 decoding with VA-API

This fork adds a VA-API decode path for AV1 video on Linux. The phone still
encodes the video. The client selects FFmpeg's native `av1` decoder instead of
`libdav1d`, then asks it to decode through VA-API. Decoded GPU frames are
downloaded as NV12 and converted to YUV420P for scrcpy's existing SDL renderer.
Input controls and audio use scrcpy's normal paths.

The VA-API render node defaults to `/dev/dri/renderD128`. If your GPU uses
another node, set `SCRCPY_VAAPI_DEVICE` before starting scrcpy:

```sh
SCRCPY_VAAPI_DEVICE=/dev/dri/renderD129 scrcpy --video-codec=av1
```

When VA-API initialization fails, the native AV1 decoder falls back to
software decoding. Other video codecs keep their original decoder selection.
The log line `AV1 decoding via VA-API` confirms that hardware decoding was
selected. The rendered frames still cross into system memory, so this is not a
zero-copy path.

This patch was tested with scrcpy 4.1, FFmpeg 9.0.2, Mesa 26.2.3, an AMD
Radeon 610M, and a Pixel 10 Pro XL. At 1280 pixels and a 60 FPS cap, the
client sustained approximately 59–60 FPS with activity on the GPU decode
engine. Results on other hardware may differ.
