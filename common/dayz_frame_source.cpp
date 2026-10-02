#include "dayz_frame_source.hpp"

#include "logging.hpp"
#include "stereo_state.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

namespace
{
    constexpr char ShaderSource[] = R"(
Texture2D GameFrame : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer FrameConstants : register(b0)
{
    float2 DisplayScale;
    float2 DisplayOffset;
    float VignetteStrength;
    float VignetteRadius;
    float2 Padding;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float2 sourceUv = (input.uv - DisplayOffset) / DisplayScale;
    if (any(sourceUv < 0.0) || any(sourceUv > 1.0))
        return float4(0.0, 0.0, 0.0, 1.0);
    float4 color = GameFrame.Sample(LinearClamp, sourceUv);
    if (VignetteStrength > 0.0)
    {
        // Radial fade from the clear centre radius to the eye-image corner.
        float distance = length(input.uv - 0.5) * 2.0;
        float fade = smoothstep(VignetteRadius, 1.2, distance) * VignetteStrength;
        color.rgb *= 1.0 - fade;
    }
    return color;
}
)";

    bool CompileShader(const char* entryPoint, const char* target,
        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode) noexcept
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(ShaderSource, sizeof(ShaderSource) - 1,
            "dayz_frame_source.hlsl", nullptr, nullptr, entryPoint, target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        if (FAILED(result))
        {
            if (errors)
                logging::Error(std::string_view(static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize()));
            else
                logging::Error("D3DCompile failed for DayZFrameSource");
            return false;
        }
        return true;
    }
}

DayZFrameSource::DayZFrameSource(IDXGISwapChain* swapChain, ID3D11Device* device,
    ID3D11DeviceContext* immediateContext) noexcept
    : swapChain_(swapChain), device_(device), immediateContext_(immediateContext)
{
    pipelineReady_ = CreatePipeline();
}

bool DayZFrameSource::CreatePipeline() noexcept
{
    if (!swapChain_ || !device_ || !immediateContext_)
        return false;
    if (FAILED(device_->CreateDeferredContext(0, &deferredContext_)))
    {
        logging::Error("CreateDeferredContext failed; game-frame capture disabled");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
    if (!CompileShader("VSMain", "vs_5_0", vertexBytecode) ||
        !CompileShader("PSMain", "ps_5_0", pixelBytecode))
        return false;
    if (FAILED(device_->CreateVertexShader(vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device_->CreatePixelShader(pixelBytecode->GetBufferPointer(),
            pixelBytecode->GetBufferSize(), nullptr, &pixelShader_)))
        return false;

    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDescription, &sampler_)))
        return false;

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(FrameConstants);
    constantDescription.Usage = D3D11_USAGE_DEFAULT;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&constantDescription, nullptr, &constants_));
}

DXGI_FORMAT DayZFrameSource::ShaderResourceFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return format;
    }
}

bool DayZFrameSource::EnsureCaptureTexture(
    const D3D11_TEXTURE2D_DESC& backBufferDescription) noexcept
{
    if (captureTextures_[0] && captureTextures_[1] &&
        sourceWidth_ == backBufferDescription.Width &&
        sourceHeight_ == backBufferDescription.Height &&
        sourceFormat_ == backBufferDescription.Format)
        return true;

    for (auto& view : captureViews_)
        view.Reset();
    for (auto& texture : captureTextures_)
        texture.Reset();
    ready_ = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = backBufferDescription.Width;
    description.Height = backBufferDescription.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = backBufferDescription.Format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = ShaderResourceFormat(description.Format);
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = 1;
    for (std::size_t eye = 0; eye < captureTextures_.size(); ++eye)
    {
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &captureTextures_[eye])) ||
            FAILED(device_->CreateShaderResourceView(captureTextures_[eye].Get(),
                &viewDescription, &captureViews_[eye])))
        {
            for (auto& view : captureViews_)
                view.Reset();
            for (auto& texture : captureTextures_)
                texture.Reset();
            return false;
        }
    }

    sourceWidth_ = description.Width;
    sourceHeight_ = description.Height;
    sourceFormat_ = description.Format;
    dayz::stereo_state::SetBackBufferSize(sourceWidth_, sourceHeight_);
    std::ostringstream message;
    message << "DayZ backbuffer capture ready: " << sourceWidth_ << 'x' << sourceHeight_
            << " format=" << static_cast<int>(sourceFormat_);
    logging::Info(message.str());
    return true;
}

