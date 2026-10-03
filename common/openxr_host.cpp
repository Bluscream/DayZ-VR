#include "config_number.hpp"
#include "openxr_host.hpp"

#include "xr_frame_policy.hpp"

#include "debug_frame_source.hpp"
#include "dayz_frame_source.hpp"
#include "dayz_runtime_probe.hpp"
#include "stereo_state.hpp"
#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <tuple>

namespace
{
    constexpr dayz::xr::ImageCalls kImageCalls{xrAcquireSwapchainImage,
        xrWaitSwapchainImage, xrReleaseSwapchainImage};

    bool SameLuid(const LUID& left, const LUID& right) noexcept
    {
        return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    std::wstring ConfigurationPath()
    {
        std::wstring executablePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
        if (length == 0 || length >= executablePath.size())
            return L"dayz_openxr.ini";
        executablePath.resize(length);
        const auto separator = executablePath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return L"dayz_openxr.ini";
        executablePath.resize(separator + 1);
        executablePath += L"dayz_openxr.ini";
        return executablePath;
    }

    bool IsOpenXrEnabled() noexcept
    {
        wchar_t value[16]{};
        const std::wstring configPath = ConfigurationPath();
        GetPrivateProfileStringW(L"openxr", L"enabled", L"true", value,
            static_cast<DWORD>(std::size(value)), configPath.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    bool ReadBoolean(const wchar_t* section, const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", value,
            static_cast<DWORD>(std::size(value)), ConfigurationPath().c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback) noexcept
    {
        wchar_t fallbackText[32]{};
        swprintf_s(fallbackText, L"%.3f", fallback);
        wchar_t value[32]{};
        GetPrivateProfileStringW(section, key, fallbackText, value,
            static_cast<DWORD>(std::size(value)), ConfigurationPath().c_str());
        return dayz::config_number::ParseFloat(value, fallback);
    }

    std::uint32_t ReadUnsigned(const wchar_t* section, const wchar_t* key,
        std::uint32_t fallback) noexcept
    {
        return static_cast<std::uint32_t>((std::max)(1u, GetPrivateProfileIntW(section,
            key, static_cast<int>(fallback), ConfigurationPath().c_str())));
    }

    XrQuaternionf YawOnly(const XrQuaternionf& orientation) noexcept
    {
        const float yaw = std::atan2(2.0f * (orientation.w * orientation.y +
            orientation.x * orientation.z), 1.0f - 2.0f *
            (orientation.x * orientation.x + orientation.y * orientation.y));
        const float half = yaw * 0.5f;
        return {0.0f, std::sin(half), 0.0f, std::cos(half)};
    }
}

OpenXrHost& OpenXrHost::Instance() noexcept
{
    static OpenXrHost host;
    return host;
}

bool OpenXrHost::DumpEyeCaptures() noexcept
{
    if (!initialized_.load())
        return false;
    eyeDumpRequested_.store(true, std::memory_order_release);
    return true;
}

bool OpenXrHost::Check(XrResult result, const char* operation) const noexcept
{
    if (XR_SUCCEEDED(result))
        return true;
    logging::XrError(operation, result);
    return false;
}

bool OpenXrHost::CheckImageUpdate(const dayz::xr::ImageUpdate& update, const char* operation) noexcept
{
    if (update.Ready())
        return true;
    if (update.failure != dayz::xr::ImageFailure::Render)
        logging::XrError(operation, update.result);
    if (update.MustStop())
        shouldExit_ = true;
    return false;
}

bool OpenXrHost::CreateInstanceAndSystem()
{
    logging::Info("Enumerating OpenXR instance extensions");
    std::uint32_t extensionCount{};
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
        "xrEnumerateInstanceExtensionProperties(count)"))
        return false;

    std::vector<XrExtensionProperties> extensions(extensionCount);
    for (auto& extension : extensions)
        extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount,
        extensions.data()), "xrEnumerateInstanceExtensionProperties(list)"))
        return false;

    const bool hasD3D11 = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension)
    {
        return std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!hasD3D11)
    {
        logging::Error("OpenXR runtime does not expose XR_KHR_D3D11_enable");
        return false;
    }

    const std::vector<const char*> enabledExtensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    logging::Info("Creating OpenXR instance");
    XrInstanceCreateInfo createInfo(MakeXr<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO));
    strcpy_s(createInfo.applicationInfo.applicationName, "DayZ OpenXR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "DayZ VR Mod");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();
    if (!Check(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance"))
        return false;

    logging::Info("Requesting HMD system");
    XrSystemGetInfo systemInfo(MakeXr<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO));
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(instance_, &systemInfo, &systemId_), "xrGetSystem"))
        return false;

    logging::Info("Resolving D3D11 graphics requirements entry point");
    PFN_xrVoidFunction function{};
    if (!Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR", &function),
        "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        return false;
    getD3D11Requirements_ = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(function);
    return getD3D11Requirements_ != nullptr;
}

