#include "h264_decoder.h"
#include "log.h"
#include <cstring>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace hscrcpy {

H264Decoder::H264Decoder() {}

H264Decoder::~H264Decoder() {
    cleanup();
}

bool H264Decoder::init() {
    if (initialized_) return true;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("H264 codec not found");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        LOGE("Could not allocate codec context");
        return false;
    }

    parser_ctx_ = av_parser_init(AV_CODEC_ID_H264);
    if (!parser_ctx_) {
        LOGE("Could not init H264 parser");
        return false;
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        LOGE("Could not open codec");
        return false;
    }

    frame_ = av_frame_alloc();
    frame_rgb_ = av_frame_alloc();
    if (!frame_ || !frame_rgb_) {
        LOGE("Could not allocate frames");
        return false;
    }

    initialized_ = true;
    LOGI("H264 decoder initialized (FFmpeg linked)");
    return true;
}

void H264Decoder::cleanup() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (frame_rgb_) {
        av_frame_free(&frame_rgb_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (parser_ctx_) {
        av_parser_close(parser_ctx_);
        parser_ctx_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    initialized_ = false;
    sws_initialized_ = false;
}

bool H264Decoder::decode(const uint8_t *h264_data, size_t h264_size) {
    if (!initialized_ || !h264_data || h264_size == 0) return false;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;

    const uint8_t *data = h264_data;
    int cur_size = static_cast<int>(h264_size);
    bool got_frame = false;

    while (cur_size > 0) {
        int ret = av_parser_parse2(parser_ctx_, codec_ctx_,
                                    &pkt->data, &pkt->size,
                                    data, cur_size,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            LOGW("av_parser_parse2 error");
            break;
        }
        data += ret;
        cur_size -= ret;

        if (pkt->size == 0) continue;

        ret = avcodec_send_packet(codec_ctx_, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == 0) {
                int w = codec_ctx_->width;
                int h = codec_ctx_->height;
                int fmt = codec_ctx_->pix_fmt;

                bool need_realloc = !sws_initialized_ ||
                    (last_width_ != w || last_height_ != h || last_pix_fmt_ != fmt);

                if (need_realloc) {
                    LOGI("Decoder realloc: w=%d h=%d fmt=%d", w, h, fmt);

                    if (sws_ctx_) {
                        sws_freeContext(sws_ctx_);
                        sws_ctx_ = nullptr;
                    }

                    av_frame_unref(frame_rgb_);
                    frame_rgb_->width = w;
                    frame_rgb_->height = h;
                    frame_rgb_->format = AV_PIX_FMT_BGR24;

                    int img_ret = av_image_alloc(frame_rgb_->data, frame_rgb_->linesize,
                                                  w, h, AV_PIX_FMT_BGR24, 1);
                    if (img_ret < 0) {
                        LOGE("av_image_alloc failed for %dx%d ret=%d", w, h, img_ret);
                        continue;
                    }

                    sws_ctx_ = sws_getContext(w, h, static_cast<AVPixelFormat>(fmt),
                                              w, h, AV_PIX_FMT_BGR24,
                                              SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws_ctx_) {
                        LOGE("sws_getContext failed for %dx%d fmt=%d", w, h, fmt);
                        continue;
                    }

                    sws_initialized_ = true;
                    last_width_ = w;
                    last_height_ = h;
                    last_pix_fmt_ = fmt;
                    LOGI("Decoder: %dx%d pix_fmt=%d", w, h, fmt);
                }

                sws_scale(sws_ctx_,
                          frame_->data, frame_->linesize, 0, codec_ctx_->height,
                          frame_rgb_->data, frame_rgb_->linesize);

                uint32_t img_w = static_cast<uint32_t>(frame_rgb_->width);
                uint32_t img_h = static_cast<uint32_t>(frame_rgb_->height);
                int stride = frame_rgb_->linesize[0];

                if (image_.width != img_w || image_.height != img_h) {
                    image_.width = img_w;
                    image_.height = img_h;
                    image_.stride = stride;
                    image_.data.resize(static_cast<size_t>(stride) * img_h);
                }

                if (stride == static_cast<int>(img_w) * 3) {
                    memcpy(image_.data.data(), frame_rgb_->data[0],
                           static_cast<size_t>(stride) * img_h);
                } else {
                    for (uint32_t y = 0; y < img_h; y++) {
                        memcpy(image_.data.data() + y * stride,
                               frame_rgb_->data[0] + y * frame_rgb_->linesize[0],
                               static_cast<size_t>(img_w) * 3);
                    }
                }
                got_frame = true;
            } else {
                break;
            }
        }
    }

    av_packet_free(&pkt);
    return got_frame;
}

} // namespace hscrcpy
