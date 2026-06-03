#ifndef HSCRCPY_JPEG_DECODER_H
#define HSCRCPY_JPEG_DECODER_H

#include "h264_decoder.h"
#include <cstdint>
#include <vector>

#include <windows.h>

namespace hscrcpy {

class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();

    bool decode(const uint8_t *jpeg_data, size_t jpeg_size);
    const DecodedImage &image() const { return image_; }

private:
    DecodedImage image_;
    void *gdiplus_token_ = nullptr;
};

} // namespace hscrcpy

#endif