bool OpenXrHost::ValidateDevice(ID3D11Device* device)
{
    XrGraphicsRequirementsD3D11KHR requirements(MakeXr<XrGraphicsRequirementsD3D11KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR));
    if (!Check(getD3D11Requirements_(instance_, systemId_, &requirements),
        "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
    {
        logging::Error("Cannot resolve the DXGI adapter for the D3D11 device");
        return false;
    }
    if (!SameLuid(description.AdapterLuid, requirements.adapterLuid))
    {
        logging::Error("D3D11 adapter LUID does not match the OpenXR runtime requirement");
        return false;
    }
    return true;
}

bool OpenXrHost::CreateCompatibleDevice()
{
    logging::Info("Reading OpenXR D3D11 graphics requirements");
    XrGraphicsRequirementsD3D11KHR requirements(MakeXr<XrGraphicsRequirementsD3D11KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR));
    if (!Check(getD3D11Requirements_(instance_, systemId_, &requirements),
        "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    logging::Info("Selecting the OpenXR-required DXGI adapter");
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0; ; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            SameLuid(description.AdapterLuid, requirements.adapterLuid))
        {
            selected = adapter;
            break;
        }
    }
    if (!selected)
    {
        logging::Error("OpenXR requested adapter was not found");
        return false;
    }

    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    D3D_FEATURE_LEVEL created{};
    logging::Info("Creating standalone D3D11 device");
    const HRESULT result = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, static_cast<UINT>(std::size(requested)),
        D3D11_SDK_VERSION, &device_, &created, &context_);
    if (FAILED(result) || created < requirements.minFeatureLevel)
    {
        logging::Error("Could not create a compatible D3D11 device");
        return false;
    }
    return true;
}

bool OpenXrHost::CreateSession()
{
    XrGraphicsBindingD3D11KHR binding(MakeXr<XrGraphicsBindingD3D11KHR>(XR_TYPE_GRAPHICS_BINDING_D3D11_KHR));
    binding.device = device_.Get();
    XrSessionCreateInfo sessionInfo(MakeXr<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO));
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    return Check(xrCreateSession(instance_, &sessionInfo, &session_), "xrCreateSession");
}

bool OpenXrHost::CreateSpaces()
{
    XrReferenceSpaceCreateInfo info(MakeXr<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO));
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(session_, &info, &localSpace_), "xrCreateReferenceSpace(local)"))
        return false;
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return Check(xrCreateReferenceSpace(session_, &info, &viewSpace_), "xrCreateReferenceSpace(view)");
}

