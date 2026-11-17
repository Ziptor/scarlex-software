#include "runtime_log.h"
#include "TextureManager.h"
#include "game_data.h"

#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()),value.size()};
}
bool valid_weapon_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';
    });
}
std::string icon_file_id(const std::string& csv_id) {
    // The supplied manifest uses these IDs for the same exact weapon names.
    static const std::unordered_map<std::string,std::string> aliases{
        {"m249_m2","lmg_m2"}, {"weapon_drm14_mk2","drm14_m2"},
        {"weapon_drm14_mk3","drm14_m3"}, {"tag12_m1","tag12k_m1"},
        {"weapon_tag12_mk2","tag12k_m2"}, {"weapon_tag12_mk3","tag12k_m3"},
        {"vulture_m1","vulture50_m1"}, {"vulture_m2","vulture50_m2"},
        {"weapon_lbr44_mk2","lbr44_m2"}, {"weapon_dp9_mk2","dp9_m2"}
    };
    const auto it=aliases.find(csv_id);
    return it==aliases.end() ? csv_id : it->second;
}
HRESULT load_png(ID3D11Device* device,const std::filesystem::path& path,
                 ID3D11ShaderResourceView** result,ImVec2& dimensions) {
    if (!device || !result) return E_POINTER;
    struct ComScope {
        HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ~ComScope() { if(SUCCEEDED(status)) CoUninitialize(); }
    } com;
    if (FAILED(com.status) && com.status!=RPC_E_CHANGED_MODE) return com.status;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,
                      WICDecodeMetadataCacheOnLoad,&decoder))) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr=decoder->GetFrame(0,&frame))) return hr;
    UINT width{},height{};
    if (FAILED(hr=frame->GetSize(&width,&height))) return hr;
    if (!width || !height || width>8192 || height>8192) return E_INVALIDARG;
    const float scale=(std::min)(1.0f,512.0f/static_cast<float>((std::max)(width,height)));
    const UINT w=(std::max)(1u,static_cast<UINT>(static_cast<float>(width)*scale));
    const UINT h=(std::max)(1u,static_cast<UINT>(static_cast<float>(height)*scale));
    ComPtr<IWICBitmapScaler> scaled;
    if (FAILED(hr=factory->CreateBitmapScaler(&scaled)) ||
        FAILED(hr=scaled->Initialize(frame.Get(),w,h,WICBitmapInterpolationModeFant))) return hr;
    ComPtr<IWICFormatConverter> rgba;
    if (FAILED(hr=factory->CreateFormatConverter(&rgba)) ||
        FAILED(hr=rgba->Initialize(scaled.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,
                                   nullptr,0.0,WICBitmapPaletteTypeCustom))) return hr;
    const UINT stride=w*4;
    std::vector<BYTE> pixels(static_cast<std::size_t>(stride)*h);
    if (FAILED(hr=rgba->CopyPixels(nullptr,stride,static_cast<UINT>(pixels.size()),pixels.data()))) return hr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width=w; description.Height=h;
    description.MipLevels=description.ArraySize=1;
    description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count=1;
    description.Usage=D3D11_USAGE_IMMUTABLE;
    description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(),stride,0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(hr=device->CreateTexture2D(&description,&data,&texture))) return hr;
    hr=device->CreateShaderResourceView(texture.Get(),nullptr,result);
    if (SUCCEEDED(hr)) dimensions={static_cast<float>(width),static_cast<float>(height)};
    return hr;
}
} // namespace

