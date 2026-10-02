#include "config_number.hpp"
#include "openxr_host.hpp"

#include "ammo_display.hpp"
#include "melee_swing.hpp"
#include "physical_stance.hpp"
#include "vehicle_steering.hpp"
#include "xr_frame_policy.hpp"
#include "comfort.hpp"
#include "script_bridge.hpp"

#include "debug_frame_source.hpp"
#include "dayz_frame_source.hpp"
#include "dayz_runtime_probe.hpp"
#include "stereo_state.hpp"
#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

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

    XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) noexcept
    {
        const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
        const XrQuaternionf inverse{-q.x, -q.y, -q.z, q.w};
        const XrQuaternionf result = Multiply(Multiply(q, p), inverse);
        return {result.x, result.y, result.z};
    }

    XrQuaternionf OrientAlongX(const XrVector3f& direction) noexcept
    {
        const float length = std::sqrt(direction.x * direction.x +
            direction.y * direction.y + direction.z * direction.z);
        if (length < 0.00001f)
            return {0.0f, 0.0f, 0.0f, 1.0f};
        const XrVector3f unit{direction.x / length, direction.y / length,
            direction.z / length};
        if (unit.x < -0.9999f)
            return {0.0f, 1.0f, 0.0f, 0.0f};
        XrQuaternionf result{0.0f, -unit.z, unit.y, 1.0f + unit.x};
        const float qLength = std::sqrt(result.x * result.x + result.y * result.y +
            result.z * result.z + result.w * result.w);
        return {result.x / qLength, result.y / qLength, result.z / qLength,
            result.w / qLength};
    }

    void SendKey(WORD key, bool down) noexcept
    {
        const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(scan);
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &input, sizeof(input));

        // DayZ uses both polled gameplay input and its window-message input path.
        // SendInput covers the former; explicitly queueing the transition covers GUI
        // actions such as Escape and Inventory which can otherwise ignore injection.
        const HWND window = dayz::runtime_probe::RealForegroundWindow();
        DWORD processId{};
        if (window && GetWindowThreadProcessId(window, &processId) &&
            processId == GetCurrentProcessId())
        {
            LPARAM parameters = 1 | (static_cast<LPARAM>(scan & 0xFFu) << 16);
            if (!down)
                parameters |= (static_cast<LPARAM>(1) << 30) |
                    (static_cast<LPARAM>(1) << 31);
            PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, key, parameters);
        }
    }

    void SendMouseButton(bool right, bool down) noexcept
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = right ?
            (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP) :
            (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        SendInput(1, &input, sizeof(input));
    }

    void SendMouseTurn(LONG x) noexcept
    {
        if (!x)
            return;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
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

    // Controller interaction profiles gated behind extensions; suggesting their
    // bindings without the extension fails with XR_ERROR_PATH_UNSUPPORTED (-22).
    // Named as strings because the vendored SDK headers predate most of them.
    static constexpr const char* kOptionalExtensions[]{
        "XR_FB_touch_controller_pro", "XR_META_touch_controller_plus",
        "XR_BD_controller_interaction", "XR_HTC_vive_cosmos_controller_interaction",
        "XR_EXT_hp_mixed_reality_controller"};
    std::vector<const char*> enabledExtensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    enabledOptionalExtensions_.clear();
    for (const char* optional : kOptionalExtensions)
        if (std::any_of(extensions.begin(), extensions.end(), [optional](const auto& extension) {
                return std::strcmp(extension.extensionName, optional) == 0; }))
        {
            enabledOptionalExtensions_.emplace_back(optional);
            enabledExtensions.push_back(optional);
            logging::Info(std::string("Enabling optional OpenXR extension ") + optional);
        }
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

bool OpenXrHost::CreateControllerActions()
{
    if (!controllerInputEnabled_)
        return true;
    XrActionSetCreateInfo setInfo(MakeXr<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO));
    strcpy_s(setInfo.actionSetName, "dayz_vr_controls");
    strcpy_s(setInfo.localizedActionSetName, "DayZ VR Controls");
    setInfo.priority = 0;
    if (!Check(xrCreateActionSet(instance_, &setInfo, &actionSet_), "xrCreateActionSet"))
        return false;
    if (!Check(xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]),
            "xrStringToPath(left hand)") ||
        !Check(xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]),
            "xrStringToPath(right hand)"))
        return false;

    const auto createAction = [&](const char* name, const char* localized,
        XrActionType type, XrAction& action) {
        XrActionCreateInfo info(MakeXr<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO));
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, localized);
        info.actionType = type;
        info.countSubactionPaths = static_cast<std::uint32_t>(handPaths_.size());
        info.subactionPaths = handPaths_.data();
        return Check(xrCreateAction(actionSet_, &info, &action), "xrCreateAction");
    };
    if (!createAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT,
            gripPoseAction_) ||
        !createAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT,
            aimPoseAction_) ||
        !createAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT,
            triggerAction_) ||
        !createAction("grab", "Grab", XR_ACTION_TYPE_FLOAT_INPUT,
            grabAction_) ||
        !createAction("x_button", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
            xButtonAction_) ||
        !createAction("y_button", "Inventory", XR_ACTION_TYPE_BOOLEAN_INPUT,
            yButtonAction_) ||
        !createAction("a_button", "Use", XR_ACTION_TYPE_BOOLEAN_INPUT,
            aButtonAction_) ||
        !createAction("b_button", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT,
            bButtonAction_) ||
        !createAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT,
            thumbstickAction_) ||
        !createAction("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT,
            thumbstickClickAction_))
        return false;

    const auto path = [&](const char* text) {
        XrPath result{XR_NULL_PATH};
        xrStringToPath(instance_, text, &result);
        return result;
    };
    const auto suggest = [&](const char* profile,
        const std::vector<XrActionSuggestedBinding>& bindings) {
        XrInteractionProfileSuggestedBinding info(MakeXr<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING));
        info.interactionProfile = path(profile);
        info.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        info.suggestedBindings = bindings.data();
        const XrResult result = xrSuggestInteractionProfileBindings(instance_, &info);
        if (XR_FAILED(result))
            logging::XrError((std::string("xrSuggestInteractionProfileBindings ") + profile).c_str(), result);
    };
    suggest("/interaction_profiles/oculus/touch_controller", {
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
        {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
        {triggerAction_, path("/user/hand/right/input/trigger/value")},
        {triggerAction_, path("/user/hand/left/input/trigger/value")},
        {grabAction_, path("/user/hand/left/input/squeeze/value")},
        {grabAction_, path("/user/hand/right/input/squeeze/value")},
        {xButtonAction_, path("/user/hand/left/input/x/click")},
        {yButtonAction_, path("/user/hand/left/input/y/click")},
        {aButtonAction_, path("/user/hand/right/input/a/click")},
        {bButtonAction_, path("/user/hand/right/input/b/click")},
        {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
        {thumbstickClickAction_, path("/user/hand/left/input/thumbstick/click")},
        {thumbstickClickAction_, path("/user/hand/right/input/thumbstick/click")}});
    suggest("/interaction_profiles/valve/index_controller", {
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
        {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
        {triggerAction_, path("/user/hand/right/input/trigger/value")},
        {triggerAction_, path("/user/hand/left/input/trigger/value")},
        {grabAction_, path("/user/hand/left/input/squeeze/value")},
        {grabAction_, path("/user/hand/right/input/squeeze/value")},
        {xButtonAction_, path("/user/hand/left/input/a/click")},
        {yButtonAction_, path("/user/hand/left/input/b/click")},
        {aButtonAction_, path("/user/hand/right/input/a/click")},
        {bButtonAction_, path("/user/hand/right/input/b/click")},
        {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
        {thumbstickClickAction_, path("/user/hand/left/input/thumbstick/click")},
        {thumbstickClickAction_, path("/user/hand/right/input/thumbstick/click")}});
    // squeeze: "value" on controllers with an analogue grip, "click" where the
    // profile only defines a digital squeeze (Vive Cosmos).
    const auto suggestXyController = [&](const char* profile, const char* squeeze = "value") {
        const std::string leftSqueeze = std::string("/user/hand/left/input/squeeze/") + squeeze;
        const std::string rightSqueeze = std::string("/user/hand/right/input/squeeze/") + squeeze;
        suggest(profile, {
            {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
            {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
            {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
            {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
            {triggerAction_, path("/user/hand/right/input/trigger/value")},
            {triggerAction_, path("/user/hand/left/input/trigger/value")},
            {grabAction_, path(leftSqueeze.c_str())},
            {grabAction_, path(rightSqueeze.c_str())},
            {xButtonAction_, path("/user/hand/left/input/x/click")},
            {yButtonAction_, path("/user/hand/left/input/y/click")},
            {aButtonAction_, path("/user/hand/right/input/a/click")},
            {bButtonAction_, path("/user/hand/right/input/b/click")},
            {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
            {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
            {thumbstickClickAction_, path("/user/hand/left/input/thumbstick/click")},
            {thumbstickClickAction_, path("/user/hand/right/input/thumbstick/click")}});
    };
    // Windows Mixed Reality sticks: no X/Y/A/B, so menu and trackpad clicks
    // stand in (left menu -> Menu, left trackpad -> Inventory, right trackpad ->
    // Use, right menu -> Jump) and the grip is a click rather than a value.
    suggest("/interaction_profiles/microsoft/motion_controller", {
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
        {aimPoseAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseAction_, path("/user/hand/right/input/aim/pose")},
        {triggerAction_, path("/user/hand/right/input/trigger/value")},
        {triggerAction_, path("/user/hand/left/input/trigger/value")},
        {grabAction_, path("/user/hand/left/input/squeeze/click")},
        {grabAction_, path("/user/hand/right/input/squeeze/click")},
        {xButtonAction_, path("/user/hand/left/input/menu/click")},
        {yButtonAction_, path("/user/hand/left/input/trackpad/click")},
        {aButtonAction_, path("/user/hand/right/input/trackpad/click")},
        {bButtonAction_, path("/user/hand/right/input/menu/click")},
        {thumbstickAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickAction_, path("/user/hand/right/input/thumbstick")},
        {thumbstickClickAction_, path("/user/hand/left/input/thumbstick/click")},
        {thumbstickClickAction_, path("/user/hand/right/input/thumbstick/click")}});
    const auto suggestIfEnabled = [&](const char* extension, const char* profile, const char* squeeze = "value") {
        const bool enabled = std::find(enabledOptionalExtensions_.begin(),
            enabledOptionalExtensions_.end(), extension) != enabledOptionalExtensions_.end();
        if (enabled)
            suggestXyController(profile, squeeze);
        else
            logging::Info(std::string("Skipping ") + profile + " (runtime lacks " + extension + ")");
    };
    suggestIfEnabled("XR_FB_touch_controller_pro", "/interaction_profiles/facebook/touch_controller_pro");
    suggestIfEnabled("XR_META_touch_controller_plus", "/interaction_profiles/meta/touch_controller_plus");
    suggestIfEnabled("XR_BD_controller_interaction", "/interaction_profiles/bytedance/pico_neo3_controller");
    suggestIfEnabled("XR_HTC_vive_cosmos_controller_interaction", "/interaction_profiles/htc/vive_cosmos_controller", "click");
    suggestIfEnabled("XR_EXT_hp_mixed_reality_controller", "/interaction_profiles/hp/mixed_reality_controller");

    XrSessionActionSetsAttachInfo attach(MakeXr<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO));
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    if (!Check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets"))
        return false;
    for (std::size_t hand = 0; hand < handPaths_.size(); ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo(MakeXr<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO));
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        spaceInfo.subactionPath = handPaths_[hand];
        spaceInfo.action = gripPoseAction_;
        if (!Check(xrCreateActionSpace(session_, &spaceInfo, &gripSpaces_[hand]),
                "xrCreateActionSpace(grip)"))
            return false;
        spaceInfo.action = aimPoseAction_;
        if (!Check(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[hand]),
                "xrCreateActionSpace(aim)"))
            return false;
    }
    logging::Info("OpenXR controller actions ready");
    return true;
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
    if ((controllerAxesEnabled_ || guiRayEnabled_ || directionRaysEnabled_) &&
        !CreateAxisSwapchain(formats))
    {
        logging::Error("Controller axis swapchain unavailable; controller input will continue");
        controllerAxesEnabled_ = guiRayEnabled_ = directionRaysEnabled_ = false;
    }
    if (ammoQuadEnabled_ && !CreateAmmoSwapchain(formats))
    {
        logging::Error("Ammo quad swapchain unavailable; the display stays off");
        ammoQuadEnabled_ = false;
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

bool OpenXrHost::CreateAmmoSwapchain(const std::vector<std::int64_t>& formats)
{
    const DXGI_FORMAT format = std::find(formats.begin(), formats.end(),
        static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM)) != formats.end()
        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(format)) ==
        formats.end())
        return false;
    ammoSwapchain_.height = ammoQuadPixelHeight_;
    ammoSwapchain_.width = dayz::ammo_display::kMaxCells *
        dayz::ammo_display::CellWidth(ammoSwapchain_.height);
    XrSwapchainCreateInfo info(MakeXr<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO));
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = ammoSwapchain_.width;
    info.height = ammoSwapchain_.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &info, &ammoSwapchain_.handle),
            "xrCreateSwapchain(ammo)"))
        return false;
    std::uint32_t count{};
    if (!Check(xrEnumerateSwapchainImages(ammoSwapchain_.handle, 0, &count, nullptr),
            "xrEnumerateSwapchainImages(ammo count)"))
        return false;
    ammoSwapchain_.images.resize(count);
    for (auto& image : ammoSwapchain_.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Check(xrEnumerateSwapchainImages(ammoSwapchain_.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(ammoSwapchain_.images.data())),
            "xrEnumerateSwapchainImages(ammo)"))
        return false;
    std::ostringstream message;
    message << "Ammo quad swapchain ready: " << ammoSwapchain_.width << 'x' << ammoSwapchain_.height
        << " images=" << count;
    logging::Info(message.str());
    return true;
}

bool OpenXrHost::PrepareAmmoLayer(XrCompositionLayerQuad& layer) noexcept
{
    if (!ammoQuadEnabled_ || ammoSwapchain_.handle == XR_NULL_HANDLE ||
        ammoQuadVisible_.load(std::memory_order_relaxed) == 0.0f ||
        !dayz::script_bridge::Enabled())
        return false;
    const XrSpaceLocation& grip = gripLocations_[1];
    constexpr XrSpaceLocationFlags kTracked =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((grip.locationFlags & kTracked) != kTracked)
        return false;
    const dayz::script_bridge::GameState game = dayz::script_bridge::GetGameState();
    if (!game.valid || game.weapon.empty() || (game.ammo < 0 && !game.chamber))
        return false;
    const std::string text = dayz::ammo_display::FormatAmmo(game.ammo, game.chamber);
    const std::uint32_t colour = dayz::ammo_display::ColourFor(game.ammo, game.chamber);
    if (!ammoSwapchain_.hasImage || text != ammoSwapchain_.text || colour != ammoSwapchain_.colour)
    {
        // Re-rasterise only on change: the bitmap is tiny, the upload is one call.
        dayz::ammo_display::Bitmap bitmap =
            dayz::ammo_display::Render(text, ammoSwapchain_.height, colour);
        // Right-align the text in the fixed-width swapchain so the quad's right edge
        // stays put over the hand while the digit count changes.
        std::vector<std::uint32_t> full(static_cast<std::size_t>(ammoSwapchain_.width) * ammoSwapchain_.height, 0u);
        const unsigned shift = ammoSwapchain_.width > bitmap.width ? ammoSwapchain_.width - bitmap.width : 0;
        for (unsigned y = 0; y < bitmap.height && y < ammoSwapchain_.height; ++y)
            for (unsigned x = 0; x < bitmap.width && x + shift < ammoSwapchain_.width; ++x)
                full[static_cast<std::size_t>(y) * ammoSwapchain_.width + x + shift] =
                    bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
        const auto update = dayz::xr::UpdateImage(ammoSwapchain_.handle, kImageCalls,
            [&](std::uint32_t imageIndex) {
                context_->UpdateSubresource(ammoSwapchain_.images[imageIndex].texture, 0, nullptr,
                    full.data(), ammoSwapchain_.width * sizeof(std::uint32_t), 0);
                return true;
            });
        if (!CheckImageUpdate(update, "ammo image update"))
            return false;
        ammoSwapchain_.text = text;
        ammoSwapchain_.colour = colour;
        ammoSwapchain_.hasImage = true;
    }
    layer = (MakeXr<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD));
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = localSpace_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = ammoSwapchain_.handle;
    layer.subImage.imageRect.offset = {0, 0};
    layer.subImage.imageRect.extent = {static_cast<std::int32_t>(ammoSwapchain_.width),
        static_cast<std::int32_t>(ammoSwapchain_.height)};
    // Tilt about the grip's x axis so the face (+z of the quad) turns towards the eyes.
    const float half = ammoQuadTiltDegrees_.load(std::memory_order_relaxed) * 3.14159265f / 360.0f;
    const XrQuaternionf tilt{std::sin(half), 0.0f, 0.0f, std::cos(half)};
    layer.pose.orientation = Multiply(grip.pose.orientation, tilt);
    const XrVector3f offset = Rotate(grip.pose.orientation,
        {ammoQuadOffsetX_.load(std::memory_order_relaxed), ammoQuadOffsetY_.load(std::memory_order_relaxed),
            ammoQuadOffsetZ_.load(std::memory_order_relaxed)});
    layer.pose.position = {grip.pose.position.x + offset.x, grip.pose.position.y + offset.y,
        grip.pose.position.z + offset.z};
    const float width = ammoQuadWidthMeters_.load(std::memory_order_relaxed);
    layer.size.width = width;
    layer.size.height = width * static_cast<float>(ammoSwapchain_.height) /
        static_cast<float>((std::max)(1u, ammoSwapchain_.width));
    return true;
}

bool OpenXrHost::CreateAxisSwapchain(const std::vector<std::int64_t>& formats)
{
    if (!controllerAxesEnabled_ && !guiRayEnabled_ && !directionRaysEnabled_)
        return true;
    const DXGI_FORMAT format = std::find(formats.begin(), formats.end(),
        static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM)) != formats.end()
        ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(format)) ==
        formats.end())
        return false;
    XrSwapchainCreateInfo info(MakeXr<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO));
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = 8;
    info.height = 1;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(xrCreateSwapchain(session_, &info, &axisSwapchain_.handle),
            "xrCreateSwapchain(controller axes)"))
        return false;
    std::uint32_t count{};
    if (!Check(xrEnumerateSwapchainImages(axisSwapchain_.handle, 0, &count, nullptr),
            "xrEnumerateSwapchainImages(controller axes count)"))
        return false;
    axisSwapchain_.images.resize(count);
    for (auto& image : axisSwapchain_.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    if (!Check(xrEnumerateSwapchainImages(axisSwapchain_.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(axisSwapchain_.images.data())),
            "xrEnumerateSwapchainImages(controller axes)"))
        return false;
    logging::Info("Controller XYZ axis swapchain ready");
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
    controllerInputEnabled_ = ReadBoolean(L"controls", L"enabled", true);
    controllerAxesEnabled_ = ReadBoolean(L"controls", L"show_controller_axes", true);
    ammoQuadEnabled_ = ReadBoolean(L"hud", L"ammo_quad", true);
    ammoQuadVisible_ = ammoQuadEnabled_ ? 1.0f : 0.0f;
    ammoQuadWidthMeters_ = (std::clamp)(ReadFloat(L"hud", L"ammo_quad_width_meters", 0.07f), 0.01f, 0.5f);
    ammoQuadOffsetX_ = (std::clamp)(ReadFloat(L"hud", L"ammo_quad_offset_x", 0.0f), -0.5f, 0.5f);
    ammoQuadOffsetY_ = (std::clamp)(ReadFloat(L"hud", L"ammo_quad_offset_y", 0.04f), -0.5f, 0.5f);
    ammoQuadOffsetZ_ = (std::clamp)(ReadFloat(L"hud", L"ammo_quad_offset_z", -0.02f), -0.5f, 0.5f);
    ammoQuadTiltDegrees_ = (std::clamp)(ReadFloat(L"hud", L"ammo_quad_tilt_degrees", 40.0f), -180.0f, 180.0f);
    ammoQuadPixelHeight_ = (std::clamp)(ReadUnsigned(L"hud", L"ammo_quad_pixel_height", 48), 8u, 256u);
    meleeMotionSwing_ = ReadBoolean(L"melee", L"motion_swing", false) ? 1.0f : 0.0f;
    meleeLightSpeed_ = (std::clamp)(ReadFloat(L"melee", L"light_speed", 1.6f), 0.2f, 20.0f);
    meleeHeavySpeed_ = (std::clamp)(ReadFloat(L"melee", L"heavy_speed", 3.2f), 0.2f, 20.0f);
    meleeCooldownSeconds_ = (std::clamp)(ReadFloat(L"melee", L"cooldown_seconds", 0.5f), 0.0f, 5.0f);
    meleeHeavyHoldSeconds_ = (std::clamp)(ReadFloat(L"melee", L"heavy_hold_seconds", 0.45f), 0.05f, 2.0f);
    vehicleSteering_ = ReadBoolean(L"vehicle", L"steering", true) ? 1.0f : 0.0f;
    vehicleWheelMaxDegrees_ = (std::clamp)(ReadFloat(L"vehicle", L"wheel_max_degrees", 90.0f), 10.0f, 180.0f);
    vehicleDeadzone_ = (std::clamp)(ReadFloat(L"vehicle", L"deadzone", 0.05f), 0.0f, 0.9f);
    vehicleInvert_ = ReadBoolean(L"vehicle", L"invert", false) ? 1.0f : 0.0f;
    vehicleRequireGrip_ = ReadBoolean(L"vehicle", L"require_grip", true) ? 1.0f : 0.0f;
    stancePhysical_ = ReadBoolean(L"stance", L"physical", false) ? 1.0f : 0.0f;
    stanceCrouchDrop_ = (std::clamp)(ReadFloat(L"stance", L"crouch_drop", 0.35f), 0.05f, 1.5f);
    stanceProneDrop_ = (std::clamp)(ReadFloat(L"stance", L"prone_drop", 0.85f), 0.1f, 2.0f);
    stanceHysteresis_ = (std::clamp)(ReadFloat(L"stance", L"hysteresis", 0.08f), 0.0f, 0.5f);
    // Same names and ranges as the ini keys; the clamps above and these bounds must agree.
    hostTunables_ = {{
        {"hud.ammo_quad", &ammoQuadVisible_, 0.0f, 1.0f},
        {"hud.ammo_quad_width_meters", &ammoQuadWidthMeters_, 0.01f, 0.5f},
        {"hud.ammo_quad_offset_x", &ammoQuadOffsetX_, -0.5f, 0.5f},
        {"hud.ammo_quad_offset_y", &ammoQuadOffsetY_, -0.5f, 0.5f},
        {"hud.ammo_quad_offset_z", &ammoQuadOffsetZ_, -0.5f, 0.5f},
        {"hud.ammo_quad_tilt_degrees", &ammoQuadTiltDegrees_, -180.0f, 180.0f},
        {"melee.motion_swing", &meleeMotionSwing_, 0.0f, 1.0f},
        {"melee.light_speed", &meleeLightSpeed_, 0.2f, 20.0f},
        {"melee.heavy_speed", &meleeHeavySpeed_, 0.2f, 20.0f},
        {"melee.cooldown_seconds", &meleeCooldownSeconds_, 0.0f, 5.0f},
        {"melee.heavy_hold_seconds", &meleeHeavyHoldSeconds_, 0.05f, 2.0f},
        {"vehicle.steering", &vehicleSteering_, 0.0f, 1.0f},
        {"vehicle.wheel_max_degrees", &vehicleWheelMaxDegrees_, 10.0f, 180.0f},
        {"vehicle.deadzone", &vehicleDeadzone_, 0.0f, 0.9f},
        {"vehicle.invert", &vehicleInvert_, 0.0f, 1.0f},
        {"vehicle.require_grip", &vehicleRequireGrip_, 0.0f, 1.0f},
        {"stance.physical", &stancePhysical_, 0.0f, 1.0f},
        {"stance.crouch_drop", &stanceCrouchDrop_, 0.05f, 1.5f},
        {"stance.prone_drop", &stanceProneDrop_, 0.1f, 2.0f},
        {"stance.hysteresis", &stanceHysteresis_, 0.0f, 0.5f},
    }};
    dayz::runtime_probe::RegisterTunables(hostTunables_.data(), hostTunables_.size());
    controllerAxesEnabled_ = controllerAxesEnabled_ && controllerInputEnabled_;
    guiRayEnabled_ = ReadBoolean(L"controls", L"show_gui_ray", true) &&
        controllerInputEnabled_;
    guiRayLength_ = (std::clamp)(ReadFloat(L"controls", L"gui_ray_length", 2.0f),
        0.2f, 10.0f);
    guiRayThickness_ = (std::clamp)(ReadFloat(L"controls", L"gui_ray_thickness", 0.004f),
        0.001f, 0.03f);
    directionRaysEnabled_ = ReadBoolean(L"controls", L"show_direction_rays", true) &&
        controllerInputEnabled_;
    directionRayLength_ = (std::clamp)(ReadFloat(L"controls",
        L"direction_ray_length", 3.0f), 0.2f, 20.0f);
    directionRayThickness_ = (std::clamp)(ReadFloat(L"controls",
        L"direction_ray_thickness", 0.006f), 0.001f, 0.03f);
    controllerTurnScale_ = ReadFloat(L"controls", L"turn_scale", 18.0f);
    recenterOnStickClick_ = ReadBoolean(L"controls", L"recenter_stick_click", true);
    dayz::comfort::Initialize(ConfigurationPath().c_str());
    controllerTurnRate_ = (std::clamp)(ReadFloat(L"controls", L"turn_rate", 90.0f), 0.0f, 720.0f);
    controllerSnapTurn_ = (std::clamp)(ReadFloat(L"controls", L"snap_turn", 0.0f), 0.0f, 180.0f);
    controllerDeadzone_ = (std::clamp)(ReadFloat(L"controls", L"deadzone", 0.3f),
        0.0f, 0.9f);
    logging::Info("Creating OpenXR session");
    if (!CreateSession())
        return false;
    if (!CreateControllerActions())
    {
        logging::Error("OpenXR controller actions unavailable; headset rendering will continue");
        controllerInputEnabled_ = false;
        controllerAxesEnabled_ = false;
    }
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
            if (sessionState_ != XR_SESSION_STATE_FOCUSED)
                ReleaseControllerKeys();
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
                ReleaseControllerKeys();
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

void OpenXrHost::ReleaseControllerKeys() noexcept
{
    ReleaseInjectedInput();
    for (auto& location : gripLocations_)
        location = MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    for (auto& location : aimLocations_)
        location = MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    dayz::stereo_state::UpdateAimOrientation(0.0f, 0.0f, 0.0f, 1.0f, false);
    dayz::script_bridge::SetVehicleSteer(0.0f, false);
    dayz::script_bridge::SetVehiclePedals(0.0f, 0.0f, false);
}

// Head height below the standing reference selects crouch/prone; DayZ only has
// toggle keys (C crouch, Z prone), so one tap at a time moves towards the wanted
// stance and the bridge's reported stance confirms it before the next tap.
void OpenXrHost::UpdatePhysicalStance(XrTime displayTime) noexcept
{
    if (stanceKeyReleaseTime_ && displayTime >= stanceKeyReleaseTime_)
    {
        SendKey(stanceKey_, false);
        stanceKeyReleaseTime_ = 0;
    }
    const dayz::stereo_state::HmdPosition head = dayz::stereo_state::GetHmdPosition();
    const unsigned generation = dayz::runtime_probe::RecenterGeneration();
    if (generation != recenterGenerationSeen_)
    {
        recenterGenerationSeen_ = generation;
        haveStandingHeight_ = false;
    }
    if (!head.valid)
        return;
    if (!haveStandingHeight_)
    {
        standingHeight_ = head.y;
        haveStandingHeight_ = true;
        return;
    }
    if (stancePhysical_.load(std::memory_order_relaxed) == 0.0f || stanceKeyReleaseTime_ ||
        displayTime < stanceNextChangeTime_)
        return;
    const dayz::script_bridge::GameState game = dayz::script_bridge::GetGameState();
    if (!game.valid || game.inventoryOpen || game.inVehicle || game.stance < 0)
        return;
    const int current = game.stance % 3;  // raised variants 3..5 map onto 0..2
    dayz::physical_stance::Config config;
    config.crouchDropMeters = stanceCrouchDrop_.load(std::memory_order_relaxed);
    config.proneDropMeters = stanceProneDrop_.load(std::memory_order_relaxed);
    config.hysteresisMeters = stanceHysteresis_.load(std::memory_order_relaxed);
    const int desired = dayz::physical_stance::Desired(standingHeight_ - head.y, current, config);
    if (desired == current)
        return;
    using dayz::physical_stance::Prone;
    using dayz::physical_stance::Crouch;
    stanceKey_ = static_cast<WORD>(desired == Prone || (current == Prone && desired != Crouch) ? 'Z' : 'C');
    SendKey(stanceKey_, true);
    stanceKeyReleaseTime_ = displayTime + 60000000;         // 60 ms tap
    stanceNextChangeTime_ = displayTime + 700000000;        // let the animation finish
    std::ostringstream message;
    message << "physical stance: head drop " << (standingHeight_ - head.y) << " m, stance " << current
        << " -> " << desired << " via " << static_cast<char>(stanceKey_);
    logging::Info(message.str());
}

// Two-hand wheel from both grips -> vr.txt steer= (applied by the Enforce side only
// while the local player drives, so publishing it every frame is harmless).
void OpenXrHost::PublishVehicleSteering() noexcept
{
    if (vehicleSteering_.load(std::memory_order_relaxed) == 0.0f)
    {
        dayz::script_bridge::SetVehicleSteer(0.0f, false);
        return;
    }
    constexpr XrSpaceLocationFlags kPosition = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    // "Grab the wheel": resting hands must not steer, so both squeezes have to be held.
    const auto squeezed = [&](std::size_t hand) {
        if (vehicleRequireGrip_.load(std::memory_order_relaxed) == 0.0f)
            return true;
        XrActionStateFloat state(MakeXr<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT));
        XrActionStateGetInfo get(MakeXr<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO));
        get.action = grabAction_;
        get.subactionPath = handPaths_[hand];
        return XR_SUCCEEDED(xrGetActionStateFloat(session_, &get, &state)) && state.isActive &&
            state.currentState > 0.5f;
    };
    dayz::vehicle_steering::Hands hands;
    hands.leftValid = (gripLocations_[0].locationFlags & kPosition) != 0 && squeezed(0);
    hands.rightValid = (gripLocations_[1].locationFlags & kPosition) != 0 && squeezed(1);
    hands.leftX = gripLocations_[0].pose.position.x;
    hands.leftY = gripLocations_[0].pose.position.y;
    hands.leftZ = gripLocations_[0].pose.position.z;
    hands.rightX = gripLocations_[1].pose.position.x;
    hands.rightY = gripLocations_[1].pose.position.y;
    hands.rightZ = gripLocations_[1].pose.position.z;
    dayz::vehicle_steering::Config config;
    config.wheelMaxDegrees = vehicleWheelMaxDegrees_.load(std::memory_order_relaxed);
    config.deadzone = vehicleDeadzone_.load(std::memory_order_relaxed);
    config.invert = vehicleInvert_.load(std::memory_order_relaxed) != 0.0f;
    const dayz::vehicle_steering::Result result = dayz::vehicle_steering::Compute(hands, config);
    dayz::script_bridge::SetVehicleSteer(result.steer, result.valid);
}