bool OpenXrHost::CreateSwapchains()
{
    logging::Info("Enumerating stereo view configuration");
    std::uint32_t viewCount{};
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
        "xrEnumerateViewConfigurationViews(count)") || viewCount < 2)
        return false;

    std::vector<XrViewConfigurationView> configs(viewCount);
    for (auto& config : configs)
        config.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configs.data()),
        "xrEnumerateViewConfigurationViews(list)"))
        return false;

    std::uint32_t formatCount{};
    logging::Info("Enumerating OpenXR swapchain formats");
    if (!Check(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
        "xrEnumerateSwapchainFormats(count)"))
        return false;
    std::vector<std::int64_t> formats(formatCount);
    if (!Check(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()),
        "xrEnumerateSwapchainFormats(list)"))
        return false;
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    DXGI_FORMAT selected = DXGI_FORMAT_UNKNOWN;
    for (const auto candidate : preferred)
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) != formats.end())
        {
            selected = candidate;
            break;
        }
    if (selected == DXGI_FORMAT_UNKNOWN)
    {
        logging::Error("OpenXR runtime has no supported RGBA swapchain format");
        return false;
    }

    for (std::size_t eye = 0; eye < eyeSwapchains_.size(); ++eye)
    {
        auto& swapchain = eyeSwapchains_[eye];
        logging::Info(eye == 0 ? "Creating left-eye swapchain" : "Creating right-eye swapchain");
        swapchain.width = configs[eye].recommendedImageRectWidth;
        swapchain.height = configs[eye].recommendedImageRectHeight;
        XrSwapchainCreateInfo info(MakeXr<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO));
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = selected;
        info.sampleCount = 1;
        info.width = swapchain.width;
        info.height = swapchain.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(xrCreateSwapchain(session_, &info, &swapchain.handle), "xrCreateSwapchain"))
            return false;

        logging::Info(eye == 0 ? "Enumerating left-eye images" : "Enumerating right-eye images");
        std::uint32_t imageCount{};
        if (!Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr),
            "xrEnumerateSwapchainImages(count)"))
            return false;
        swapchain.images.resize(imageCount);
        for (auto& image : swapchain.images)
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        if (!Check(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
            "xrEnumerateSwapchainImages(list)"))
            return false;
        {
            std::ostringstream imageInfo;
            imageInfo << (eye == 0 ? "Left" : "Right") << " eye image count=" << imageCount;
            logging::Info(imageInfo.str());
        }
        swapchain.rtvs.resize(imageCount);
        logging::Info(eye == 0 ? "Creating left-eye render targets" : "Creating right-eye render targets");
        D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
        rtvDescription.Format = selected;
        rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDescription.Texture2D.MipSlice = 0;
        for (std::size_t index = 0; index < imageCount; ++index)
        {
            std::ostringstream imageInfo;
            imageInfo << "Creating RTV eye=" << eye << " image=" << index
                      << " texture=" << swapchain.images[index].texture;
            logging::Info(imageInfo.str());
            const HRESULT rtvResult = device_->CreateRenderTargetView(
                swapchain.images[index].texture, &rtvDescription, &swapchain.rtvs[index]);
            if (FAILED(rtvResult))
            {
                std::ostringstream error;
                error << "CreateRenderTargetView failed HRESULT=0x" << std::hex << rtvResult;
                logging::Error(error.str());
                return false;
            }
        }
    }
    if (guiQuadEnabled_ && !CreateGuiSwapchain(formats))
    {
        logging::Error("GUI quad swapchain is unavailable; projection rendering will continue");
        guiQuadEnabled_ = false;
    }
    logging::Info("Stereo swapchains are ready");
    return true;
}

bool OpenXrHost::CreateGuiSwapchain(const std::vector<std::int64_t>& formats)
{
    const DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    DXGI_FORMAT selected = DXGI_FORMAT_UNKNOWN;
    for (const auto candidate : preferred)
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) !=
            formats.end())
        {
            selected = candidate;
            break;
        }
    if (selected == DXGI_FORMAT_UNKNOWN)
        return false;

    guiSwapchain_.width = (std::clamp)(ReadUnsigned(L"gui", L"quad_pixel_width", 1920),
        512u, 4096u);
    guiSwapchain_.height = (std::clamp)(ReadUnsigned(L"gui", L"quad_pixel_height", 1400),
        512u, 4096u);
    XrSwapchainCreateInfo info(MakeXr<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO));
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = selected;
    info.sampleCount = 1;
    info.width = guiSwapchain_.width;
    info.height = guiSwapchain_.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &info, &guiSwapchain_.handle),
        "xrCreateSwapchain(gui)"))
        return false;

    std::uint32_t imageCount{};
    if (!Check(xrEnumerateSwapchainImages(guiSwapchain_.handle, 0, &imageCount, nullptr),
        "xrEnumerateSwapchainImages(gui count)"))
        return false;
    guiSwapchain_.images.resize(imageCount);
    for (auto& image : guiSwapchain_.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Check(xrEnumerateSwapchainImages(guiSwapchain_.handle, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(guiSwapchain_.images.data())),
        "xrEnumerateSwapchainImages(gui list)"))
        return false;

    guiSwapchain_.rtvs.resize(imageCount);
    D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
    rtvDescription.Format = selected;
    rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (std::size_t index = 0; index < imageCount; ++index)
        if (FAILED(device_->CreateRenderTargetView(guiSwapchain_.images[index].texture,
            &rtvDescription, &guiSwapchain_.rtvs[index])))
            return false;

    std::ostringstream message;
    message << "GUI quad swapchain ready: " << guiSwapchain_.width << 'x'
        << guiSwapchain_.height << " images=" << imageCount;
    logging::Info(message.str());
    return true;
}

