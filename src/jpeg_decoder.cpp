#include "jpeg_decoder.h"
#include "log.h"
#include <cstring>

#include <objidl.h>
#include <gdiplus.h>

namespace hscrcpy {

JpegDecoder::JpegDecoder() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(reinterpret_cast<ULONG_PTR *>(&gdiplus_token_), &input, NULL);
}

JpegDecoder::~JpegDecoder() {
    if (gdiplus_token_) {
        Gdiplus::GdiplusShutdown(reinterpret_cast<ULONG_PTR>(gdiplus_token_));
    }
}

bool JpegDecoder::decode(const uint8_t *jpeg_data, size_t jpeg_size) {
    if (!jpeg_data || jpeg_size == 0) return false;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, jpeg_size);
    if (!hMem) return false;

    void *pMem = GlobalLock(hMem);
    memcpy(pMem, jpeg_data, jpeg_size);
    GlobalUnlock(hMem);

    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) {
        GlobalFree(hMem);
        return false;
    }

    Gdiplus::Bitmap *bitmap = Gdiplus::Bitmap::FromStream(stream);
    stream->Release();

    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        return false;
    }

    UINT w = bitmap->GetWidth();
    UINT h = bitmap->GetHeight();

    if (image_.width != w || image_.height != h) {
        image_.width  = w;
        image_.height = h;
        image_.stride = w * 4;  // BGRA = 4 bytes/pixel
        image_.data.resize(image_.stride * h);
    }

    Gdiplus::BitmapData bd;
    Gdiplus::Rect rect(0, 0, w, h);
    if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &bd) != Gdiplus::Ok) {
        delete bitmap;
        return false;
    }

    for (UINT y = 0; y < h; y++) {
        auto *src = static_cast<uint8_t *>(bd.Scan0) + y * bd.Stride;
        auto *dst = image_.data.data() + y * image_.stride;
        memcpy(dst, src, w * 4);
    }

    bitmap->UnlockBits(&bd);
    delete bitmap;
    return true;
}

} // namespace hscrcpy
