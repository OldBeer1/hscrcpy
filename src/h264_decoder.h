#ifndef HSCRCPY_H264_DECODER_H
#define HSCRCPY_H264_DECODER_H

#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVCodecParserContext;
struct AVFrame;
struct SwsContext;

namespace hscrcpy {

struct DecodedImage {
    std::vector<uint8_t> data;
    uint32_t width  = 0;
    uint32_t height = 0;
    int stride      = 0;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    bool init();
    void cleanup();
    bool decode(const uint8_t *h264_data, size_t h264_size);
    const DecodedImage &image() const { return image_; }
    DecodedImage &image_ref() { return image_; }
    bool is_initialized() const { return initialized_; }

private:
    DecodedImage image_;
    bool initialized_ = false;

    AVCodecContext *codec_ctx_  = nullptr;
    AVCodecParserContext *parser_ctx_ = nullptr;
    AVFrame *frame_      = nullptr;
    AVFrame *frame_rgb_  = nullptr;
    SwsContext *sws_ctx_ = nullptr;

    bool sws_initialized_ = false;
    int last_width_  = 0;
    int last_height_ = 0;
    int last_pix_fmt_ = 0;
};

} // namespace hscrcpy

#endif
