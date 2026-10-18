#pragma once
#include "../core/card_clipboard.h"
#include <richedit.h>
#include <wincodec.h>
#include <richole.h>
#include <wrl/client.h>
#include <cmath>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace tcard_image {
using Microsoft::WRL::ComPtr;
struct Picture { std::vector<BYTE> dib; LONG width = 0, height = 0; };
inline Picture decode(const std::wstring& uri)
{
    auto bytes = tcard_clip::image_bytes(uri);
    if (bytes.empty()) return {};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Release { bool active; ~Release() { if (active) CoUninitialize(); } } release{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    if (!stream || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 8192 || h > 8192 || static_cast<UINT64>(w) * h > 16000000) return {};
    const double scale = (std::min)(1.0, (std::min)(std::sqrt(4000000.0 / (static_cast<double>(w) * h)), (std::min)(2048.0 / w, 4096.0 / h)));
    const UINT width = (std::max)(1U, static_cast<UINT>(w * scale));
    const UINT height = (std::max)(1U, static_cast<UINT>(h * scale));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return {};
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
    std::vector<BYTE> pixels(width * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return {};
    BITMAPINFOHEADER header{}; header.biSize = sizeof(header); header.biWidth = width; header.biHeight = height;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB; header.biSizeImage = static_cast<DWORD>(pixels.size());
    std::vector<BYTE> dib(sizeof(header) + pixels.size()); memcpy(dib.data(), &header, sizeof(header));
    for (UINT y = 0; y < height; ++y) {
        BYTE* dst = dib.data() + sizeof(header) + (height - y - 1) * width * 4;
        const BYTE* src = pixels.data() + y * width * 4;
        for (UINT x = 0; x < width; ++x) {
            const UINT alpha = src[x * 4 + 3];
            for (UINT channel = 0; channel < 3; ++channel) dst[x * 4 + channel] = static_cast<BYTE>((src[x * 4 + channel] * alpha + 255 * (255 - alpha)) / 255);
            dst[x * 4 + 3] = 0;
        }
    }
    const UINT displayWidth = (std::min)(w, 1200U);
    const UINT displayHeight = (std::max)(1U, height * displayWidth / width);
    return {std::move(dib), MulDiv(displayWidth, 2540, 96), MulDiv(displayHeight, 2540, 96)};
}
class BitmapData final : public IDataObject {
    LONG refs = 1;
    std::vector<BYTE> bytes;
public:
    explicit BitmapData(const std::vector<BYTE>& data) : bytes(data) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) { *out = static_cast<IDataObject*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { auto n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format && format->cfFormat == CF_DIB && (format->tymed & TYMED_HGLOBAL) && format->dwAspect == DVASPECT_CONTENT ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER; ZeroMemory(medium, sizeof(*medium));
        if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes.size()); if (!global) return E_OUTOFMEMORY;
        void* data = GlobalLock(global); if (!data) { GlobalFree(global); return E_OUTOFMEMORY; }
        memcpy(data, bytes.data(), bytes.size()); GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL; medium->hGlobal = global; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override { if (out) out->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
inline bool place(HWND control, size_t at, const Picture& picture, LONG available = 0, DWORD identity = 0, bool undo = false)
{
    ComPtr<IRichEditOle> rich;
    if (!SendMessageW(control, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(rich.GetAddressOf()))) return false;
    ComPtr<IOleClientSite> site; if (FAILED(rich->GetClientSite(&site))) return false;
    ComPtr<ILockBytes> bytes; ComPtr<IStorage> storage;
    if (FAILED(CreateILockBytesOnHGlobal(nullptr, TRUE, &bytes)) ||
        FAILED(StgCreateDocfileOnILockBytes(bytes.Get(), STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE, 0, &storage))) return false;
    ComPtr<IDataObject> data; data.Attach(new BitmapData(picture.dib));
    FORMATETC format{CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    ComPtr<IOleObject> object;
    if (FAILED(OleCreateStaticFromData(data.Get(), IID_IOleObject, OLERENDER_FORMAT, &format, site.Get(), storage.Get(), reinterpret_cast<void**>(object.GetAddressOf())))) return false;
    REOBJECT item{}; item.cbStruct = sizeof(item); item.cp = static_cast<LONG>(at);
    item.poleobj = object.Get(); item.polesite = site.Get(); item.pstg = storage.Get();
    item.dvaspect = DVASPECT_CONTENT; item.dwFlags = REO_BELOWBASELINE; item.dwUser = identity;
    item.sizel = {picture.width, picture.height};
    if (available > 0 && item.sizel.cx > available) {
        item.sizel.cy = (std::max)(1, MulDiv(item.sizel.cy, available, item.sizel.cx));
        item.sizel.cx = available;
    }
    object->GetUserClassID(&item.clsid);
    OleSetContainedObject(object.Get(), TRUE);
    const bool readonly = (GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) != 0;
    SendMessageW(control, EM_SETREADONLY, FALSE, 0);
    SendMessageW(control, EM_SETSEL, at, at + 1);
    SendMessageW(control, EM_REPLACESEL, undo, reinterpret_cast<LPARAM>(L""));
    const bool ok = SUCCEEDED(rich->InsertObject(&item));
    if (!ok) { SendMessageW(control, EM_SETSEL, at, at); SendMessageW(control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"!")); }
    SendMessageW(control, EM_SETREADONLY, readonly, 0);
    return ok;
}
inline std::wstring source_text(std::wstring text)
{
    for (auto& c : text) if (c == L'\xfffc') c = L'!';
    return text;
}
inline void insert(HWND control, const std::wstring& text, const std::vector<bool>& code)
{
    size_t images = 0;
    for (size_t i = 0; i < text.size() && images < 12; ++i) {
        if (text[i] == L'\xfffc') { ++images; continue; }
        if (text[i] != L'!' || (i < code.size() && code[i]) || (i && text[i - 1] == '\\') || i + 1 >= text.size() || text[i + 1] != '[') continue;
        size_t endLabel = text.find(L"](data:image/", i + 2);
        if (endLabel == std::wstring::npos || text.find_first_of(L"\r\n", i) < endLabel) continue;
        const size_t end = text.find(L')', endLabel + 2);
        if (end == std::wstring::npos) continue;
        ++images;
        auto picture = decode(text.substr(endLabel + 2, end - endLabel - 2));
        if (!picture.dib.empty()) place(control, i, picture);
        i = end;
    }
}
}
