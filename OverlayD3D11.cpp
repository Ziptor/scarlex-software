#include "runtime_log.h"
#include "OverlayD3D11.h"
#include "core/types.h"
#include "game_entities.h"
#include "TextureManager.h"
#include "mesh_overlay_shader.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t window_class[] = L"RobRunTool.OverlayD3D11";
constexpr DXGI_FORMAT target_format = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT swap_chain_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr std::size_t max_mesh_vertices = 1'200'000;
constexpr std::size_t max_mesh_indices = 6'000'000;
constexpr LONG_PTR passive_styles = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

class ThreadDpiScope final {
public:
    ThreadDpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~ThreadDpiScope() {
        if (previous_) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
private:
    DPI_AWARENESS_CONTEXT previous_{};
};

HRESULT win32_failure() {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

bool read_target_bounds(HWND target, RECT& bounds) {
    // Iconic windows can report coordinates far outside the desktop.
    RECT client{};
    if (IsIconic(target) || !GetClientRect(target, &client) ||
        client.right <= client.left || client.bottom <= client.top) {
        return false;
    }
    POINT top_left{client.left, client.top};
    if (!ClientToScreen(target, &top_left)) {
        return false;
    }
    bounds = {top_left.x, top_left.y, top_left.x + (client.right - client.left),
              top_left.y + (client.bottom - client.top)};
    return true;
}

HRESULT enforce_passive_input(HWND window) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (styles == 0 && GetLastError() != ERROR_SUCCESS) {
        return win32_failure();
    }
    if ((styles & passive_styles) != passive_styles) {
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWL_EXSTYLE, styles | passive_styles) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            return win32_failure();
        }
        if ((styles & WS_EX_LAYERED) == 0 && !SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) {
            return win32_failure();
        }
        if (!SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            return win32_failure();
        }
    }
    if (GetCapture() == window) {
        ReleaseCapture();
    }
    if (IsWindowEnabled(window)) {
        EnableWindow(window, FALSE);
        if (IsWindowEnabled(window)) {
            return E_FAIL;
        }
    }
    return S_OK;
}

struct MenuPointer {
    ImGuiContext* context{};
    HWND window{}, target{};
    ImVec2 position{};
    RECT bounds{};
    bool raw{};

    [[nodiscard]] bool accepts_input() const noexcept {
        const HWND foreground = GetForegroundWindow();
        return foreground == window || foreground == target;
    }

    void clamp() noexcept {
        position.x = std::clamp(position.x, static_cast<float>(bounds.left),
                                static_cast<float>((std::max)(bounds.left, bounds.right - 1)));
        position.y = std::clamp(position.y, static_cast<float>(bounds.top),
                                static_cast<float>((std::max)(bounds.top, bounds.bottom - 1)));
    }

    void move(const RAWMOUSE& mouse) noexcept {
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            position.x += static_cast<float>(mouse.lLastX);
            position.y += static_cast<float>(mouse.lLastY);
        } else {
            const bool desktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int x = desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
            const int y = desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
            const int w = GetSystemMetrics(desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int h = GetSystemMetrics(desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            POINT point{x + MulDiv(mouse.lLastX, w - 1, 65535), y + MulDiv(mouse.lLastY, h - 1, 65535)};
            if (ScreenToClient(window, &point)) {
                position = {static_cast<float>(point.x), static_cast<float>(point.y)};
            }
        }
        clamp();
    }

    void feed(ImGuiIO& io, const RAWMOUSE& mouse) noexcept {
        move(mouse);
        const auto flags = mouse.usButtonFlags;
        if (flags == 0) return;
        io.AddMousePosEvent(position.x, position.y);
        constexpr std::array<USHORT, 5> down{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
            RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        constexpr std::array<USHORT, 5> up{RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
            RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
        for (int i = 0; i < static_cast<int>(down.size()); ++i) {
            if (flags & down[i]) io.AddMouseButtonEvent(i, true);
            if (flags & up[i]) io.AddMouseButtonEvent(i, false);
        }
        const float delta = static_cast<float>(static_cast<SHORT>(mouse.usButtonData)) / WHEEL_DELTA;
        if (flags & RI_MOUSE_WHEEL) io.AddMouseWheelEvent(0.0f, delta);
        if (flags & RI_MOUSE_HWHEEL) io.AddMouseWheelEvent(delta, 0.0f);
    }

    void stop() noexcept {
        if (raw) {
            const RAWINPUTDEVICE device{1, 2, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&device, 1, sizeof(device));
            raw = false;
        }
    }
};

LRESULT CALLBACK overlay_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* pointer = reinterpret_cast<MenuPointer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (pointer) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(pointer->context);
        if (pointer->raw) {
            if (message == WM_INPUT) {
                RAWINPUT packet{};
                UINT size = sizeof(packet);
                if (pointer->accepts_input() &&
                    GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &packet,
                                    &size, sizeof(RAWINPUTHEADER)) == sizeof(packet) &&
                    packet.header.dwType == RIM_TYPEMOUSE) {
                    pointer->feed(ImGui::GetIO(), packet.data.mouse);
                }
                ImGui::SetCurrentContext(previous);
                return DefWindowProcW(window, message, wparam, lparam);
            }
            if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                ImGui::SetCurrentContext(previous);
                return TRUE;
            }
            if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSELEAVE) {
                ImGui::SetCurrentContext(previous);
                return 0;
            }
        }
        if (message == WM_SETFOCUS) ClipCursor(nullptr);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        ImGui::SetCurrentContext(previous);
        if (handled) return handled;
        if (message == WM_ERASEBKGND) return 1;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        return 0;
    }
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