bool OpenXrHost::FinishInitialization(ID3D11Device* device)
{
    device_ = device;
    device_->GetImmediateContext(&context_);
    guiQuadEnabled_ = ReadBoolean(L"gui", L"quad_enabled", true);
    guiQuadWidthMeters_ = (std::clamp)(ReadFloat(L"gui", L"quad_width_meters", 1.4f),
        0.4f, 4.0f);
    guiQuadDistance_ = (std::clamp)(ReadFloat(L"gui", L"quad_distance", 1.25f),
        0.4f, 5.0f);
    guiQuadVerticalOffset_ = (std::clamp)(ReadFloat(L"gui", L"quad_vertical_offset", -0.1f),
        -2.0f, 2.0f);
    logging::Info("Creating OpenXR session");
    if (!CreateSession())
        return false;
    logging::Info("Creating OpenXR reference spaces");
    if (!CreateSpaces())
        return false;
    logging::Info("Creating OpenXR eye swapchains");
    if (!CreateSwapchains())
        return false;
    debugFrameSource_ = std::make_unique<DebugFrameSource>(context_.Get());
    if (gameSwapChain_)
        gameFrameSource_ = std::make_unique<DayZFrameSource>(gameSwapChain_.Get(), device_.Get(),
            context_.Get());
    dayz::stereo_state::SetEyeCaptureCallback([](unsigned eye, void* resource) {
        auto& source = OpenXrHost::Instance().gameFrameSource_;
        return source && source->CaptureIfBackBuffer(eye, resource);
    });
    initialized_ = true;
    logging::Info("OpenXR host initialized");
    return true;
}

void OpenXrHost::AttachGameSwapChain(IDXGISwapChain* swapChain) noexcept
{
    if (!swapChain)
        return;
    std::scoped_lock lock(mutex_);
    if (!gameSwapChain_)
        gameSwapChain_ = swapChain;
}

bool OpenXrHost::InitializeWithDevice(ID3D11Device* device) noexcept
{
    std::scoped_lock lock(mutex_);
    if (initialized_)
        return true;
    logging::Initialize();
    if (!IsOpenXrEnabled())
    {
        logging::Info("OpenXR is disabled by dayz_openxr.ini");
        return false;
    }
    if (!device || !CreateInstanceAndSystem() || !ValidateDevice(device) || !FinishInitialization(device))
    {
        logging::Error("OpenXR initialization skipped; the application will continue without VR");
        return false;
    }
    return true;
}

bool OpenXrHost::InitializeStandalone() noexcept
{
    std::scoped_lock lock(mutex_);
    if (initialized_)
        return true;
    logging::Initialize(L"xr_probe.log");
    logging::Info("xr_probe initialization started");
    if (!CreateInstanceAndSystem() || !CreateCompatibleDevice() || !FinishInitialization(device_.Get()))
        return false;
    return true;
}

void OpenXrHost::PollEvents()
{
    XrEventDataBuffer event(MakeXr<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER));
    while (xrPollEvent(instance_, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            sessionState_ = changed->state;
            std::ostringstream message;
            message << "XrSessionState = " << static_cast<int>(sessionState_.load());
            logging::Info(message.str());
            if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_)
            {
                XrSessionBeginInfo begin(MakeXr<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO));
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                sessionRunning_ = Check(xrBeginSession(session_, &begin), "xrBeginSession");
            }
            else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_)
            {
                Check(xrEndSession(session_), "xrEndSession");
                sessionRunning_ = false;
            }
            else if (sessionState_ == XR_SESSION_STATE_EXITING ||
                sessionState_ == XR_SESSION_STATE_LOSS_PENDING)
                shouldExit_ = true;
        }
        event = (MakeXr<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER));
    }
}

