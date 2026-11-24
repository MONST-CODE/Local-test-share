// DevLink — engine/media/jpeg_decoder.cpp
#include "jpeg_decoder.hpp"

#include <string>

#include "../browser/remote_frame.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace plpt::media {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct JpegDecoder::Impl {
    bool com_initialized = false;
    ComPtr<IWICImagingFactory> factory;
    HRESULT init_error = S_OK;
    Impl() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized = SUCCEEDED(co); // S_FALSE also counts: balanced below
        // RPC_E_CHANGED_MODE: the thread is already STA, which WIC accepts too.
        init_error = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory, reinterpret_cast<void**>(factory.GetAddressOf()));
    }
    ~Impl() {
        factory.Reset();
        if (com_initialized) CoUninitialize();
    }
};

JpegDecoder::JpegDecoder() : impl_(std::make_unique<Impl>()) {}
JpegDecoder::~JpegDecoder() = default;
bool JpegDecoder::available() { return true; }

Result<void> JpegDecoder::decode(std::span<const std::uint8_t> jpeg, std::uint32_t expected_width,
    std::uint32_t expected_height, DecodedImage& out) {
    auto fail = [](ErrorCode code, const char* what, HRESULT hr = S_OK) {
        return Result<void>::failure(Error{code, what, "jpeg", static_cast<std::int64_t>(hr)});
    };
    if (!impl_->factory) return fail(ErrorCode::PlatformError, "fabrique WIC indisponible", impl_->init_error);
    if (jpeg.empty() || jpeg.size() > kMaxCompressedPayloadBytes) return fail(ErrorCode::TooLarge, "frame JPEG hors bornes");
    const auto dims = jpeg_dimensions(jpeg);
    if (!dims || dims->width != expected_width || dims->height != expected_height ||
        expected_width > kMaxFrameWidth || expected_height > kMaxFrameHeight)
        return fail(ErrorCode::ProtocolViolation, "dimensions JPEG incohérentes");
    ComPtr<IWICStream> stream;
    HRESULT hr = impl_->factory->CreateStream(&stream);
    if (FAILED(hr)) return fail(ErrorCode::PlatformError, "flux WIC impossible", hr);
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()));
    if (FAILED(hr)) return fail(ErrorCode::PlatformError, "flux WIC impossible", hr);
    ComPtr<IWICBitmapDecoder> decoder;
    // Explicit JPEG decoder: never let WIC pick another (possibly third-party) codec.
    hr = impl_->factory->CreateDecoder(GUID_ContainerFormatJpeg, nullptr, &decoder);
    if (FAILED(hr)) return fail(ErrorCode::PlatformError, "décodeur JPEG WIC indisponible", hr);
    hr = decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand);
    if (FAILED(hr)) return fail(ErrorCode::ProtocolViolation, "JPEG invalide", hr);
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return fail(ErrorCode::ProtocolViolation, "JPEG sans image", hr);
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w != expected_width || h != expected_height) return fail(ErrorCode::ProtocolViolation, "taille décodée incohérente");
    ComPtr<IWICFormatConverter> converter;
    hr = impl_->factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return fail(ErrorCode::PlatformError, "convertisseur WIC impossible", hr);
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return fail(ErrorCode::PlatformError, "conversion RGBA impossible", hr);
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    out.rgba.resize(stride * h);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(out.rgba.size()), out.rgba.data());
    if (FAILED(hr)) return fail(ErrorCode::ProtocolViolation, "décodage JPEG échoué", hr);
    out.width = w;
    out.height = h;
    return Result<void>::success();
}
#else
struct JpegDecoder::Impl {};
JpegDecoder::JpegDecoder() = default;
JpegDecoder::~JpegDecoder() = default;
bool JpegDecoder::available() { return false; }
Result<void> JpegDecoder::decode(std::span<const std::uint8_t>, std::uint32_t, std::uint32_t, DecodedImage&) {
    return Result<void>::failure(Error{ErrorCode::Unsupported,
        "décodeur JPEG invité non intégré sur cette plateforme (Windows uniquement, ADR-018)", "jpeg"});
}
#endif

} // namespace plpt::media
