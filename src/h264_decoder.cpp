#include "h264_decoder.h"
#include "log.h"
#include <cstring>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace hscrcpy {

static bool preload_ffmpeg_dlls() {
    const char *dlls[] = {"swresample-6.dll", "avutil-60.dll", "swscale-9.dll", "avcodec-62.dll"};
    for (auto name : dlls) {
        if (!GetModuleHandleA(name)) {
            HMODULE h = LoadLibraryA(name);
            if (!h) {
                LOGE("LoadLibrary('%s') failed: error %lu", name, GetLastError());
                return false;
            }
            LOGI("Preloaded %s", name);
        }
    }
    return true;
}

H264Decoder::H264Decoder() {}

H264Decoder::~H264Decoder() {
    cleanup();
}

bool H264Decoder::init() {
    if (initialized_) return true;

    if (!preload_ffmpeg_dlls()) {
        LOGE("FFmpeg DLLs not available - gRPC streaming disabled");
        LOGE("Required: avcodec-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll in exe directory");
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("H264 codec not found - FFmpeg DLL may be incompatible");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        LOGE("Could not allocate codec context");
        return false;
    }

    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    codec_ctx_->thread_count = (std::max)(1, (std::min)(hw_threads / 4, 4));
    codec_ctx_->thread_type = FF_THREAD_SLICE;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        LOGE("Could not open codec");
        return false;
    }

    parser_ctx_ = av_parser_init(AV_CODEC_ID_H264);
    if (!parser_ctx_) {
        LOGE("Could not init H264 parser");
        return false;
    }

    frame_ = av_frame_alloc();
    frame_rgb_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frame_ || !frame_rgb_ || !pkt_) {
        LOGE("Could not allocate frames/packet");
        return false;
    }

    initialized_ = true;
    LOGI("H264 decoder initialized (FFmpeg linked, threads=%d, parser=yes)", codec_ctx_->thread_count);
    return true;
}

void H264Decoder::cleanup() {
    if (parser_ctx_) {
        av_parser_close(parser_ctx_);
        parser_ctx_ = nullptr;
    }
    if (pkt_) {
        av_packet_free(&pkt_);
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (frame_rgb_) {
        if (frame_rgb_->data[0])
            av_freep(&frame_rgb_->data[0]);
        av_frame_free(&frame_rgb_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    initialized_ = false;
    sws_initialized_ = false;
    rgb_buf_size_ = 0;
}

bool H264Decoder::decode(const uint8_t *h264_data, size_t h264_size) {
    if (!initialized_ || !h264_data || h264_size == 0) return false;

    bool got_frame = false;
    bool data_accepted = false;
    const uint8_t *data = h264_data;
    size_t data_size = h264_size;

    while (data_size > 0) {
        av_packet_unref(pkt_);
        int ret = av_parser_parse2(parser_ctx_, codec_ctx_,
                                    &pkt_->data, &pkt_->size,
                                    data, static_cast<int>(data_size),
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            LOGE("av_parser_parse2 error ret=%d", ret);
            break;
        }

        data += ret;
        data_size -= static_cast<size_t>(ret);

        if (pkt_->size == 0)
            continue;

        int send_ret = avcodec_send_packet(codec_ctx_, pkt_);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
            continue;
        }
        data_accepted = true;

        while (true) {
            int recv_ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (recv_ret != 0) {
                break;
            }

            int w = codec_ctx_->width;
            int h = codec_ctx_->height;
            int fmt = codec_ctx_->pix_fmt;

            if (w <= 0 || h <= 0) continue;

            bool need_realloc = !sws_initialized_ ||
                (last_width_ != w || last_height_ != h || last_pix_fmt_ != fmt);

            if (need_realloc) {
                if (sws_ctx_) {
                    sws_freeContext(sws_ctx_);
                    sws_ctx_ = nullptr;
                }

                frame_rgb_->width = w;
                frame_rgb_->height = h;
                frame_rgb_->format = AV_PIX_FMT_BGR24;

                if (frame_rgb_->data[0])
                    av_freep(&frame_rgb_->data[0]);

                int img_ret = av_image_alloc(frame_rgb_->data, frame_rgb_->linesize,
                                              w, h, AV_PIX_FMT_BGR24, 4);
                if (img_ret < 0) {
                    LOGE("av_image_alloc failed for %dx%d ret=%d", w, h, img_ret);
                    break;
                }

                sws_ctx_ = sws_getContext(w, h, static_cast<AVPixelFormat>(fmt),
                                          w, h, AV_PIX_FMT_BGR24,
                                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_ctx_) {
                    LOGE("sws_getContext failed for %dx%d fmt=%d", w, h, fmt);
                    break;
                }

                sws_initialized_ = true;
                last_width_ = w;
                last_height_ = h;
                last_pix_fmt_ = fmt;
                rgb_buf_size_ = static_cast<size_t>(frame_rgb_->linesize[0]) * h;
                LOGI("Decoder: %dx%d pix_fmt=%d rgb_buf=%zu", w, h, fmt, rgb_buf_size_);
            }

            sws_scale(sws_ctx_,
                      frame_->data, frame_->linesize, 0, h,
                      frame_rgb_->data, frame_rgb_->linesize);

            image_.width = static_cast<uint32_t>(w);
            image_.height = static_cast<uint32_t>(h);
            image_.stride = frame_rgb_->linesize[0];
            decode_buffer_.resize(rgb_buf_size_);
            memcpy(decode_buffer_.data(), frame_rgb_->data[0], rgb_buf_size_);
            image_.data.swap(decode_buffer_);
            got_frame = true;
        }
    }

    return got_frame || data_accepted;
}

} // namespace hscrcpy