void OpenXrHost::AnchorGuiQuad(const XrPosef& headPose) noexcept
{
    const XrQuaternionf yaw = YawOnly(headPose.orientation);
    const XrVector3f forward{
        -2.0f * (yaw.x * yaw.z + yaw.w * yaw.y),
        -2.0f * (yaw.y * yaw.z - yaw.w * yaw.x),
        -(1.0f - 2.0f * (yaw.x * yaw.x + yaw.y * yaw.y))};
    guiQuadPose_.orientation = yaw;
    guiQuadPose_.position = {
        headPose.position.x + forward.x * guiQuadDistance_,
        headPose.position.y + forward.y * guiQuadDistance_ + guiQuadVerticalOffset_,
        headPose.position.z + forward.z * guiQuadDistance_};
    guiQuadAnchored_ = true;
    logging::Info("GUI quad anchored in LOCAL space");
}

void OpenXrHost::RenderFrame()
{
    XrFrameWaitInfo waitInfo(MakeXr<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO));
    XrFrameState frameState(MakeXr<XrFrameState>(XR_TYPE_FRAME_STATE));
    const auto frameStart = std::chrono::steady_clock::now();
    if (!Check(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame"))
    {
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
        return;
    }
    timing_.waitFrame += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    XrFrameBeginInfo beginInfo(MakeXr<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO));
    if (!Check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame"))
    {
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
        return;
    }

    std::array<XrCompositionLayerProjectionView, 2> projectionViews{{
        (MakeXr<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)),
        (MakeXr<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW))}};
    std::uint32_t viewCount{};
    XrViewState viewState(MakeXr<XrViewState>(XR_TYPE_VIEW_STATE));
    XrViewLocateInfo locate(MakeXr<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO));
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frameState.predictedDisplayTime;
    locate.space = localSpace_;
    const bool located = Check(xrLocateViews(session_, &locate, &viewState,
        static_cast<std::uint32_t>(views_.size()), &viewCount, views_.data()), "xrLocateViews") &&
        dayz::xr::ValidStereoViews(viewCount, viewState.viewStateFlags);

    const bool guiVisible = gameSwapChain_ && guiQuadEnabled_ && guiSwapchain_.handle != XR_NULL_HANDLE &&
        dayz::runtime_probe::IsGuiQuadVisible();
    if (!located || !frameState.shouldRender)
    {
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
    }
    bool projectionReady = false;
    if (frameState.shouldRender && located)
    {
        dayz::stereo_state::UpdateEyePositions(
            views_[0].pose.position.x, views_[0].pose.position.y, views_[0].pose.position.z,
            views_[1].pose.position.x, views_[1].pose.position.y, views_[1].pose.position.z);
        dayz::stereo_state::UpdateHmdPosition(
            (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f,
            (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f,
            (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f);
        const auto& orientation = views_[0].pose.orientation;
        dayz::stereo_state::UpdateHmdOrientation(orientation.x, orientation.y,
            orientation.z, orientation.w);
        {
            dayz::stereo_state::ViewPose recorded[2]{};
            for (std::size_t eye = 0; eye < 2; ++eye)
            {
                const XrView& view = views_[eye];
                recorded[eye] = {view.pose.orientation.x, view.pose.orientation.y,
                    view.pose.orientation.z, view.pose.orientation.w,
                    view.pose.position.x, view.pose.position.y, view.pose.position.z,
                    view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown};
            }
            dayz::stereo_state::UpdateHmdViews(recorded[0], recorded[1]);
        }
        if (guiVisible && (!guiQuadWasVisible_ || !guiQuadAnchored_))
            AnchorGuiQuad(views_[0].pose);
        if (gameFrameSource_)
        {
            // This runs inside DayZ's Present: the backbuffer holds the frame filed
            // frame_lag game frames ago. Capture it under that frame's eye and keep the
            // view poses it was rendered with for the layer below.
            dayz::stereo_state::FrameRecord presented{};
            const bool haveRecord = dayz::stereo_state::PresentedRecord(dayz::stereo_state::FrameLag(), presented);
            const unsigned eye = haveRecord ? presented.eye : dayz::stereo_state::RenderedEye();
            gameFrameSource_->PrepareFrame(eye);
            capturedViews_[eye & 1u] = {presented, haveRecord && presented.valid};
        }
        if (gameFrameSource_ && eyeDumpRequested_.exchange(false, std::memory_order_acq_rel))
        {
            std::wstring directory = ConfigurationPath();
            const auto separator = directory.find_last_of(L"\\/");
            directory = separator == std::wstring::npos ? L"." : directory.substr(0, separator);
            logging::Info(gameFrameSource_->DumpCaptures(directory) ?
                "Eye captures written beside DayZ_x64.exe" : "Eye capture dump failed");
        }
        static std::uint64_t logCounter{};
        static auto lastLog = std::chrono::steady_clock::now();
        if (++logCounter % 120 == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - lastLog).count();
            lastLog = now;
            const auto& q = views_[0].pose.orientation;
            const float sinPitch = std::clamp(2.0f * (q.w * q.x - q.z * q.y), -1.0f, 1.0f);
            const float pitch = std::asin(sinPitch);
            const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
            std::ostringstream pose;
            lastFps_ = seconds > 0.0 ? 120.0 / seconds : 0.0;
            pose << "pose q=(" << q.x << ',' << q.y << ',' << q.z << ',' << q.w
                 << ") ypr=(" << yaw << ',' << pitch << ',' << roll << ") fps="
                 << lastFps_ << " state=" << static_cast<int>(sessionState_.load());
            if (timing_.frames > 0)
            {
                // Where the frame time goes inside the OpenXR calls (ms per frame, averaged
                // over the log interval): a wait-bound game shows up here, a CPU/GPU-bound
                // one in the remainder (frame interval minus xr_total).
                const double n = static_cast<double>(timing_.frames);
                pose << " xr_ms wait_frame=" << timing_.waitFrame / n << " image_work=" << timing_.imageWork / n
                     << " end_frame=" << timing_.endFrame / n << " total=" << timing_.total / n
                     << " interval=" << (seconds * 1000.0 / 120.0);
                timing_ = {};
            }
            logging::Info(pose.str());
        }
        {
            std::scoped_lock debugLock(debugMutex_);
            debugSnapshot_.initialized = initialized_;
            debugSnapshot_.sessionRunning = sessionRunning_;
            debugSnapshot_.sessionState = static_cast<int>(sessionState_.load());
            debugSnapshot_.fps = lastFps_;
            debugSnapshot_.hmdValid = true;
            debugSnapshot_.hmdPose = views_[0].pose;
        }
        projectionReady = true;
        for (std::size_t eye = 0; eye < eyeSwapchains_.size(); ++eye)
        {
            auto& swapchain = eyeSwapchains_[eye];
            const auto imageStart = std::chrono::steady_clock::now();
            const auto update = dayz::xr::UpdateImage(swapchain.handle, kImageCalls,
                [&](std::uint32_t imageIndex) {
                    EyeRenderInfo renderInfo{};
                    renderInfo.eyeIndex = static_cast<std::uint32_t>(eye);
                    renderInfo.pose = views_[eye].pose;
                    renderInfo.fov = views_[eye].fov;
                    renderInfo.target = swapchain.images[imageIndex].texture;
                    renderInfo.rtv = swapchain.rtvs[imageIndex].Get();
                    renderInfo.width = swapchain.width;
                    renderInfo.height = swapchain.height;
                    if (gameFrameSource_ && gameFrameSource_->HasGameData())
                        gameFrameSource_->RenderEye(renderInfo);
                    else
                        debugFrameSource_->RenderEye(renderInfo);
                    return true;
                });
            timing_.imageWork += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - imageStart).count();
            if (!CheckImageUpdate(update, "eye image update"))
            {
                projectionReady = false;
                break;
            }
            auto& layerView = projectionViews[eye];
            layerView.pose = views_[eye].pose;
            layerView.fov = views_[eye].fov;
            const CapturedView& captured = capturedViews_[eye];
            if (dayz::stereo_state::SubmitRenderedPose() && captured.valid &&
                gameFrameSource_ && gameFrameSource_->HasGameData())
            {
                // The compositor reprojects from the pose the image was rendered with
                // to the display pose; submitting the newest pose instead would glue
                // the (older) image to the head.
                const dayz::stereo_state::ViewPose& view = captured.record.views[eye];
                layerView.pose.orientation = {view.qx, view.qy, view.qz, view.qw};
                layerView.pose.position = {view.px, view.py, view.pz};
                layerView.fov = {view.fovLeft, view.fovRight, view.fovUp, view.fovDown};
            }
            layerView.subImage.swapchain = swapchain.handle;
            layerView.subImage.imageRect.extent = {
                static_cast<std::int32_t>(swapchain.width), static_cast<std::int32_t>(swapchain.height)};
            layerView.subImage.imageArrayIndex = 0;
        }

        guiQuadHasImage_ = false;
        if (guiVisible && !shouldExit_)
        {
            const auto update = dayz::xr::UpdateImage(guiSwapchain_.handle, kImageCalls,
                [&](std::uint32_t imageIndex) {
                    return dayz::runtime_probe::RenderGuiQuad(guiSwapchain_.rtvs[imageIndex].Get(),
                        guiSwapchain_.width, guiSwapchain_.height);
                });
            guiQuadHasImage_ = CheckImageUpdate(update, "GUI image update");
        }
    }
    guiQuadWasVisible_ = guiVisible;
    if (!guiVisible)
    {
        guiQuadAnchored_ = false;
        guiQuadHasImage_ = false;
    }

    XrCompositionLayerProjection layer(MakeXr<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION));
    layer.space = localSpace_;
    layer.viewCount = static_cast<std::uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    XrCompositionLayerQuad guiLayer(MakeXr<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD));
    guiLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    guiLayer.space = localSpace_;
    guiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    guiLayer.subImage.swapchain = guiSwapchain_.handle;
    guiLayer.subImage.imageRect.extent = {static_cast<std::int32_t>(guiSwapchain_.width),
        static_cast<std::int32_t>(guiSwapchain_.height)};
    guiLayer.pose = guiQuadPose_;
    guiLayer.size.width = guiQuadWidthMeters_;
    guiLayer.size.height = guiQuadWidthMeters_ * static_cast<float>(guiSwapchain_.height) /
        static_cast<float>((std::max)(1u, guiSwapchain_.width));
    std::array<const XrCompositionLayerBaseHeader*, 16> layers{};
    std::uint32_t layerCount = projectionReady && !shouldExit_ ? 1u : 0u;
    if (layerCount)
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    if (layerCount && guiVisible && guiQuadHasImage_)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&guiLayer);
    XrFrameEndInfo endInfo(MakeXr<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO));
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // An unresolved image ownership failure poisons the session; complete the
    // begun frame with no layers and let Tick stop issuing frames afterwards.
    if (shouldExit_)
        layerCount = 0;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount ? layers.data() : nullptr;
    const auto endStart = std::chrono::steady_clock::now();
    Check(xrEndFrame(session_, &endInfo), "xrEndFrame");
    timing_.endFrame += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - endStart).count();
    timing_.total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    ++timing_.frames;
}