// Feeds the right grip position to the swing detector and holds DayZ's attack
// button (left mouse) for a tap or a heavy-attack hold. Returns true while held.
bool OpenXrHost::UpdateMotionMelee(XrTime displayTime, float dt, bool guiVisible) noexcept
{
    if (meleeReleaseTime_ && displayTime >= meleeReleaseTime_)
        meleeReleaseTime_ = 0;
    if (meleeMotionSwing_.load(std::memory_order_relaxed) == 0.0f || guiVisible)
    {
        meleeSwing_.Reset();
        return meleeReleaseTime_ != 0;
    }
    const XrSpaceLocation& grip = gripLocations_[1];
    const dayz::script_bridge::GameState game = dayz::script_bridge::GetGameState();
    if (!(grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !game.valid ||
        !game.melee || game.inventoryOpen)
    {
        meleeSwing_.Reset();
        return meleeReleaseTime_ != 0;
    }
    dayz::melee::Config config;
    config.lightSpeed = meleeLightSpeed_.load(std::memory_order_relaxed);
    config.heavySpeed = meleeHeavySpeed_.load(std::memory_order_relaxed);
    config.cooldownSeconds = meleeCooldownSeconds_.load(std::memory_order_relaxed);
    const dayz::melee::Swing swing = meleeSwing_.Update(grip.pose.position.x, grip.pose.position.y,
        grip.pose.position.z, dt, config);
    if (swing != dayz::melee::Swing::None && !meleeReleaseTime_)
    {
        const float holdSeconds = swing == dayz::melee::Swing::Heavy ?
            meleeHeavyHoldSeconds_.load(std::memory_order_relaxed) : 0.06f;
        meleeReleaseTime_ = displayTime + static_cast<XrTime>(holdSeconds * 1e9f);
        std::ostringstream message;
        message << "motion melee: " << (swing == dayz::melee::Swing::Heavy ? "heavy" : "light")
            << " swing at " << meleeSwing_.Speed() << " m/s";
        logging::Info(message.str());
    }
    return meleeReleaseTime_ != 0;
}

void OpenXrHost::ReleaseInjectedInput() noexcept
{
    leftStickClickDown_ = false;
    snapTurnArmed_ = true;
    lastTurnTime_ = 0;
    meleeReleaseTime_ = 0;
    meleeSwing_.Reset();
    if (stanceKeyReleaseTime_)
    {
        SendKey(stanceKey_, false);
        stanceKeyReleaseTime_ = 0;
    }
    guiRayValid_ = false;
    const auto comfort = dayz::stereo_state::GetComfortVignette();
    dayz::stereo_state::SetComfortVignette(0.0f, comfort.radius);
    constexpr WORD keys[4]{'W', 'A', 'S', 'D'};
    for (std::size_t index = 0; index < movementKeys_.size(); ++index)
        if (movementKeys_[index])
        {
            SendKey(keys[index], false);
            movementKeys_[index] = false;
        }
    if (leftMouseDown_)
    {
        SendMouseButton(false, false);
        leftMouseDown_ = false;
    }
    if (rightMouseDown_)
    {
        SendMouseButton(true, false);
        rightMouseDown_ = false;
    }
    if (xKeyDown_)
    {
        SendKey('C', false);
        xKeyDown_ = false;
    }
    if (yKeyDown_)
    {
        SendKey('R', false);
        yKeyDown_ = false;
    }
    if (escapeKeyDown_)
    {
        SendKey(VK_ESCAPE, false);
        escapeKeyDown_ = false;
    }
    if (tabKeyDown_)
    {
        SendKey(VK_TAB, false);
        tabKeyDown_ = false;
    }
    if (rightGrabDown_)
    {
        SendKey('F', false);
        rightGrabDown_ = false;
    }
    if (aButtonDown_)
    {
        SendKey(VK_SHIFT, false);
        aButtonDown_ = false;
    }
    if (bButtonDown_)
    {
        SendKey(VK_SPACE, false);
        bButtonDown_ = false;
    }
    if (hotbarPreviousDown_)
    {
        SendKey(hotbarPreviousKey_, false);
        hotbarPreviousDown_ = false;
    }
    if (hotbarNextDown_)
    {
        SendKey(hotbarNextKey_, false);
        hotbarNextDown_ = false;
    }
}

void OpenXrHost::SyncControllerInput(XrTime displayTime, bool guiVisible, bool injectInput)
{
    if (!controllerInputEnabled_ || actionSet_ == XR_NULL_HANDLE)
    {
        ReleaseControllerKeys();
        return;
    }
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync(MakeXr<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO));
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult synchronized = xrSyncActions(session_, &sync);
    if (synchronized != XR_SUCCESS)
    {
        if (XR_FAILED(synchronized))
            logging::XrError("xrSyncActions", synchronized);
        ReleaseControllerKeys();
        return;
    }
    static XrPath loggedInteractionProfile{XR_NULL_PATH};
    XrInteractionProfileState interaction(MakeXr<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE));
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPaths_[0], &interaction)) &&
        interaction.interactionProfile != XR_NULL_PATH &&
        interaction.interactionProfile != loggedInteractionProfile)
    {
        char profile[XR_MAX_PATH_LENGTH]{};
        std::uint32_t length{};
        if (XR_SUCCEEDED(xrPathToString(instance_, interaction.interactionProfile,
                static_cast<std::uint32_t>(std::size(profile)), &length, profile)))
        {
            logging::Info(std::string("OpenXR left controller profile: ") + profile);
            loggedInteractionProfile = interaction.interactionProfile;
        }
    }
    for (std::size_t hand = 0; hand < handPaths_.size(); ++hand)
    {
        gripLocations_[hand] = (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION));
        aimLocations_[hand] = (MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION));
        xrLocateSpace(gripSpaces_[hand], localSpace_, displayTime, &gripLocations_[hand]);
        xrLocateSpace(aimSpaces_[hand], localSpace_, displayTime, &aimLocations_[hand]);
    }
    {
        const XrSpaceLocation& rightAim = aimLocations_[1];
        const bool valid = (rightAim.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        dayz::stereo_state::UpdateAimOrientation(rightAim.pose.orientation.x,
            rightAim.pose.orientation.y, rightAim.pose.orientation.z,
            rightAim.pose.orientation.w, valid);
    }
    PublishVehicleSteering();
    // vr_common is shared by the DLL and the standalone probe. _WINDLL describes
    // how this translation unit was compiled, not which host is using it.
    if (!gameSwapChain_)
        return;
    if (!injectInput)
    {
        // Poses above stay live (ammo quad, rays, debug snapshot); nothing may reach
        // the game while its window is not the desktop foreground.
        ReleaseInjectedInput();
        return;
    }
    const auto vectorState = [&](std::size_t hand) {
        XrActionStateVector2f state(MakeXr<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F));
        XrActionStateGetInfo get(MakeXr<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO));
        get.action = thumbstickAction_;
        get.subactionPath = handPaths_[hand];
        xrGetActionStateVector2f(session_, &get, &state);
        return state.isActive ? state.currentState : XrVector2f{};
    };
    const XrVector2f leftStick = vectorState(0);
    const XrVector2f rightStick = vectorState(1);
    const bool desired[4]{leftStick.y > controllerDeadzone_,
        leftStick.x < -controllerDeadzone_, leftStick.y < -controllerDeadzone_,
        leftStick.x > controllerDeadzone_};
    constexpr WORD keys[4]{'W', 'A', 'S', 'D'};
    for (std::size_t index = 0; index < movementKeys_.size(); ++index)
        if (movementKeys_[index] != desired[index])
        {
            SendKey(keys[index], desired[index]);
            movementKeys_[index] = desired[index];
        }
    const bool turning = std::fabs(rightStick.x) > controllerDeadzone_;
    const bool moving = desired[0] || desired[1] || desired[2] || desired[3];
    const float inputSeconds = dayz::xr::AdvanceInputClock(lastTurnTime_, displayTime);
    dayz::comfort::Update(moving, turning && controllerSnapTurn_ <= 0.0f, inputSeconds);
    if (dayz::runtime_probe::ClosedLoopAimActive())
    {
        // With the closed loop owning DayZ's mouse camera, stick turns rotate
        // the yaw target instead of injecting raw counts that the loop would
        // immediately undo. Snap turn fires once per deflection past the deadzone.
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        if (controllerSnapTurn_ > 0.0f)
        {
            if (turning && snapTurnArmed_)
            {
                dayz::runtime_probe::AddAimYawOffset(
                    (rightStick.x > 0.0f ? -1.0f : 1.0f) * controllerSnapTurn_ * kDegToRad);
                snapTurnArmed_ = false;
            }
            else if (!turning)
                snapTurnArmed_ = true;
        }
        else if (turning)
            dayz::runtime_probe::AddAimYawOffset(-rightStick.x * controllerTurnRate_ * kDegToRad * inputSeconds);
    }
    else if (turning)
        SendMouseTurn(static_cast<LONG>(std::lround(rightStick.x * controllerTurnScale_)));

    const auto booleanState = [&](XrAction action, std::size_t hand) {
        XrActionStateBoolean state(MakeXr<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN));
        XrActionStateGetInfo get(MakeXr<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO));
        get.action = action;
        get.subactionPath = handPaths_[hand];
        xrGetActionStateBoolean(session_, &get, &state);
        return state;
    };
    const XrActionStateBoolean xState = booleanState(xButtonAction_, 0);
    const XrActionStateBoolean yState = booleanState(yButtonAction_, 0);
    const XrActionStateBoolean aState = booleanState(aButtonAction_, 1);
    const XrActionStateBoolean bState = booleanState(bButtonAction_, 1);
    const auto floatState = [&](XrAction action, std::size_t hand) {
        XrActionStateFloat state(MakeXr<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT));
        XrActionStateGetInfo get(MakeXr<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO));
        get.action = action;
        get.subactionPath = handPaths_[hand];
        xrGetActionStateFloat(session_, &get, &state);
        return state;
    };
    const XrActionStateFloat leftGrabState = floatState(grabAction_, 0);
    const XrActionStateFloat rightGrabState = floatState(grabAction_, 1);
    const XrActionStateFloat leftTriggerState = floatState(triggerAction_, 0);
    const XrActionStateFloat rightTriggerState = floatState(triggerAction_, 1);
    // Driving: the triggers are pedals for the script side (right throttle, left
    // brake) instead of mouse buttons.
    const bool driving = dayz::script_bridge::GetGameState().inVehicle &&
        vehicleSteering_.load(std::memory_order_relaxed) != 0.0f;
    dayz::script_bridge::SetVehiclePedals(
        rightTriggerState.isActive ? (std::clamp)(rightTriggerState.currentState, 0.0f, 1.0f) : 0.0f,
        leftTriggerState.isActive ? (std::clamp)(leftTriggerState.currentState, 0.0f, 1.0f) : 0.0f,
        driving && (rightTriggerState.isActive || leftTriggerState.isActive));
    const bool xDown = xState.isActive && xState.currentState;
    if (recenterOnStickClick_)
    {
        const XrActionStateBoolean clickState = booleanState(thumbstickClickAction_, 0);
        const bool clickDown = clickState.isActive && clickState.currentState;
        if (clickDown && !leftStickClickDown_)
        {
            dayz::runtime_probe::RecenterHmd();
            logging::Info("controller left stick click -> recenter");
        }
        leftStickClickDown_ = clickDown;
    }
    const bool yDown = yState.isActive && yState.currentState;
    const bool leftGrabDown = leftGrabState.isActive && leftGrabState.currentState > 0.55f;
    const bool rightGrabDown = rightGrabState.isActive && rightGrabState.currentState > 0.55f;
    const bool aDown = aState.isActive && aState.currentState;
    const bool bDown = bState.isActive && bState.currentState;
    const auto updateKey = [&](WORD key, bool desired, bool& current,
        const char* downMessage, const char* upMessage) {
        if (desired == current)
            return;
        SendKey(key, desired);
        logging::Info(desired ? downMessage : upMessage);
        current = desired;
    };
    updateKey('C', xDown && !leftGrabDown, xKeyDown_,
        "controller X -> C down", "controller X -> C up");
    updateKey(VK_ESCAPE, xDown && leftGrabDown, escapeKeyDown_,
        "controller LGRAB+X -> Escape down", "controller LGRAB+X -> Escape up");
    updateKey('R', yDown && !leftGrabDown, yKeyDown_,
        "controller Y -> R down", "controller Y -> R up");
    updateKey(VK_TAB, yDown && leftGrabDown, tabKeyDown_,
        "controller LGRAB+Y -> Tab down", "controller LGRAB+Y -> Tab up");
    updateKey('F', rightGrabDown, rightGrabDown_,
        "controller RGRAB -> F down", "controller RGRAB -> F up");
    updateKey(VK_SHIFT, aDown && !leftGrabDown, aButtonDown_,
        "controller A -> Shift down", "controller A -> Shift up");
    updateKey(VK_SPACE, bDown && !leftGrabDown, bButtonDown_,
        "controller B -> Space down", "controller B -> Space up");
    const auto hotbarVirtualKey = [](unsigned slot) -> WORD {
        return slot == 10 ? '0' : static_cast<WORD>('0' + slot);
    };
    const auto updateHotbar = [&](bool desired, bool& current, WORD& activeKey,
        int direction, const char* name) {
        if (desired == current)
            return;
        if (desired)
        {
            if (direction < 0)
                hotbarSlot_ = hotbarSlot_ == 1 ? 10 : hotbarSlot_ - 1;
            else
                hotbarSlot_ = hotbarSlot_ == 10 ? 1 : hotbarSlot_ + 1;
            activeKey = hotbarVirtualKey(hotbarSlot_);
            SendKey(activeKey, true);
            std::ostringstream message;
            message << name << " -> hotbar slot " << hotbarSlot_ << " down";
            logging::Info(message.str());
        }
        else
        {
            SendKey(activeKey, false);
            std::ostringstream message;
            message << name << " -> hotbar slot " << hotbarSlot_ << " up";
            logging::Info(message.str());
        }
        current = desired;
    };
    updateHotbar(leftGrabDown && aDown, hotbarPreviousDown_, hotbarPreviousKey_, -1,
        "controller LGRAB+A");
    updateHotbar(leftGrabDown && bDown, hotbarNextDown_, hotbarNextKey_, 1,
        "controller LGRAB+B");

    const bool meleeHeld = UpdateMotionMelee(displayTime, inputSeconds, guiVisible);
    if (!guiVisible)
        UpdatePhysicalStance(displayTime);
    const bool desiredLeftMouse = meleeHeld || (!driving && rightTriggerState.isActive &&
        rightTriggerState.currentState > 0.55f);
    if (desiredLeftMouse != leftMouseDown_)
    {
        SendMouseButton(false, desiredLeftMouse);
        leftMouseDown_ = desiredLeftMouse;
    }
    const bool desiredRightMouse = !driving && leftTriggerState.isActive &&
        leftTriggerState.currentState > 0.55f;
    if (desiredRightMouse != rightMouseDown_)
    {
        SendMouseButton(true, desiredRightMouse);
        rightMouseDown_ = desiredRightMouse;
    }

    bool cursorHit{};
    guiRayValid_ = guiVisible &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    currentGuiRayLength_ = guiRayLength_;
    if (guiVisible && guiQuadAnchored_ &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (aimLocations_[1].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        const XrPosef& aim = aimLocations_[1].pose;
        const XrVector3f ray = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
        const XrQuaternionf inverse{-guiQuadPose_.orientation.x, -guiQuadPose_.orientation.y,
            -guiQuadPose_.orientation.z, guiQuadPose_.orientation.w};
        const XrVector3f relative{aim.position.x - guiQuadPose_.position.x,
            aim.position.y - guiQuadPose_.position.y,
            aim.position.z - guiQuadPose_.position.z};
        const XrVector3f localOrigin = Rotate(inverse, relative);
        const XrVector3f localDirection = Rotate(inverse, ray);
        if (std::fabs(localDirection.z) > 0.00001f)
        {
            const float distance = -localOrigin.z / localDirection.z;
            const float height = guiQuadWidthMeters_ *
                static_cast<float>(guiSwapchain_.height) /
                static_cast<float>((std::max)(1u, guiSwapchain_.width));
            const float x = localOrigin.x + localDirection.x * distance;
            const float y = localOrigin.y + localDirection.y * distance;
            if (distance > 0.0f && std::fabs(x) <= guiQuadWidthMeters_ * 0.5f &&
                std::fabs(y) <= height * 0.5f)
            {
                cursorHit = true;
                currentGuiRayLength_ = distance;
                dayz::runtime_probe::SetGuiVirtualCursorNormalized(
                    x / guiQuadWidthMeters_ + 0.5f, 0.5f - y / height);
            }
        }
    }
    static std::uint64_t inputLogCounter{};
    if (++inputLogCounter % 180 == 0)
    {
        std::ostringstream message;
        message << "controller input left=" << leftStick.x << ',' << leftStick.y
            << " right=" << rightStick.x << ',' << rightStick.y
            << " X=" << xDown << "(active=" << xState.isActive << ')'
            << " Y=" << yDown << "(active=" << yState.isActive << ')'
            << " A=" << aDown << "(active=" << aState.isActive << ')'
            << " B=" << bDown << "(active=" << bState.isActive << ')'
            << " LGrab=" << leftGrabState.currentState
            << " RGrab=" << rightGrabState.currentState
            << " LTrigger=" << leftTriggerState.currentState
            << " RTrigger=" << rightTriggerState.currentState
            << " gui_hit=" << cursorHit;
        if (guiRayValid_)
        {
            const XrVector3f aimForward = Rotate(aimLocations_[1].pose.orientation,
                {0.0f, 0.0f, -1.0f});
            message << " right_aim_forward=(" << aimForward.x << ',' << aimForward.y
                << ',' << aimForward.z << ')';
        }
        logging::Info(message.str());
    }
}