std::filesystem::path TextureManager::ExecutableDirectory() {
    std::wstring buffer(32768,L'\0');
    const DWORD size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if (!size || size>=buffer.size()) return std::filesystem::current_path();
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
}
TextureManager::TextureManager(ID3D11Device* device,std::filesystem::path base_directory)
    : device_(device),base_directory_(std::filesystem::absolute(base_directory).lexically_normal()) {
    if (base_directory_ != ExecutableDirectory().lexically_normal()) return;
    std::wstring assets(32768, L'\0');
    const auto length = GetEnvironmentVariableW(L"SCARLEX_ASSET_DIR", assets.data(), static_cast<DWORD>(assets.size()));
    if (length && length < assets.size()) {
        assets.resize(length);
        auto candidate = std::filesystem::path(assets);
        std::error_code error;
        if (candidate.is_absolute() && std::filesystem::is_directory(candidate / L"icons", error))
            base_directory_ = candidate.lexically_normal();
    }
}
TextureManager::~TextureManager() { Clear(); }

ImTextureID TextureManager::LoadTexture(const std::string& path) {
    std::u8string encoded;
    encoded.reserve(path.size());
    for (unsigned char c:path) encoded.push_back(static_cast<char8_t>(c));
    auto resolved=std::filesystem::path(encoded);
    if (resolved.is_relative()) resolved=base_directory_/resolved;
    resolved=resolved.lexically_normal();
    auto key=utf8(resolved);
    // Windows paths are case-insensitive for this asset directory.
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) {
        return static_cast<char>(c>='A' && c<='Z' ? c+('a'-'A') : c);
    });
    if (const auto it=textures_.find(key);it!=textures_.end()) return it->second;
    ComPtr<ID3D11ShaderResourceView> view;
    ImVec2 dimensions{};
    const HRESULT hr=load_png(device_.Get(),resolved,view.GetAddressOf(),dimensions);
    if (FAILED(hr)) {
        textures_.emplace(key,ImTextureID{});
        scarlex::diagnostic_printf("[TextureManager] PNG unavailable: %s HRESULT=0x%08lX\n",key.c_str(),static_cast<unsigned long>(hr));
        return {};
    }
    const auto id=reinterpret_cast<ImTextureID>(view.Get());
    // Insertions can allocate; retain COM ownership until both have succeeded.
    sizes_.emplace(id,dimensions);
    try { textures_.emplace(key,id); }
    catch (...) { sizes_.erase(id); throw; }
    static_cast<void>(view.Detach());
    return id;
}

ImTextureID TextureManager::WeaponIcon(const std::string& raw_id) {
    if (!valid_weapon_id(raw_id)) return {};
    const auto normalized=game_data::normalize_id(raw_id);
    const auto canonical_file=icon_file_id(normalized);
    const auto raw_file=icon_file_id(raw_id);
    const auto id=valid_weapon_id(canonical_file) ? LoadTexture("icons/"+canonical_file+".png") : ImTextureID{};
    if (id) return id;
    return raw_file!=canonical_file ? LoadTexture("icons/"+raw_file+".png") : ImTextureID{};
}

ImVec2 TextureManager::IconSize(ImTextureID texture,float width) const noexcept {
    const auto it=sizes_.find(texture);
    if (it==sizes_.end()) return {};
    width=std::isfinite(width) ? std::clamp(width,16.0f,64.0f) : 48.0f;
    const float scale=(std::min)(width/it->second.x,(width*0.5f)/it->second.y);
    return {it->second.x*scale,it->second.y*scale};
}

void TextureManager::PreloadWeaponIcons() {
    std::error_code error;
    const auto folder=base_directory_/L"icons";
    for (std::filesystem::directory_iterator it(folder,error),end;!error && it!=end;it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension()==L".png")
            static_cast<void>(LoadTexture(utf8(it->path())));
    }
    if (error) scarlex::diagnostic_printf("[TextureManager] Cannot enumerate icons folder: %s\n",error.message().c_str());
    scarlex::diagnostic_printf("[TextureManager] Weapon icons ready: %zu\n",loaded_count());
}

void TextureManager::Clear() noexcept {
    for (const auto& [key,id]:textures_) if (id) reinterpret_cast<ID3D11ShaderResourceView*>(id)->Release();
    textures_.clear(); sizes_.clear();
}

