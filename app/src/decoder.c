#include "decoder.h"

#include <errno.h>
#include <string.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>

#include "util/log.h"

/** Downcast packet_sink to decoder */
#define DOWNCAST(SINK) container_of(SINK, struct sc_decoder, packet_sink)

// scrcpy's SDL renderer accepts planar YUV420P frames. VA-API decodes AV1
// into GPU surfaces, which are downloaded as NV12 and split into three planes.
static AVFrame *
sc_decoder_download_vaapi_frame(const AVFrame *hw_frame) {
    AVFrame *nv12 = av_frame_alloc();
    AVFrame *yuv = av_frame_alloc();
    if (!nv12 || !yuv) {
        LOG_OOM();
        goto error;
    }

    int ret = av_hwframe_transfer_data(nv12, hw_frame, 0);
    if (ret < 0 || nv12->format != AV_PIX_FMT_NV12) {
        LOGE("Could not download AV1 VA-API frame as NV12: %d (format %d)",
             ret, nv12->format);
        goto error;
    }

    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = nv12->width;
    yuv->height = nv12->height;
    ret = av_frame_get_buffer(yuv, 32);
    if (ret < 0) {
        LOGE("Could not allocate YUV420P frame: %d", ret);
        goto error;
    }

    for (int row = 0; row < yuv->height; ++row) {
        memcpy(yuv->data[0] + row * yuv->linesize[0],
               nv12->data[0] + row * nv12->linesize[0], yuv->width);
    }

    int chroma_width = (yuv->width + 1) / 2;
    int chroma_height = (yuv->height + 1) / 2;
    for (int row = 0; row < chroma_height; ++row) {
        const uint8_t *uv = nv12->data[1] + row * nv12->linesize[1];
        uint8_t *u = yuv->data[1] + row * yuv->linesize[1];
        uint8_t *v = yuv->data[2] + row * yuv->linesize[2];
        for (int col = 0; col < chroma_width; ++col) {
            u[col] = uv[2 * col];
            v[col] = uv[2 * col + 1];
        }
    }

    ret = av_frame_copy_props(yuv, hw_frame);
    if (ret < 0) {
        LOGE("Could not copy AV1 frame metadata: %d", ret);
        goto error;
    }

    av_frame_free(&nv12);
    return yuv;

error:
    av_frame_free(&nv12);
    av_frame_free(&yuv);
    return NULL;
}

static bool
sc_decoder_open(struct sc_decoder *decoder, AVCodecContext *ctx,
                const struct sc_stream_session *session) {
    decoder->frame = av_frame_alloc();
    if (!decoder->frame) {
        LOG_OOM();
        return false;
    }

    if (!sc_frame_source_sinks_open(&decoder->frame_source, ctx, session)) {
        av_frame_free(&decoder->frame);
        return false;
    }

    decoder->ctx = ctx;

    // A video stream must have a session
    assert(session || ctx->codec_type != AVMEDIA_TYPE_VIDEO);

    if (session) {
        decoder->session = *session;
    }

    memset(&decoder->frame_size, 0, sizeof(decoder->frame_size));

    return true;
}

static void
sc_decoder_close(struct sc_decoder *decoder) {
    sc_frame_source_sinks_close(&decoder->frame_source);
    av_frame_free(&decoder->frame);
}

static bool
sc_decoder_push(struct sc_decoder *decoder, const AVPacket *packet) {
    bool is_config = packet->pts == AV_NOPTS_VALUE;
    if (is_config) {
        // nothing to do
        return true;
    }

    int ret = avcodec_send_packet(decoder->ctx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LOGE("Decoder '%s': could not send video packet: %d",
             decoder->name, ret);
        return false;
    }

    for (;;) {
        ret = avcodec_receive_frame(decoder->ctx, decoder->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }

        if (ret) {
            LOGE("Decoder '%s', could not receive video frame: %d",
                 decoder->name, ret);
            return false;
        }

        // a frame was received

        if (decoder->ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            assert(decoder->frame->width >= 0);
            assert(decoder->frame->height >= 0);
            struct sc_size frame_size = {
                .width = decoder->frame->width,
                .height = decoder->frame->height,
            };
            if (decoder->frame_size.width != frame_size.width
                    || decoder->frame_size.height != frame_size.height) {
                // The frame size has changed, check if it matches the session
                uint32_t sw = decoder->session.video.width;
                uint32_t sh = decoder->session.video.height;
                if (frame_size.width != sw || frame_size.height != sh) {
                    LOGW("Unexpected video size: %" PRIu32 "x%" PRIu32
                         " (expected %" PRIu32 "x%" PRIu32 ")",
                         frame_size.width, frame_size.height, sw, sh);

                    LOGW("The encoder did not respect the requested size, "
                         "please retry with a lower resolution (-m/--max-size)");
                }
            }

            decoder->frame_size = frame_size;
        }

        AVFrame *downloaded = NULL;
        AVFrame *output = decoder->frame;
        if (decoder->frame->format == AV_PIX_FMT_VAAPI) {
            downloaded = sc_decoder_download_vaapi_frame(decoder->frame);
            if (!downloaded) {
                av_frame_unref(decoder->frame);
                return false;
            }
            output = downloaded;
        }

        bool ok = sc_frame_source_sinks_push(&decoder->frame_source, output);
        av_frame_free(&downloaded);
        av_frame_unref(decoder->frame);
        if (!ok) {
            // Error already logged
            return false;
        }
    }

    return true;
}

static bool
sc_decoder_push_session(struct sc_decoder *decoder,
                        const struct sc_stream_session *session) {
    decoder->session = *session;
    return sc_frame_source_sinks_push_session(&decoder->frame_source, session);
}

static bool
sc_decoder_packet_sink_open(struct sc_packet_sink *sink, AVCodecContext *ctx,
                            const struct sc_stream_session *session) {
    struct sc_decoder *decoder = DOWNCAST(sink);
    return sc_decoder_open(decoder, ctx, session);
}

static void
sc_decoder_packet_sink_close(struct sc_packet_sink *sink) {
    struct sc_decoder *decoder = DOWNCAST(sink);
    sc_decoder_close(decoder);
}

static bool
sc_decoder_packet_sink_push(struct sc_packet_sink *sink,
                            const AVPacket *packet) {
    struct sc_decoder *decoder = DOWNCAST(sink);
    return sc_decoder_push(decoder, packet);
}

static bool
sc_decoder_packet_sink_push_session(struct sc_packet_sink *sink,
                                    const struct sc_stream_session *session) {

    struct sc_decoder *decoder = DOWNCAST(sink);
    return sc_decoder_push_session(decoder, session);
}

void
sc_decoder_init(struct sc_decoder *decoder, const char *name) {
    decoder->name = name; // statically allocated
    sc_frame_source_init(&decoder->frame_source);

    static const struct sc_packet_sink_ops ops = {
        .open = sc_decoder_packet_sink_open,
        .close = sc_decoder_packet_sink_close,
        .push = sc_decoder_packet_sink_push,
        .push_session = sc_decoder_packet_sink_push_session,
    };

    decoder->packet_sink.ops = &ops;
}