void OpenXrHost::RenderFrame()
{
    XrFrameWaitInfo waitInfo(MakeXr<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO));
    XrFrameState frameState(MakeXr<XrFrameState>(XR_TYPE_FRAME_STATE));
    const auto frameStart = std::chrono::steady_clock::now();
    if (!Check(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame"))
    {
        ReleaseControllerKeys();
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
        return;
    }
    timing_.waitFrame += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    XrFrameBeginInfo beginInfo(MakeXr<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO));
    if (!Check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame"))
    {
        ReleaseControllerKeys();
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
    const HWND foreground = dayz::runtime_probe::RealForegroundWindow();
    DWORD foregroundProcess{};
    const bool desktopFocused = foreground && GetWindowThreadProcessId(foreground, &foregroundProcess) &&
        foregroundProcess == GetCurrentProcessId();
    const bool rendering = frameState.shouldRender != XR_FALSE;
    if (dayz::xr::TrackingAllowed(sessionState_.load(), rendering, located))
        SyncControllerInput(frameState.predictedDisplayTime, guiVisible,
            dayz::xr::InputAllowed(sessionState_.load(), rendering, located,
                gameSwapChain_ != nullptr, desktopFocused));
    else
        ReleaseControllerKeys();
    if (!located || !frameState.shouldRender)
    {
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
        debugSnapshot_.grip = {};
        debugSnapshot_.aim = {};
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
        if (guiVisible && (!guiQuadWasVisible_ || !guiQuadAnchored_))
            AnchorGuiQuad(views_[0].pose);
        if (gameFrameSource_)
            gameFrameSource_->PrepareFrame(dayz::stereo_state::RenderedEye());
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
            debugSnapshot_.grip = gripLocations_;
            debugSnapshot_.aim = aimLocations_;
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
    std::array<XrCompositionLayerQuad, 14> axisLayers{};
    std::uint32_t axisLayerCount{};
    if (projectionReady && !shouldExit_ &&
        (controllerAxesEnabled_ || guiRayEnabled_ || directionRaysEnabled_) &&
        axisSwapchain_.handle != XR_NULL_HANDLE)
    {
        const auto update = dayz::xr::UpdateImage(axisSwapchain_.handle, kImageCalls,
            [&](std::uint32_t imageIndex) {
                const std::uint32_t pixels[8]{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u,
                    0x70FFFF00u, 0xB0FFFFFFu, 0xB0FF8000u, 0xB000FFFFu, 0xB0FF00FFu};
                context_->UpdateSubresource(axisSwapchain_.images[imageIndex].texture, 0,
                    nullptr, pixels, sizeof(pixels), 0);
                return true;
            });
        const bool axisReady = CheckImageUpdate(update, "axis image update");
        constexpr float s = 0.70710678f;
        if (axisReady && controllerAxesEnabled_)
        {
            constexpr float halfLength = 0.04f;
            constexpr float thickness = 0.006f;
            const XrVector3f directions[3]{{1.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
            const XrQuaternionf axisRotations[3]{{0.0f, 0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, s, s}, {0.0f, -s, 0.0f, s}};
            for (std::size_t hand = 0; hand < gripLocations_.size(); ++hand)
            {
                const auto& location = gripLocations_[hand];
                if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
                    continue;
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    XrCompositionLayerQuad& axisLayer = axisLayers[axisLayerCount++];
                    axisLayer = (MakeXr<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD));
                    axisLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    axisLayer.space = localSpace_;
                    axisLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    axisLayer.subImage.swapchain = axisSwapchain_.handle;
                    axisLayer.subImage.imageRect.offset = {static_cast<std::int32_t>(axis), 0};
                    axisLayer.subImage.imageRect.extent = {1, 1};
                    const XrVector3f direction = Rotate(location.pose.orientation,
                        directions[axis]);
                    axisLayer.pose.position = {
                        location.pose.position.x + direction.x * halfLength,
                        location.pose.position.y + direction.y * halfLength,
                        location.pose.position.z + direction.z * halfLength};
                    axisLayer.pose.orientation = Multiply(location.pose.orientation,
                        axisRotations[axis]);
                    axisLayer.size = {halfLength * 2.0f, thickness};
                }
            }
        }
        if (axisReady && guiRayEnabled_ && guiVisible && guiRayValid_ &&
            axisLayerCount + 4 <= axisLayers.size())
        {
            const XrPosef& aim = aimLocations_[1].pose;
            const XrVector3f direction = Rotate(aim.orientation, {0.0f, 0.0f, -1.0f});
            const XrVector3f center{aim.position.x + direction.x * currentGuiRayLength_ * 0.5f,
                aim.position.y + direction.y * currentGuiRayLength_ * 0.5f,
                aim.position.z + direction.z * currentGuiRayLength_ * 0.5f};
            const XrQuaternionf alongRay = Multiply(aim.orientation, {0.0f, s, 0.0f, s});
            const XrQuaternionf quarterTurn{s, 0.0f, 0.0f, s};
            const XrQuaternionf reverse{1.0f, 0.0f, 0.0f, 0.0f};
            const XrQuaternionf orientations[4]{alongRay,
                Multiply(alongRay, reverse), Multiply(alongRay, quarterTurn),
                Multiply(Multiply(alongRay, quarterTurn), reverse)};
            for (const XrQuaternionf& orientation : orientations)
            {
                XrCompositionLayerQuad& rayLayer = axisLayers[axisLayerCount++];
                rayLayer = (MakeXr<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD));
                rayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                rayLayer.space = localSpace_;
                rayLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                rayLayer.subImage.swapchain = axisSwapchain_.handle;
                rayLayer.subImage.imageRect.offset = {3, 0};
                rayLayer.subImage.imageRect.extent = {1, 1};
                rayLayer.pose.position = center;
                rayLayer.pose.orientation = orientation;
                rayLayer.size = {currentGuiRayLength_, guiRayThickness_};
            }
        }
        if (axisReady && directionRaysEnabled_ && !guiVisible && axisLayerCount + 6 <= axisLayers.size())
        {
            const XrVector3f origin{
                (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f,
                (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f,
                (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f};
            const XrVector3f hmdForward = Rotate(views_[0].pose.orientation,
                {0.0f, 0.0f, -1.0f});
            const dayz::stereo_state::CameraDirections cameraDirections =
                dayz::stereo_state::GetCameraDirections();
            const XrVector3f nativeForward{cameraDirections.nativeX,
                cameraDirections.nativeY, cameraDirections.nativeZ};
            const XrVector3f renderForward{cameraDirections.renderX,
                cameraDirections.renderY, cameraDirections.renderZ};
            // The right-controller ray is a GUI pointer only. Keep the gameplay
            // diagnostics limited to HMD, native DayZ aim, and render-camera aim.
            const XrVector3f directions[3]{hmdForward,
                cameraDirections.valid ? nativeForward : hmdForward,
                cameraDirections.valid ? renderForward : hmdForward};
            const std::int32_t colorPixels[3]{4, 6, 7};
            const XrQuaternionf crossTurn{s, 0.0f, 0.0f, s};
            for (std::size_t ray = 0; ray < std::size(directions); ++ray)
            {
                const XrQuaternionf along = OrientAlongX(directions[ray]);
                const XrQuaternionf orientations[2]{along, Multiply(along, crossTurn)};
                for (const XrQuaternionf& orientation : orientations)
                {
                    XrCompositionLayerQuad& rayLayer = axisLayers[axisLayerCount++];
                    rayLayer = (MakeXr<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD));
                    rayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    rayLayer.space = localSpace_;
                    rayLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    rayLayer.subImage.swapchain = axisSwapchain_.handle;
                    rayLayer.subImage.imageRect.offset = {colorPixels[ray], 0};
                    rayLayer.subImage.imageRect.extent = {1, 1};
                    rayLayer.pose.position = {origin.x + directions[ray].x *
                        directionRayLength_ * 0.5f, origin.y + directions[ray].y *
                        directionRayLength_ * 0.5f, origin.z + directions[ray].z *
                        directionRayLength_ * 0.5f};
                    rayLayer.pose.orientation = orientation;
                    rayLayer.size = {directionRayLength_, directionRayThickness_};
                }
            }
        }
    }
    std::array<const XrCompositionLayerBaseHeader*, 16> layers{};
    std::uint32_t layerCount = projectionReady && !shouldExit_ ? 1u : 0u;
    if (layerCount)
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    for (std::uint32_t index = 0; layerCount && index < axisLayerCount && layerCount < layers.size(); ++index)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &axisLayers[index]);
    XrCompositionLayerQuad ammoLayer{};
    if (layerCount && !guiVisible && layerCount < layers.size() && PrepareAmmoLayer(ammoLayer))
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ammoLayer);
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
        ReleaseControllerKeys();
        dayz::stereo_state::InvalidateTracking();
        std::scoped_lock debugLock(debugMutex_);
        debugSnapshot_.hmdValid = false;
        debugSnapshot_.grip = {};
        debugSnapshot_.aim = {};
    }
}

void OpenXrHost::Shutdown() noexcept
{
    std::scoped_lock lock(mutex_);
    ReleaseControllerKeys();
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
    axisSwapchain_.images.clear();
    if (axisSwapchain_.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(axisSwapchain_.handle);
    if (ammoSwapchain_.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(ammoSwapchain_.handle);
    ammoSwapchain_ = {};
    axisSwapchain_.handle = XR_NULL_HANDLE;
    for (XrSpace& space : aimSpaces_)
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    for (XrSpace& space : gripSpaces_)
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    aimSpaces_.fill(XR_NULL_HANDLE);
    gripSpaces_.fill(XR_NULL_HANDLE);
    if (actionSet_ != XR_NULL_HANDLE)
        xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
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