HRESULT register_window_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = overlay_wndproc;
    wc.lpszClassName = window_class;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return S_OK;
    }
    return win32_failure();
}

void unmatte_menu_logo(std::span<BYTE> rgba) {
    // Recover straight alpha from the original artwork, which was flattened onto black.
    constexpr unsigned black_floor = 6;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned peak = (std::max)({rgba[i], rgba[i + 1], rgba[i + 2]});
        if (peak <= black_floor) {
            rgba[i + 3] = 0;
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            rgba[i + channel] = static_cast<BYTE>((rgba[i + channel] * 255u + peak / 2) / peak);
        }
        rgba[i + 3] = static_cast<BYTE>(
            (rgba[i + 3] * (peak - black_floor) + (255u - black_floor) / 2) / (255u - black_floor));
    }
}

HRESULT create_menu_logo(ID3D11Device* device, ID3D11ShaderResourceView** result) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!resource) return win32_failure();
    const HGLOBAL data = LoadResource(module, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(data));
    const DWORD length = SizeofResource(module, resource);
    if (!bytes || length == 0) return E_FAIL;

    struct ComScope {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) return com.hr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromMemory(bytes, length))) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                     &decoder))) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) return hr;
    UINT source_width{}, source_height{};
    if (FAILED(hr = frame->GetSize(&source_width, &source_height))) return hr;
    if (source_width == 0 || source_height == 0 || source_width > 8192 || source_height > 8192) return E_FAIL;
    constexpr UINT width = 512;
    const UINT height = (std::max)(1u, width * source_height / source_width);
    ComPtr<IWICBitmapScaler> scaled;
    if (FAILED(hr = factory->CreateBitmapScaler(&scaled))) return hr;
    if (FAILED(hr = scaled->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return hr;
    ComPtr<IWICFormatConverter> pixels;
    if (FAILED(hr = factory->CreateFormatConverter(&pixels))) return hr;
    if (FAILED(hr = pixels->Initialize(scaled.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                       nullptr, 0.0, WICBitmapPaletteTypeCustom))) return hr;
    constexpr UINT stride = width * 4;
    std::vector<BYTE> rgba(height * stride);
    if (FAILED(hr = pixels->CopyPixels(nullptr, stride, static_cast<UINT>(rgba.size()), rgba.data()))) return hr;
    unmatte_menu_logo(rgba);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA image{rgba.data(), stride, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(hr = device->CreateTexture2D(&description, &image, &texture))) return hr;
    return device->CreateShaderResourceView(texture.Get(), nullptr, result);
}

ImFont* create_menu_font(ImFontAtlas& atlas) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10));
    if (!resource) return nullptr;
    const HGLOBAL data = LoadResource(module, resource);
    void* bytes = LockResource(data);
    const DWORD length = SizeofResource(module, resource);
    if (!bytes || length == 0 || length > 4'000'000) return nullptr;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false;
    // CSV localization also uses typographic quotes and price-range dashes.
    static constexpr ImWchar text_ranges[]{
        0x0020,0x00FF, 0x0400,0x052F, 0x2000,0x206F,
        0x2DE0,0x2DFF, 0xA640,0xA69F, 0
    };
    return atlas.AddFontFromMemoryTTF(bytes, static_cast<int>(length), 15.0f,
                                     &font, text_ranges);
}