OpenXrHost::DebugSnapshot OpenXrHost::GetDebugSnapshot() const noexcept
{
    std::scoped_lock debugLock(debugMutex_);
    DebugSnapshot snapshot = debugSnapshot_;
    // Lifecycle fields are atomic; pose tuples remain protected by debugMutex_.
    snapshot.initialized = initialized_;
    snapshot.sessionRunning = sessionRunning_;
    snapshot.sessionState = static_cast<int>(sessionState_.load());
    return snapshot;
}

void OpenXrHost::Tick() noexcept
{
    std::scoped_lock lock(mutex_);
    if (!initialized_)
        return;
    PollEvents();
    if (sessionRunning_ && !shouldExit_)
        RenderFrame();
    if (shouldExit_ || !sessionRunning_)
    {
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
    }
}

void OpenXrHost::Shutdown() noexcept
{
    std::scoped_lock lock(mutex_);
    dayz::stereo_state::SetEyeCaptureCallback(nullptr);
    gameFrameSource_.reset();
    debugFrameSource_.reset();
    gameSwapChain_.Reset();
    for (auto& swapchain : eyeSwapchains_)
    {
        swapchain.rtvs.clear();
        swapchain.images.clear();
        if (swapchain.handle != XR_NULL_HANDLE)
            xrDestroySwapchain(swapchain.handle);
        swapchain.handle = XR_NULL_HANDLE;
    }
    guiSwapchain_.rtvs.clear();
    guiSwapchain_.images.clear();
    if (guiSwapchain_.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(guiSwapchain_.handle);
    guiSwapchain_.handle = XR_NULL_HANDLE;
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    viewSpace_ = localSpace_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    context_.Reset();
    device_.Reset();
    initialized_ = sessionRunning_ = false;
}
