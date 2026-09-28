# AV1 decoding with VA-API

This fork adds a Linux VA-API decode path for AV1 video while keeping scrcpy's
existing display, audio, and input handling.

## Architecture

The Android device encodes AV1 as usual. On the client, the video demuxer
selects FFmpeg's native `av1` decoder, since FFmpeg may otherwise select
`libdav1d`, which has no VA-API decode path. The client initializes a VA-API
device and selects `AV_PIX_FMT_VAAPI` when the decoder offers it. The decoded
GPU frame is downloaded as NV12, separated into YUV420P planes, and passed to
scrcpy's existing frame sinks and SDL renderer. This keeps control, audio,
video buffering, and texture rendering on their existing paths.

The download and format conversion copy each frame through system memory.
This is hardware decoding, but it is not zero-copy rendering.

## Device selection and fallback

The VA-API render node defaults to `/dev/dri/renderD128`. Set
`SCRCPY_VAAPI_DEVICE` if your GPU uses another node:

```sh
SCRCPY_VAAPI_DEVICE=/dev/dri/renderD129 scrcpy --video-codec=av1
```

If VA-API device initialization fails, FFmpeg's native AV1 decoder runs in
software. If the device initializes but the decoder does not offer VA-API,
the decoder uses a software pixel format. A failure while transferring a
decoded GPU frame stops the stream; it does not retry decoding that stream in
software. The log line `AV1 decoding via VA-API` confirms that the hardware
format was selected.

## Scope and tested environment

Only AV1 decoder setup and the handling of VA-API output frames change.
H.264, H.265, audio, input, and server code retain their original paths.
The conversion currently accepts NV12 output, so 10-bit AV1 and other
transfer formats are not supported by this path.

The patch was tested with scrcpy 4.1, FFmpeg 9.0.2, Mesa 26.2.3, an AMD
Radeon 610M, and a Pixel 10 Pro XL. The phone used its hardware AV1 encoder.
At 1280 pixels and a 60 FPS cap, the client sustained approximately 59–60 FPS;
the GPU decode engine showed activity. These observations cover one host and
phone, not other GPUs, pixel formats, or operating systems.