ImFont* configure_overlay_fonts(ImGuiIO& io) {
    io.Fonts->AddFontDefault();
    auto* font = create_menu_font(*io.Fonts);
    if (font) io.FontDefault = font;
    return font;
}

}  // namespace

namespace {

struct MeshVertex {
    core::Vec4 position;
    std::uint32_t color;
};
static_assert(sizeof(MeshVertex) == 20 && offsetof(MeshVertex, color) == 16);

struct MeshBatch {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertex_buffer, index_buffer;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11DepthStencilState> prepass, color_pass;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    std::size_t vertex_capacity{}, index_capacity{};

    HRESULT initialize(ID3D11Device* device) {
        ComPtr<ID3DBlob> vs_code, ps_code, error;
        const auto compile = [&](const char* entry, const char* profile, ID3DBlob** code) {
            const HRESULT hr = D3DCompile(mesh_overlay_shader, sizeof(mesh_overlay_shader) - 1,
                                          "mesh_overlay.hlsl", nullptr, nullptr, entry, profile,
                                          D3DCOMPILE_ENABLE_STRICTNESS, 0, code, error.ReleaseAndGetAddressOf());
            if (FAILED(hr) && error) {
                std::fputs(static_cast<const char*>(error->GetBufferPointer()), stderr);
            }
            return hr;
        };
        constexpr D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        HRESULT hr{};
        if (FAILED(hr = compile("VSMain", "vs_4_0", vs_code.GetAddressOf())) ||
            FAILED(hr = compile("PSMain", "ps_4_0", ps_code.GetAddressOf())) ||
            FAILED(hr = device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                                   nullptr, vs.GetAddressOf())) ||
            FAILED(hr = device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
                                                  nullptr, ps.GetAddressOf())) ||
            FAILED(hr = device->CreateInputLayout(elements, ARRAYSIZE(elements), vs_code->GetBufferPointer(),
                                                  vs_code->GetBufferSize(), layout.GetAddressOf()))) {
            return hr;
        }

        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = TRUE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        depth_desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                D3D11_COMPARISON_ALWAYS};
        depth_desc.BackFace = depth_desc.FrontFace;
        if (FAILED(hr = device->CreateDepthStencilState(&depth_desc, prepass.GetAddressOf()))) {
            return hr;
        }
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D11_COMPARISON_EQUAL;
        depth_desc.StencilEnable = TRUE;
        depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        depth_desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR_SAT,
                                D3D11_COMPARISON_EQUAL};
        depth_desc.BackFace = depth_desc.FrontFace;
        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        D3D11_BLEND_DESC blend_desc{};
        auto& rt = blend_desc.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = device->CreateDepthStencilState(&depth_desc, color_pass.GetAddressOf())) ||
            FAILED(hr = device->CreateRasterizerState(&raster_desc, raster.GetAddressOf())) ||
            FAILED(hr = device->CreateBlendState(&blend_desc, blend.GetAddressOf()))) {
            return hr;
        }
        return S_OK;
    }

    HRESULT resize(ID3D11Device* device, int width, int height) {
        depth.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());
        return FAILED(hr) ? hr : device->CreateDepthStencilView(texture.Get(), nullptr, depth.GetAddressOf());
    }

    static HRESULT upload(ID3D11Device* device, ID3D11DeviceContext* context,
                          const void* data, std::size_t bytes, UINT binding,
                          ComPtr<ID3D11Buffer>& buffer, std::size_t& capacity) {
        if (capacity < bytes) {
            const auto new_capacity = (std::max)(bytes, capacity * 2 + 4096);
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(new_capacity);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = binding;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> pending;
            const HRESULT hr = device->CreateBuffer(&desc, nullptr, pending.GetAddressOf());
            if (FAILED(hr)) {
                return hr;
            }
            buffer = std::move(pending);
            capacity = new_capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped.pData, data, bytes);
            context->Unmap(buffer.Get(), 0);
        }
        return hr;
    }

    HRESULT draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                 int width, int height) {
        if (indices.empty()) {
            return S_OK;
        }
        HRESULT hr{};
        if (FAILED(hr = upload(device, context, vertices.data(), vertices.size() * sizeof(MeshVertex),
                               D3D11_BIND_VERTEX_BUFFER, vertex_buffer, vertex_capacity)) ||
            FAILED(hr = upload(device, context, indices.data(), indices.size() * sizeof(std::uint32_t),
                               D3D11_BIND_INDEX_BUFFER, index_buffer, index_capacity))) {
            return hr;
        }
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        constexpr UINT stride = sizeof(MeshVertex), offset = 0;
        auto* buffer = vertex_buffer.Get();
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
        context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1, &viewport);
        context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        // Resolve the nearest triangle first. The stencil prevents coincident faces from doubling alpha.
        context->OMSetRenderTargets(0, nullptr, depth.Get());
        context->OMSetDepthStencilState(prepass.Get(), 0);
        context->PSSetShader(nullptr, nullptr, 0);
        context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
        context->OMSetRenderTargets(1, &target, depth.Get());
        context->OMSetDepthStencilState(color_pass.Get(), 0);
        context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFFu);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->DrawIndexed(static_cast<UINT>(indices.size()), 0, 0);
        context->OMSetRenderTargets(1, &target, nullptr);
        return S_OK;
    }
};

}  // namespace