void DayZFrameSource::PrepareFrame(std::uint32_t sourceEye) noexcept
{
    if (!pipelineReady_)
        return;
    sourceEye = (std::min)(sourceEye, 1u);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return;
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (!EnsureCaptureTexture(description))
        return;

    if (description.SampleDesc.Count > 1)
    {
        immediateContext_->ResolveSubresource(captureTextures_[sourceEye].Get(), 0,
            backBuffer.Get(), 0,
            ShaderResourceFormat(description.Format));
    }
    else
    {
        immediateContext_->CopyResource(captureTextures_[sourceEye].Get(), backBuffer.Get());
    }
    ready_[sourceEye] = true;
}

void DayZFrameSource::RenderEye(const EyeRenderInfo& eye) noexcept
{
    if (!HasGameData() || !deferredContext_ || !eye.rtv || eye.width == 0 || eye.height == 0)
        return;

    FrameConstants values{{1.0f, 1.0f}, {0.0f, 0.0f}, 0.0f, 0.6f, {0.0f, 0.0f}};
    const dayz::stereo_state::ComfortVignette vignette = dayz::stereo_state::GetComfortVignette();
    values.vignetteStrength = vignette.strength;
    values.vignetteRadius = vignette.radius;
    const float sourceAspect = static_cast<float>(sourceWidth_) / sourceHeight_;
    const float targetAspect = static_cast<float>(eye.width) / eye.height;
    const dayz::stereo_state::Presentation presentation =
        dayz::stereo_state::GetPresentation();
    if (presentation.fitMode == dayz::stereo_state::FitMode::Contain)
    {
        if (sourceAspect > targetAspect)
            values.displayScale[1] = targetAspect / sourceAspect;
        else
            values.displayScale[0] = sourceAspect / targetAspect;
    }
    else if (presentation.fitMode == dayz::stereo_state::FitMode::Cover)
    {
        if (sourceAspect > targetAspect)
            values.displayScale[0] = sourceAspect / targetAspect;
        else
            values.displayScale[1] = targetAspect / sourceAspect;
    }
    values.displayScale[0] *= (std::max)(presentation.scaleX, 0.01f);
    values.displayScale[1] *= (std::max)(presentation.scaleY, 0.01f);
    values.displayOffset[0] = (1.0f - values.displayScale[0]) * 0.5f;
    values.displayOffset[1] = (1.0f - values.displayScale[1]) * 0.5f;
    const float imageShift = dayz::stereo_state::ImageShift();
    values.displayOffset[0] += eye.eyeIndex == 0 ? -imageShift : imageShift;

    deferredContext_->ClearState();
    deferredContext_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
    ID3D11RenderTargetView* renderTarget = eye.rtv;
    deferredContext_->OMSetRenderTargets(1, &renderTarget, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(eye.width),
        static_cast<float>(eye.height), 0.0f, 1.0f};
    deferredContext_->RSSetViewports(1, &viewport);
    deferredContext_->IASetInputLayout(nullptr);
    deferredContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferredContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    deferredContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    const std::size_t requestedEye = (std::min)(static_cast<std::size_t>(eye.eyeIndex),
        captureViews_.size() - 1);
    const std::size_t availableEye = ready_[requestedEye] ? requestedEye : 1 - requestedEye;
    ID3D11ShaderResourceView* sourceView = captureViews_[availableEye].Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ID3D11Buffer* constants = constants_.Get();
    deferredContext_->PSSetShaderResources(0, 1, &sourceView);
    deferredContext_->PSSetSamplers(0, 1, &sampler);
    deferredContext_->PSSetConstantBuffers(0, 1, &constants);
    deferredContext_->Draw(3, 0);
    sourceView = nullptr;
    deferredContext_->PSSetShaderResources(0, 1, &sourceView);

    Microsoft::WRL::ComPtr<ID3D11CommandList> commands;
    if (SUCCEEDED(deferredContext_->FinishCommandList(FALSE, &commands)))
        immediateContext_->ExecuteCommandList(commands.Get(), TRUE);
}

