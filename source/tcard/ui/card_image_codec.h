#pragma once
#include "../core/card_clipboard.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <span>
#include <cstdint>
#include <cmath>

namespace tcard_codec {
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<BYTE>;
constexpr size_t imageLimit=8*1024*1024,targetBytes=2*1024*1024,inputLimit=64*1024*1024+1024;
struct Apartment {HRESULT value=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~Apartment(){if(SUCCEEDED(value))CoUninitialize();}};
class Source {
    Apartment apartment_;
    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IStream> stream_;
    ComPtr<IWICBitmapDecoder> decoder_;
public:
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width=0,height=0;GUID container{};
    explicit Source(const Bytes& bytes)
    {
        if(bytes.empty()||bytes.size()>inputLimit)return;
        stream_.Attach(SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size())));
        if(!stream_||FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory_)))||
           FAILED(factory_->CreateDecoderFromStream(stream_.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder_))||
           FAILED(decoder_->GetContainerFormat(&container))||FAILED(decoder_->GetFrame(0,&frame))||FAILED(frame->GetSize(&width,&height))||
           !width||!height||width>8192||height>8192||uint64_t(width)*height>16000000||
           (container!=GUID_ContainerFormatPng&&container!=GUID_ContainerFormatJpeg&&container!=GUID_ContainerFormatGif&&container!=GUID_ContainerFormatBmp))frame.Reset();
    }
    std::wstring mime()const{return container==GUID_ContainerFormatJpeg?L"jpeg":container==GUID_ContainerFormatGif?L"gif":container==GUID_ContainerFormatBmp?L"bmp":L"png";}
    Bytes pixels()
    {
        if(!frame)return {};
        ComPtr<IWICFormatConverter> converter;
        if(FAILED(factory_->CreateFormatConverter(&converter))||FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
        BITMAPINFOHEADER header{};header.biSize=sizeof(header);header.biWidth=width;header.biHeight=-static_cast<LONG>(height);header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;
        Bytes result(sizeof(header)+size_t(width)*height*4);memcpy(result.data(),&header,sizeof(header));auto* pixels=result.data()+sizeof(header);
        if(FAILED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(result.size()-sizeof(header)),pixels)))return {};
        for(size_t at=0;at<size_t(width)*height*4;at+=4){const UINT alpha=pixels[at+3];for(size_t c=0;c<3;++c)pixels[at+c]=static_cast<BYTE>((pixels[at+c]*alpha+255*(255-alpha))/255);pixels[at+3]=0;}
        return result;
    }
    Bytes encode(double scale=1,bool jpeg=false)
    {
        if(!frame||scale<=0||scale>1)return {};
        const UINT w=(std::max)(1U,static_cast<UINT>(width*scale)),h=(std::max)(1U,static_cast<UINT>(height*scale));
        ComPtr<IWICBitmapSource> pixels=frame;ComPtr<IWICBitmapScaler> scaler;
        if(w!=width||h!=height){if(FAILED(factory_->CreateBitmapScaler(&scaler))||FAILED(scaler->Initialize(frame.Get(),w,h,WICBitmapInterpolationModeFant)))return {};pixels=scaler;}
        ComPtr<IWICFormatConverter> converter;const auto format=jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;
        if(FAILED(factory_->CreateFormatConverter(&converter))||FAILED(converter->Initialize(pixels.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
        ComPtr<IStream> output;output.Attach(SHCreateMemStream(nullptr,0));ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> target;ComPtr<IPropertyBag2> options;
        if(!output||FAILED(factory_->CreateEncoder(jpeg?GUID_ContainerFormatJpeg:GUID_ContainerFormatPng,nullptr,&encoder))||FAILED(encoder->Initialize(output.Get(),WICBitmapEncoderNoCache))||FAILED(encoder->CreateNewFrame(&target,&options)))return {};
        if(jpeg&&options){PROPBAG2 property{};property.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=0.85f;if(FAILED(options->Write(1,&property,&quality)))return {};}
        auto pixelFormat=format;
        if(FAILED(target->Initialize(options.Get()))||FAILED(target->SetSize(w,h))||FAILED(target->SetPixelFormat(&pixelFormat))||pixelFormat!=format||
           FAILED(target->WriteSource(converter.Get(),nullptr))||FAILED(target->Commit())||FAILED(encoder->Commit()))return {};
        STATSTG size{};LARGE_INTEGER start{};if(FAILED(output->Stat(&size,STATFLAG_NONAME))||!size.cbSize.QuadPart||size.cbSize.QuadPart>inputLimit||FAILED(output->Seek(start,STREAM_SEEK_SET,nullptr)))return {};
        Bytes result(static_cast<size_t>(size.cbSize.QuadPart));ULONG read=0;if(FAILED(output->Read(result.data(),static_cast<ULONG>(result.size()),&read))||read!=result.size())return {};return result;
    }
};
inline Bytes bitmap(const Bytes& dib)
{
    if(dib.size()<sizeof(BITMAPINFOHEADER)||dib.size()>inputLimit-sizeof(BITMAPFILEHEADER))return {};
    BITMAPINFOHEADER info{};memcpy(&info,dib.data(),sizeof(info));
    if(info.biSize!=sizeof(info)||info.biCompression!=BI_RGB||info.biPlanes!=1||(info.biBitCount!=24&&info.biBitCount!=32)||
       info.biWidth<=0||info.biWidth>8192||!info.biHeight||info.biHeight<-8192||info.biHeight>8192||info.biClrUsed>256)return {};
    const size_t height=info.biHeight<0?-int64_t(info.biHeight):info.biHeight,stride=((size_t(info.biWidth)*info.biBitCount+31)/32)*4,offset=sizeof(info)+size_t(info.biClrUsed)*sizeof(RGBQUAD);
    if(size_t(info.biWidth)*height>16000000||offset>dib.size()||stride*height>dib.size()-offset)return {};
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfSize=static_cast<DWORD>(sizeof(header)+dib.size());header.bfOffBits=static_cast<DWORD>(sizeof(header)+offset);
    Bytes result(sizeof(header)+dib.size());memcpy(result.data(),&header,sizeof(header));memcpy(result.data()+sizeof(header),dib.data(),dib.size());return result;
}
struct Encoded {Bytes bytes;std::wstring mime;bool oversized=false;};
inline Encoded prepare(const Bytes& input,bool adjust=false)
{
    Source source(input);if(!source.frame)throw std::runtime_error("Unsupported or oversized image dimensions");
    const bool photo=source.container==GUID_ContainerFormatJpeg;Bytes encoded;const Bytes* bytes=&input;
    if(source.container==GUID_ContainerFormatBmp){encoded=source.encode();if(encoded.empty())throw std::runtime_error("Bitmap encoding failed");bytes=&encoded;}
    if(bytes->size()<=imageLimit)return {*bytes,source.container==GUID_ContainerFormatBmp?L"png":source.mime(),false};
    if(!adjust)return {{},{},true};
    double scale=(std::min)(0.9,std::sqrt(double(targetBytes)/bytes->size())*0.95);
    for(unsigned attempt=0;attempt<2;++attempt){auto result=source.encode(scale,photo);if(!result.empty()&&result.size()<=imageLimit)return {std::move(result),photo?L"jpeg":L"png",false};scale*=0.6;}
    throw std::runtime_error("Adjusted image still exceeds the image limit");
}
inline std::wstring uri(const Encoded& image){return L"data:image/"+image.mime+L";base64,"+tcard_clip::base64(image.bytes);}
}