struct OverlayD3D11::State {
    HWND target{};
    HWND window{};
    RECT target_rect{};
    RECT shown_rect{};
    int width{};
    int height{};
    bool frame_open{};
    bool dx11_ready{};
    bool win32_ready{};
    bool menu_interactive{};
    bool crosshair = true;
    HRESULT last_present = E_PENDING;
    std::chrono::steady_clock::time_point last_frame{};
    ImGuiContext* imgui{};
    MenuPointer pointer;
    ComPtr<ID3D11ShaderResourceView> logo;
    ImFont* menu_typeface{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain2> swap_chain;
    HANDLE frame_latency_event{};
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> composition_target;
    ComPtr<IDCompositionVisual> visual;
    MeshBatch mesh;
    std::unique_ptr<TextureManager> textures;

    ~State() {
        pointer.stop();
        if (window && IsWindow(window)) {
            if (GetCapture() == window) ReleaseCapture();
            if (GetForegroundWindow() == window && IsWindow(target)) SetForegroundWindow(target);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        if (frame_latency_event) {
            CloseHandle(frame_latency_event);
        }
        if (imgui) {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(imgui);
            if (frame_open) {
                ImGui::EndFrame();
            }
            if (dx11_ready) {
                ImGui_ImplDX11_Shutdown();
            }
            if (win32_ready) {
                ImGui_ImplWin32_Shutdown();
            }
            ImGui::DestroyContext(imgui);
            if (previous != imgui) {
                ImGui::SetCurrentContext(previous);
            }
        }
        if (composition_target) {
            composition_target->SetRoot(nullptr);
            composition->Commit();
        }
        if (context) context->ClearState();
        textures.reset();
        if (window && IsWindow(window)) {
            DestroyWindow(window);
        }
    }

    HRESULT create_render_target() {
        ComPtr<ID3D11Texture2D> buffer;
        const HRESULT hr = swap_chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
        const HRESULT target_hr = device->CreateRenderTargetView(buffer.Get(), nullptr,
                                                                render_target.ReleaseAndGetAddressOf());
        return FAILED(target_hr) ? target_hr : mesh.resize(device.Get(), width, height);
    }

    HRESULT create_graphics(int initial_width, int initial_height) {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(hr = device.As(&dxgi_device)) ||
            FAILED(hr = dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
            FAILED(hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
            return hr;
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(initial_width);
        desc.Height = static_cast<UINT>(initial_height);
        desc.Format = target_format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = swap_chain_flags;

        ComPtr<IDXGISwapChain1> composition_chain;
        if (FAILED(hr = factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr,
                                                               composition_chain.GetAddressOf())) ||
            FAILED(hr = composition_chain.As(&swap_chain)) ||
            FAILED(hr = swap_chain->SetMaximumFrameLatency(1))) {
            return hr;
        }
        frame_latency_event = swap_chain->GetFrameLatencyWaitableObject();
        if (!frame_latency_event) {
            return E_FAIL;
        }
        width = initial_width;
        height = initial_height;
        if (FAILED(hr = mesh.initialize(device.Get())) || FAILED(hr = create_render_target()) ||
            FAILED(hr = DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(composition.GetAddressOf()))) ||
            FAILED(hr = composition->CreateTargetForHwnd(window, TRUE, composition_target.GetAddressOf())) ||
            FAILED(hr = composition->CreateVisual(visual.GetAddressOf())) ||
            FAILED(hr = visual->SetContent(swap_chain.Get())) ||
            FAILED(hr = composition_target->SetRoot(visual.Get())) ||
            FAILED(hr = composition->Commit())) {
            return hr;
        }
        return S_OK;
    }

    HRESULT resize(int new_width, int new_height) {
        if (new_width == width && new_height == height && render_target) {
            return S_OK;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        render_target.Reset();
        const HRESULT hr = swap_chain->ResizeBuffers(0, static_cast<UINT>(new_width),
                                                     static_cast<UINT>(new_height), DXGI_FORMAT_UNKNOWN,
                                                     swap_chain_flags);
        if (FAILED(hr)) {
            return hr;
        }
        width = new_width;
        height = new_height;
        return create_render_target();
    }
};

OverlayD3D11::OverlayD3D11() = default;
OverlayD3D11::~OverlayD3D11() = default;

HRESULT OverlayD3D11::initialize(HWND target) {
    if (state_ || !IsWindow(target)) {
        return E_INVALIDARG;
    }
    const ThreadDpiScope dpi;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HRESULT hr = register_window_class(instance);
    if (FAILED(hr)) {
        return hr;
    }
    auto pending = std::make_unique<State>();
    pending->target = target;
    pending->window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | static_cast<DWORD>(passive_styles),
        window_class, L"ROB RUN Overlay", WS_POPUP | WS_DISABLED,
        0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!pending->window) {
        return win32_failure();
    }
    // Initialize the layered HWND at full window opacity. The swap chain still supplies per-pixel alpha.
    if (!SetLayeredWindowAttributes(pending->window, 0, 255, LWA_ALPHA)) {
        return win32_failure();
    }
    hr = enforce_passive_input(pending->window);
    if (FAILED(hr)) {
        return hr;
    }
    if (!read_target_bounds(target, pending->target_rect)) {
        // The game may already be minimized when the overlay starts.
        const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) {
            return win32_failure();
        }
        pending->target_rect = info.rcWork;
    }
    const auto& bounds = pending->target_rect;
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    hr = pending->create_graphics(w, h);
    if (FAILED(hr)) {
        return hr;
    }

    IMGUI_CHECKVERSION();
    pending->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(pending->imgui);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    pending->menu_typeface = configure_overlay_fonts(io);
    if (!pending->menu_typeface) return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
    pending->textures = std::make_unique<TextureManager>(pending->device.Get());
    pending->win32_ready = ImGui_ImplWin32_Init(pending->window);
    if (!pending->win32_ready) {
        return E_FAIL;
    }
    pending->dx11_ready = ImGui_ImplDX11_Init(pending->device.Get(), pending->context.Get());
    if (!pending->dx11_ready) {
        return E_FAIL;
    }
    const HRESULT logo = create_menu_logo(pending->device.Get(), pending->logo.GetAddressOf());
    if (FAILED(logo)) {
        scarlex::diagnostic_printf("[OverlayD3D11] Logo load failed. HRESULT=0x%08lX\n", static_cast<unsigned long>(logo));
    }
    state_ = std::move(pending);
    return S_OK;
}

HRESULT OverlayD3D11::set_menu_interactive(bool interactive) {
    if (!state_) return E_UNEXPECTED;
    auto& s = *state_;
    if (s.menu_interactive == interactive) return S_OK;

    if (interactive) {
        const ThreadDpiScope dpi;
        constexpr LONG_PTR input_styles = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR styles = GetWindowLongPtrW(s.window, GWL_EXSTYLE);
        if (styles == 0 && GetLastError() != ERROR_SUCCESS) return win32_failure();
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(s.window, GWL_EXSTYLE, styles & ~input_styles) == 0 &&
            GetLastError() != ERROR_SUCCESS) return win32_failure();
        s.pointer = {s.imgui, s.window, s.target};
        s.pointer.bounds = desktop_client_rect();
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(s.window, &cursor)) {
            s.pointer.position = {static_cast<float>(cursor.x), static_cast<float>(cursor.y)};
        }
        s.pointer.clamp();
        const RAWINPUTDEVICE raw_mouse{1, 2, RIDEV_INPUTSINK, s.window};
        s.pointer.raw = RegisterRawInputDevices(&raw_mouse, 1, sizeof(raw_mouse)) != FALSE;
        if (!s.pointer.raw) {
            scarlex::diagnostic_printf("[OverlayD3D11] Raw mouse input unavailable. Win32=%lu\n", GetLastError());
        }
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(s.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s.pointer)) == 0 &&
            GetLastError() != ERROR_SUCCESS) return win32_failure();
        EnableWindow(s.window, TRUE);
        if (!SetWindowPos(s.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                          SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
            return win32_failure();
        }
        if (GetForegroundWindow() == s.target && SetForegroundWindow(s.window)) {
            ClipCursor(nullptr);
        }
    } else {
        s.pointer.stop();
        ImGui::SetCurrentContext(s.imgui);
        // Clear the Win32 backend's capture mask even when a drag is interrupted.
        ImGui_ImplWin32_WndProcHandler(s.window, WM_LBUTTONUP, 0, 0);
        ImGui_ImplWin32_WndProcHandler(s.window, WM_RBUTTONUP, 0, 0);
        ImGui_ImplWin32_WndProcHandler(s.window, WM_MBUTTONUP, 0, 0);
        ImGui_ImplWin32_WndProcHandler(s.window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
        ImGui_ImplWin32_WndProcHandler(s.window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
        if (GetForegroundWindow() == s.window && IsWindow(s.target)) SetForegroundWindow(s.target);
        SetWindowLongPtrW(s.window, GWLP_USERDATA, 0);
        const HRESULT hr = enforce_passive_input(s.window);
        if (FAILED(hr)) return hr;
    }

    ImGui::SetCurrentContext(s.imgui);
    auto& io = ImGui::GetIO();
    constexpr auto disabled_input = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    if (interactive) {
        io.ConfigFlags &= ~disabled_input;
    } else {
        io.ConfigFlags |= disabled_input;
        io.MouseDrawCursor = false;
    }
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.AddFocusEvent(interactive && (GetForegroundWindow() == s.window ||
                                    (s.pointer.raw && s.pointer.accepts_input())));
    s.menu_interactive = interactive;
    return S_OK;
}

void OverlayD3D11::set_crosshair(bool visible) noexcept {
    if (state_) state_->crosshair = visible;
}

bool OverlayD3D11::menu_has_focus() const noexcept {
    return state_ && state_->menu_interactive && GetForegroundWindow() == state_->window;
}

std::uint64_t OverlayD3D11::menu_logo() const noexcept {
    return state_ ? reinterpret_cast<std::uintptr_t>(state_->logo.Get()) : 0;
}

ImFont* OverlayD3D11::menu_font() const noexcept {
    return state_ ? state_->menu_typeface : nullptr;
}

TextureManager* OverlayD3D11::texture_manager() const noexcept {
    return state_ ? state_->textures.get() : nullptr;
}

bool OverlayD3D11::pump_messages() {
    const ThreadDpiScope dpi;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return state_ && IsWindow(state_->window) && IsWindow(state_->target);
}

HRESULT OverlayD3D11::begin_frame() {
    if (!state_ || state_->frame_open) {
        return E_UNEXPECTED;
    }
    auto& s = *state_;
    // Wait before reading the snapshot or recording the frame, including the first frame.
    // The window thread also wakes to dispatch messages while the GPU is busy.
    const DWORD ready = MsgWaitForMultipleObjectsEx(1, &s.frame_latency_event, 50, QS_ALLINPUT,
                                                    MWMO_INPUTAVAILABLE);
    if (ready != WAIT_OBJECT_0) {
        if (ready == WAIT_OBJECT_0 + 1 || ready == WAIT_TIMEOUT) {
            return S_FALSE;
        }
        return ready == WAIT_FAILED ? win32_failure() : E_UNEXPECTED;
    }
    if (!IsWindow(s.target)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    const ThreadDpiScope dpi;
    // Preserve the last good bounds during minimization or a transient geometry failure.
    read_target_bounds(s.target, s.target_rect);
    const auto& bounds = s.target_rect;
    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    const HRESULT hr = s.resize(w, h);
    if (FAILED(hr)) {
        return hr;
    }
    if (!IsWindowVisible(s.window) || !EqualRect(&s.shown_rect, &bounds)) {
        if (!SetWindowPos(s.window, HWND_TOPMOST, bounds.left, bounds.top, w, h,
                          SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
            return win32_failure();
        }
        s.shown_rect = bounds;
    }

    ImGui::SetCurrentContext(s.imgui);
    auto& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(s.width), static_cast<float>(s.height)};
    const auto now = std::chrono::steady_clock::now();
    const float dt = s.last_frame == std::chrono::steady_clock::time_point{}
        ? 1.0f / 60.0f
        : std::chrono::duration<float>(now - s.last_frame).count();
    io.DeltaTime = std::clamp(dt, 0.0001f, 0.25f);
    s.last_frame = now;
    ImGui_ImplDX11_NewFrame();
    if (s.menu_interactive) {
        if (s.pointer.raw) {
            const bool focused = s.pointer.accepts_input();
            io.AddFocusEvent(focused);
            io.MouseDrawCursor = focused;
            s.pointer.bounds = desktop_client_rect();
            s.pointer.clamp();
            if (focused) io.AddMousePosEvent(s.pointer.position.x, s.pointer.position.y);
        } else {
            io.MouseDrawCursor = GetForegroundWindow() == s.window;
            ImGui_ImplWin32_NewFrame();
        }
    }
    ImGui::NewFrame();
    s.mesh.vertices.clear();
    s.mesh.indices.clear();
    s.frame_open = true;
    return S_OK;
}

HRESULT OverlayD3D11::end_frame() {
    if (!state_ || !state_->frame_open) {
        return E_UNEXPECTED;
    }
    auto& s = *state_;
    ImGui::SetCurrentContext(s.imgui);
    if (s.crosshair && !s.menu_interactive) {
        const float x = static_cast<float>(s.width) * 0.5f;
        const float y = static_cast<float>(s.height) * 0.5f;
        constexpr float arm = 12.0f;
        auto* diagnostics = ImGui::GetForegroundDrawList();
        diagnostics->AddLine({x - arm, y}, {x + arm, y}, IM_COL32_WHITE, 2.0f);
        diagnostics->AddLine({x, y - arm}, {x, y + arm}, IM_COL32_WHITE, 2.0f);
    }
    ImGui::Render();
    s.frame_open = false;
    ID3D11RenderTargetView* target = s.render_target.Get();
    s.context->OMSetRenderTargets(1, &target, nullptr);
    constexpr float transparent[4]{};
    s.context->ClearRenderTargetView(target, transparent);
    const HRESULT mesh_result = s.mesh.draw(s.device.Get(), s.context.Get(), target, s.width, s.height);
    if (FAILED(mesh_result)) {
        return mesh_result;
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = s.swap_chain->Present(0, 0);
    if (hr != s.last_present) {
        scarlex::diagnostic_printf("[OverlayD3D11] Present=0x%08lX viewport=%dx%d\n",
                    static_cast<unsigned long>(hr), s.width, s.height);
        s.last_present = hr;
    }
    return hr;
}

int OverlayD3D11::width() const noexcept { return state_ ? state_->width : 0; }
int OverlayD3D11::height() const noexcept { return state_ ? state_->height : 0; }

RECT OverlayD3D11::visible_client_rect() const noexcept {
    if (!state_) {
        return {};
    }
    const auto& bounds = state_->target_rect;
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return {0, 0, state_->width, state_->height};
    }
    RECT visible{};
    if (!IntersectRect(&visible, &bounds, &info.rcWork)) {
        return {};
    }
    OffsetRect(&visible, -bounds.left, -bounds.top);
    return visible;
}

RECT OverlayD3D11::desktop_client_rect() const noexcept {
    if (!state_) return {};
    const ThreadDpiScope dpi;
    const LONG x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const RECT desktop{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                       y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    const auto& bounds = state_->target_rect;
    RECT drawable{};
    if (IntersectRect(&drawable, &desktop, &bounds)) {
        OffsetRect(&drawable, -bounds.left, -bounds.top);
    }
    return drawable;
}

void OverlayD3D11::draw_mesh(const std::vector<game_entities::Vec3>& screen_vertices,
                            const std::vector<std::uint32_t>& indices, std::uint32_t color) const {
    if (!state_ || !state_->frame_open || screen_vertices.size() > max_mesh_vertices) {
        return;
    }
    const float inv_w = 2.0f / static_cast<float>(state_->width);
    const float inv_h = 2.0f / static_cast<float>(state_->height);
    std::vector<core::Vec4> clip;
    clip.reserve(screen_vertices.size());
    for (const auto& point : screen_vertices) {
        clip.push_back({point.x * inv_w - 1.0f, 1.0f - point.y * inv_h, point.z, 1.0f});
    }
    draw_mesh_clip(clip, indices, color);
}

void OverlayD3D11::draw_mesh_clip(std::span<const core::Vec4> vertices,
                                 std::span<const std::uint32_t> indices, std::uint32_t color) const {
    if (!state_ || !state_->frame_open || vertices.empty() || indices.empty() ||
        indices.size() % 3 != 0 || (color >> 24) == 0) {
        return;
    }
    auto& batch = state_->mesh;
    if (vertices.size() > max_mesh_vertices - batch.vertices.size() ||
        indices.size() > max_mesh_indices - batch.indices.size() ||
        std::any_of(vertices.begin(), vertices.end(), [](const auto& v) {
            return !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.w);
        }) ||
        std::any_of(indices.begin(), indices.end(), [&](auto i) { return i >= vertices.size(); })) {
        return;
    }
    const auto base = static_cast<std::uint32_t>(batch.vertices.size());
    for (const auto& vertex : vertices) {
        batch.vertices.push_back({vertex, color});
    }
    for (const auto index : indices) {
        batch.indices.push_back(base + index);
    }
}

void OverlayD3D11::draw_line(float x1, float y1, float x2, float y2, std::uint32_t color,
                             float thickness) const {
    if (state_ && state_->frame_open) {
        ImGui::GetBackgroundDrawList()->AddLine({x1, y1}, {x2, y2}, color, thickness);
    }
}

void OverlayD3D11::draw_circle(float x, float y, float radius, std::uint32_t color) const {
    if (state_ && state_->frame_open && std::isfinite(radius) && radius > 0.0f) {
        ImGui::GetBackgroundDrawList()->AddCircle({x, y}, radius, color);
    }
}

void OverlayD3D11::fill_circle(float x, float y, float radius, std::uint32_t color) const {
    if (state_ && state_->frame_open && std::isfinite(radius) && radius > 0.0f) {
        ImGui::GetBackgroundDrawList()->AddCircleFilled({x, y}, radius, color);
    }
}

void OverlayD3D11::draw_rect(float left, float top, float right, float bottom, std::uint32_t color,
                             float thickness) const {
    if (state_ && state_->frame_open) {
        ImGui::GetBackgroundDrawList()->AddRect({left, top}, {right, bottom}, color, 0.0f, 0, thickness);
    }
}

core::Vec2 OverlayD3D11::measure_text(std::string_view text) const {
    if (!state_ || !state_->menu_typeface || text.empty()) return {};
    auto* font=state_->menu_typeface;
    const auto size=font->CalcTextSizeA(font->FontSize,(std::numeric_limits<float>::max)(),0,
                                       text.data(),text.data()+text.size());
    return {size.x,size.y};
}

void OverlayD3D11::draw_text(float x, float y, std::string_view text, std::uint32_t color) const {
    if (!state_ || !state_->frame_open || text.empty()) {
        return;
    }
    const char* end = text.data() + text.size();
    auto* font = state_->menu_typeface;
    const float size = font->FontSize;
    const float left = x - font->CalcTextSizeA(size, (std::numeric_limits<float>::max)(), 0.0f,
                                             text.data(), end).x * 0.5f;
    auto* draw = ImGui::GetBackgroundDrawList();
    draw->AddText(font, size, {left + 1.0f, y + 1.0f}, IM_COL32(0, 0, 0, 225), text.data(), end);
    draw->AddText(font, size, {left, y}, color, text.data(), end);
}