bool DayZFrameSource::DumpCaptures(const std::wstring& directory) noexcept
{
    if (!device_ || !immediateContext_ || !HasGameData())
        return false;
    bool any = false;
    for (std::size_t eye = 0; eye < captureTextures_.size(); ++eye)
    {
        if (!ready_[eye] || !captureTextures_[eye])
            continue;
        D3D11_TEXTURE2D_DESC description{};
        captureTextures_[eye]->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&description, nullptr, &staging)))
            continue;
        immediateContext_->CopyResource(staging.Get(), captureTextures_[eye].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(immediateContext_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            continue;
        // 24-bit BMP, bottom-up rows; the capture is BGRA or RGBA 8-bit.
        const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const std::uint32_t width = description.Width;
        const std::uint32_t height = description.Height;
        const std::uint32_t rowBytes = (width * 3 + 3) & ~3u;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * height);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(mapped.RowPitch) * y;
            std::uint8_t* target = pixels.data() + static_cast<std::size_t>(rowBytes) * (height - 1 - y);
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::uint8_t* p = source + x * 4;
                target[x * 3 + 0] = rgba ? p[2] : p[0];
                target[x * 3 + 1] = p[1];
                target[x * 3 + 2] = rgba ? p[0] : p[2];
            }
        }
        immediateContext_->Unmap(staging.Get(), 0);
        const std::wstring path = directory + L"\\dayz_openxr_eye" + std::to_wstring(eye) + L".bmp";
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            continue;
        const std::uint32_t dataSize = static_cast<std::uint32_t>(pixels.size());
        const std::uint32_t fileSize = 54 + dataSize;
        const std::uint8_t header[54] = {
            'B', 'M',
            static_cast<std::uint8_t>(fileSize), static_cast<std::uint8_t>(fileSize >> 8),
            static_cast<std::uint8_t>(fileSize >> 16), static_cast<std::uint8_t>(fileSize >> 24),
            0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
            static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8),
            static_cast<std::uint8_t>(width >> 16), static_cast<std::uint8_t>(width >> 24),
            static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(height >> 8),
            static_cast<std::uint8_t>(height >> 16), static_cast<std::uint8_t>(height >> 24),
            1, 0, 24, 0, 0, 0, 0, 0,
            static_cast<std::uint8_t>(dataSize), static_cast<std::uint8_t>(dataSize >> 8),
            static_cast<std::uint8_t>(dataSize >> 16), static_cast<std::uint8_t>(dataSize >> 24),
            0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        fwrite(header, 1, sizeof(header), file);
        fwrite(pixels.data(), 1, pixels.size(), file);
        fclose(file);
        any = true;
    }
    return any;
}

bool DayZFrameSource::CaptureIfBackBuffer(std::uint32_t sourceEye, void* resource) noexcept
{
    // DayZ never clears the backbuffer itself; the first clear of any
    // backbuffer-sized target marks the start of the next pass in its command
    // stream, at which point the backbuffer holds the finished previous pass.
    if (!pipelineReady_ || !resource)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    static_cast<IUnknown*>(resource)->QueryInterface(IID_PPV_ARGS(&texture));
    if (!texture)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return false;
    D3D11_TEXTURE2D_DESC backDescription{};
    backBuffer->GetDesc(&backDescription);
    if (description.Width != backDescription.Width || description.Height != backDescription.Height)
        return false;
    PrepareFrame(sourceEye);
    static int logged{};
    if (logged < 2)
    {
        ++logged;
        std::ostringstream message;
        message << "Mid-frame eye capture: backbuffer copied for eye " << sourceEye
                << " before clear of " << description.Width << 'x' << description.Height
                << " fmt=" << static_cast<int>(description.Format)
                << " samples=" << description.SampleDesc.Count;
        logging::Info(message.str());
    }
    return true;
}
