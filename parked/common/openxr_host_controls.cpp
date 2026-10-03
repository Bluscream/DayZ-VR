// Parked: controller input, script-bridge consumers, ammo quad, axis/ray layers,
// motion melee, physical stance, two-hand steering and haptics cut out of
// common/openxr_host.cpp. Not compiled; an excerpt, not a standalone unit.
// Restore by re-inserting the ranges into openxr_host.cpp (see git history for
// the full file before the cut).

// ---- quaternion and SendInput helpers (openxr_host.cpp) ----
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

// ---- optional controller extensions (openxr_host.cpp) ----
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

// ---- host tunables and controller actions (openxr_host.cpp) ----
namespace
{
    // One row per host-owned live tunable: the ini key ("section.key"), its default
    // and range. The ini read, the clamp and the debug-protocol table all come from
    // here, so a bound can no longer disagree with itself. Booleans are 0/1 floats.
    struct HostTunableSpec
    {
        const char* name;
        float fallback;
        float minimum;
        float maximum;
        bool boolean;
    };

    float ReadTunable(const HostTunableSpec& spec, float current) noexcept
    {
        const std::string name(spec.name);
        const auto dot = name.find('.');
        if (dot == std::string::npos)
            return current;
        const std::wstring section(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(dot));
        const std::wstring key(name.begin() + static_cast<std::ptrdiff_t>(dot) + 1, name.end());
        if (spec.boolean)
            return ReadBoolean(section.c_str(), key.c_str(), spec.fallback >= 0.5f) ? 1.0f : 0.0f;
        return (std::clamp)(ReadFloat(section.c_str(), key.c_str(), spec.fallback), spec.minimum, spec.maximum);
    }
}

void OpenXrHost::LoadHostTunables() noexcept
{
    struct Row
    {
        HostTunableSpec spec;
        std::atomic<float>* value;
    };
    const Row rows[]{
        {{"hud.ammo_quad", 1.0f, 0.0f, 1.0f, true}, &ammoQuadVisible_},
        {{"hud.ammo_quad_width_meters", 0.07f, 0.01f, 0.5f, false}, &ammoQuadWidthMeters_},
        {{"hud.ammo_quad_offset_x", 0.0f, -0.5f, 0.5f, false}, &ammoQuadOffsetX_},
        {{"hud.ammo_quad_offset_y", 0.04f, -0.5f, 0.5f, false}, &ammoQuadOffsetY_},
        {{"hud.ammo_quad_offset_z", -0.02f, -0.5f, 0.5f, false}, &ammoQuadOffsetZ_},
        {{"hud.ammo_quad_tilt_degrees", 40.0f, -180.0f, 180.0f, false}, &ammoQuadTiltDegrees_},
        {{"melee.motion_swing", 0.0f, 0.0f, 1.0f, true}, &meleeMotionSwing_},
        {{"melee.light_speed", 1.6f, 0.2f, 20.0f, false}, &meleeLightSpeed_},
        {{"melee.heavy_speed", 3.2f, 0.2f, 20.0f, false}, &meleeHeavySpeed_},
        {{"melee.cooldown_seconds", 0.5f, 0.0f, 5.0f, false}, &meleeCooldownSeconds_},
        {{"melee.heavy_hold_seconds", 0.45f, 0.05f, 2.0f, false}, &meleeHeavyHoldSeconds_},
        {{"vehicle.steering", 1.0f, 0.0f, 1.0f, true}, &vehicleSteering_},
        {{"vehicle.wheel_max_degrees", 90.0f, 10.0f, 180.0f, false}, &vehicleWheelMaxDegrees_},
        {{"vehicle.deadzone", 0.05f, 0.0f, 0.9f, false}, &vehicleDeadzone_},
        {{"vehicle.invert", 0.0f, 0.0f, 1.0f, true}, &vehicleInvert_},
        {{"vehicle.require_grip", 1.0f, 0.0f, 1.0f, true}, &vehicleRequireGrip_},
        {{"stance.physical", 0.0f, 0.0f, 1.0f, true}, &stancePhysical_},
        {{"stance.crouch_drop", 0.35f, 0.05f, 1.5f, false}, &stanceCrouchDrop_},
        {{"stance.prone_drop", 0.85f, 0.1f, 2.0f, false}, &stanceProneDrop_},
        {{"stance.hysteresis", 0.08f, 0.0f, 0.5f, false}, &stanceHysteresis_},
        {{"haptics.fire", 1.0f, 0.0f, 1.0f, true}, &hapticsFire_},
        {{"haptics.fire_seconds", 0.08f, 0.01f, 1.0f, false}, &hapticsFireSeconds_},
        {{"haptics.fire_amplitude", 0.8f, 0.0f, 1.0f, false}, &hapticsFireAmplitude_},
    };
    static_assert(std::size(rows) == std::tuple_size<decltype(hostTunables_)>::value,
        "hostTunables_ must hold exactly one entry per row");
    std::size_t index = 0;
    for (const Row& row : rows)
    {
        // hud.ammo_quad also decides whether the swapchain exists (needs a restart);
        // the ini value was read into ammoQuadEnabled_ already, keep them in step.
        if (std::strcmp(row.spec.name, "hud.ammo_quad") != 0)
            row.value->store(ReadTunable(row.spec, row.value->load()), std::memory_order_relaxed);
        hostTunables_[index++] = {row.spec.name, row.value, row.spec.minimum, row.spec.maximum};
    }
    dayz::runtime_probe::RegisterTunables(hostTunables_.data(), hostTunables_.size());
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
            thumbstickClickAction_) ||
        !createAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, hapticAction_))
        return false;

    const auto path = [&](const char* text) {
        XrPath result{XR_NULL_PATH};
        xrStringToPath(instance_, text, &result);
        return result;
    };
    const auto suggest = [&](const char* profile,
        std::vector<XrActionSuggestedBinding> bindings) {
        // Every supported profile exposes output/haptic on both hands.
        bindings.push_back({hapticAction_, path("/user/hand/left/output/haptic")});
        bindings.push_back({hapticAction_, path("/user/hand/right/output/haptic")});
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


// ---- axis/ammo swapchain creation (openxr_host.cpp) ----
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

// ---- ammo and axis swapchains (openxr_host.cpp) ----
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


// ---- FinishInitialization controls reads (openxr_host.cpp) ----
    controllerInputEnabled_ = ReadBoolean(L"controls", L"enabled", true);
    controllerAxesEnabled_ = ReadBoolean(L"controls", L"show_controller_axes", true);
    ammoQuadEnabled_ = ReadBoolean(L"hud", L"ammo_quad", true);
    ammoQuadVisible_ = ammoQuadEnabled_ ? 1.0f : 0.0f;
    ammoQuadPixelHeight_ = (std::clamp)(ReadUnsigned(L"hud", L"ammo_quad_pixel_height", 48), 8u, 256u);
    LoadHostTunables();
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

// ---- FinishInitialization CreateControllerActions (openxr_host.cpp) ----
    if (!CreateControllerActions())
    {
        logging::Error("OpenXR controller actions unavailable; headset rendering will continue");
        controllerInputEnabled_ = false;
        controllerAxesEnabled_ = false;
    }

// ---- PollEvents focus release (openxr_host.cpp) ----
            if (sessionState_ != XR_SESSION_STATE_FOCUSED)
                ReleaseControllerKeys();

// ---- controller input, melee, stance, steering, haptics (openxr_host.cpp) ----
void OpenXrHost::ReleaseControllerKeys() noexcept
{
    ReleaseInjectedInput();
    dayz::input_hooks::ClearAllActions();
    directActions_ = {};
    for (auto& location : gripLocations_)
        location = MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    for (auto& location : aimLocations_)
        location = MakeXr<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    dayz::stereo_state::UpdateAimOrientation(0.0f, 0.0f, 0.0f, 1.0f, false);
    dayz::script_bridge::SetVehicleSteer(0.0f, false);
    dayz::script_bridge::SetVehiclePedals(0.0f, 0.0f, false);
    dayz::script_bridge::SetControllerButtons({});
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
bool OpenXrHost::PulseHaptic(int hand, float seconds, float amplitude) noexcept
{
    if (hapticAction_ == XR_NULL_HANDLE || hand < 0 || hand >= static_cast<int>(handPaths_.size()) ||
        sessionState_ != XR_SESSION_STATE_FOCUSED)
        return false;
    XrHapticActionInfo info(MakeXr<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO));
    info.action = hapticAction_;
    info.subactionPath = handPaths_[static_cast<std::size_t>(hand)];
    XrHapticVibration vibration(MakeXr<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION));
    vibration.duration = static_cast<XrDuration>((std::clamp)(seconds, 0.01f, 1.0f) * 1.0e9f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = (std::clamp)(amplitude, 0.0f, 1.0f);
    const XrResult result = xrApplyHapticFeedback(session_, &info,
        reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    if (XR_FAILED(result))
    {
        logging::XrError("xrApplyHapticFeedback", result);
        return false;
    }
    ++hapticPulses_;
    return true;
}

bool OpenXrHost::TestHaptic() noexcept
{
    const bool pulsed = PulseHaptic(1, 0.25f, 1.0f);
    logging::Info(pulsed ? "Haptic test pulse sent to the right controller"
                         : "Haptic test pulse failed (no haptic action or session not focused)");
    return pulsed;
}

void OpenXrHost::UpdateFireHaptics() noexcept
{
    // Shots are detected from the bridge's ammo readback (magazine + chamber drops by
    // one), so no draw-call heuristic is needed; melee swings do not count.
    const dayz::script_bridge::GameState game = dayz::script_bridge::GetGameState();
    const int shots = shotDetector_.Update(game.valid ? game.frame : -1, game.weapon, game.ammo, game.chamber);
    if (shots <= 0 || hapticsFire_.load(std::memory_order_relaxed) < 0.5f)
        return;
    if (PulseHaptic(1, hapticsFireSeconds_.load(std::memory_order_relaxed),
            hapticsFireAmplitude_.load(std::memory_order_relaxed)))
    {
        std::ostringstream message;
        message << "haptic pulse: shot from " << game.weapon << " (" << game.ammo
                << (game.chamber ? "+1" : "") << " left), pulses=" << hapticPulses_;
        logging::Info(message.str());
    }
}

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

void OpenXrHost::WriteDirectAction(std::size_t slot, const char* name, float value, bool held)
{
    // Only changes reach the table: a value another producer set (the debug plugin's
    // "action" command, a future gesture) survives until the controller moves, and a
    // released action is cleared so the engine sees exactly one release edge.
    DirectActionState& state = directActions_[slot];
    const bool active = held || value > 0.0f;
    if (!active)
    {
        if (state.written)
            dayz::input_hooks::ClearAction(name);
        state = {};
        return;
    }
    if (state.written && state.value == value && state.held == held)
        return;
    dayz::input_hooks::SetAction(name, value, held);
    state.value = value;
    state.held = held;
    state.written = true;
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
    // Direct input: with the engine action hooks active (dayz_input_hooks) gameplay
    // input is written as named actions and does not need the desktop foreground;
    // only the SendInput leftovers (menu keys, GUI clicks) still require it.
    const bool direct = dayz::input_hooks::Active();
    if (!injectInput)
    {
        // Poses above stay live (ammo quad, rays, debug snapshot); nothing may reach
        // the game through SendInput while its window is not the desktop foreground.
        ReleaseInjectedInput();
        if (!direct)
            return;
    }
    const bool keysAllowed = injectInput;
    if (direct && guiVisible)
    {
        dayz::input_hooks::ClearAllActions();
        directActions_ = {};
    }
    const bool actions = direct && !guiVisible;
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
    if (actions)
    {
        // Analogue movement: the stick deflection beyond the deadzone becomes the
        // action value (DayZ walks below its run threshold), one action per direction.
        const auto axis = [&](float value) {
            const float span = (std::max)(0.01f, 1.0f - controllerDeadzone_);
            return (std::min)(1.0f, (std::max)(0.0f, std::fabs(value) - controllerDeadzone_) / span);
        };
        const float forward = desired[0] ? axis(leftStick.y) : 0.0f;
        const float left = desired[1] ? axis(leftStick.x) : 0.0f;
        const float back = desired[2] ? axis(leftStick.y) : 0.0f;
        const float right = desired[3] ? axis(leftStick.x) : 0.0f;
        WriteDirectAction(0, "UAMoveForward", forward, desired[0]);
        WriteDirectAction(1, "UAMoveLeft", left, desired[1]);
        WriteDirectAction(2, "UAMoveBack", back, desired[2]);
        WriteDirectAction(3, "UAMoveRight", right, desired[3]);
    }
    else if (keysAllowed)
    {
        constexpr WORD keys[4]{'W', 'A', 'S', 'D'};
        for (std::size_t index = 0; index < movementKeys_.size(); ++index)
            if (movementKeys_[index] != desired[index])
            {
                SendKey(keys[index], desired[index]);
                movementKeys_[index] = desired[index];
            }
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
    else if (direct)
    {
        // Stick turn as an aim change through the engine's own axis getter: an exact
        // angle per frame, no mouse counts, no desktop focus. Positive yaw turns
        // right. Independent of direct head aim ([input] direct_aim), which only
        // decides how the HMD rotation reaches the game.
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        if (actions)
        {
            if (controllerSnapTurn_ > 0.0f)
            {
                if (turning && snapTurnArmed_)
                {
                    dayz::input_hooks::AddAimDelta(
                        (rightStick.x > 0.0f ? 1.0f : -1.0f) * controllerSnapTurn_ * kDegToRad, 0.0f);
                    snapTurnArmed_ = false;
                }
                else if (!turning)
                    snapTurnArmed_ = true;
            }
            else if (turning)
                dayz::input_hooks::AddAimDelta(rightStick.x * controllerTurnRate_ * kDegToRad * inputSeconds, 0.0f);
        }
    }
    else if (turning && keysAllowed)
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
    {
        // Raw state for Enforce features (vr.txt): buttons as pressed flags, analogue
        // inputs as values. The right stick click is read here only for the bridge.
        const XrActionStateBoolean rightClick = booleanState(thumbstickClickAction_, 1);
        dayz::script_bridge::Buttons buttons;
        buttons.x = xDown;
        buttons.y = yDown;
        buttons.a = aState.isActive && aState.currentState;
        buttons.b = bState.isActive && bState.currentState;
        buttons.stickClickLeft = leftStickClickDown_;
        buttons.stickClickRight = rightClick.isActive && rightClick.currentState;
        buttons.grabLeft = leftGrabState.isActive ? leftGrabState.currentState : 0.0f;
        buttons.grabRight = rightGrabState.isActive ? rightGrabState.currentState : 0.0f;
        buttons.triggerLeft = leftTriggerState.isActive ? leftTriggerState.currentState : 0.0f;
        buttons.triggerRight = rightTriggerState.isActive ? rightTriggerState.currentState : 0.0f;
        dayz::script_bridge::SetControllerButtons(buttons);
    }
    const bool leftGrabDown = leftGrabState.isActive && leftGrabState.currentState > 0.55f;
    const bool rightGrabDown = rightGrabState.isActive && rightGrabState.currentState > 0.55f;
    const bool aDown = aState.isActive && aState.currentState;
    const bool bDown = bState.isActive && bState.currentState;
    const auto updateKey = [&](WORD key, bool desired, bool& current,
        const char* downMessage, const char* upMessage) {
        if (!keysAllowed)
            desired = false;
        if (desired == current)
            return;
        SendKey(key, desired);
        logging::Info(desired ? downMessage : upMessage);
        current = desired;
    };
    // Gameplay buttons: the named engine action when the hooks are active (set every
    // frame, the hooks latch it), the emulated key otherwise.
    const auto updateAction = [&](const char* action, WORD key, bool desired, bool& current,
        const char* downMessage, const char* upMessage) {
        if (!direct)
        {
            updateKey(key, desired, current, downMessage, upMessage);
            return;
        }
        if (current)
        {
            SendKey(key, false);
            current = false;
        }
        if (actions)
            dayz::input_hooks::SetAction(action, desired ? 1.0f : 0.0f, desired);
    };
    updateAction("UAStance", 'C', xDown && !leftGrabDown, xKeyDown_,
        "controller X -> C down", "controller X -> C up");
    updateKey(VK_ESCAPE, xDown && leftGrabDown, escapeKeyDown_,
        "controller LGRAB+X -> Escape down", "controller LGRAB+X -> Escape up");
    updateAction("UAReloadMagazine", 'R', yDown && !leftGrabDown, yKeyDown_,
        "controller Y -> R down", "controller Y -> R up");
    updateKey(VK_TAB, yDown && leftGrabDown, tabKeyDown_,
        "controller LGRAB+Y -> Tab down", "controller LGRAB+Y -> Tab up");
    // Not while driving: the two-hand wheel needs both grips held, and a held F is
    // "Get out" once the vehicle is set up (headset test: grip ejected the driver).
    updateAction("UADefaultAction", 'F', rightGrabDown && !driving, rightGrabDown_,
        "controller RGRAB -> F down", "controller RGRAB -> F up");
    // While driving, A (engine), LGRAB+A (headlights) and the right stick click (horn)
    // belong to the @DayZVR mod, which reads them from vr.txt; B stays the handbrake.
    updateAction("UATurbo", VK_SHIFT, aDown && !leftGrabDown && !driving, aButtonDown_,
        "controller A -> Shift down", "controller A -> Shift up");
    updateAction("UAGetOver", VK_SPACE, bDown && !leftGrabDown, bButtonDown_,
        "controller B -> Space down", "controller B -> Space up");
    const auto hotbarVirtualKey = [](unsigned slot) -> WORD {
        return slot == 10 ? '0' : static_cast<WORD>('0' + slot);
    };
    // Quickbar slots 1..10 are the engine actions UAItem0..UAItem9.
    const auto hotbarAction = [](unsigned slot) -> const char* {
        static constexpr const char* kNames[10]{"UAItem0", "UAItem1", "UAItem2", "UAItem3",
            "UAItem4", "UAItem5", "UAItem6", "UAItem7", "UAItem8", "UAItem9"};
        return kNames[(slot + 9) % 10];
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
            if (direct)
                dayz::input_hooks::SetAction(hotbarAction(hotbarSlot_), 1.0f, true);
            else
                SendKey(activeKey, true);
            std::ostringstream message;
            message << name << " -> hotbar slot " << hotbarSlot_ << " down";
            logging::Info(message.str());
        }
        else
        {
            if (direct)
                dayz::input_hooks::ClearAction(hotbarAction(hotbarSlot_));
            else
                SendKey(activeKey, false);
            std::ostringstream message;
            message << name << " -> hotbar slot " << hotbarSlot_ << " up";
            logging::Info(message.str());
        }
        current = desired;
    };
    const bool hotbarAllowed = direct ? actions : keysAllowed;
    updateHotbar(hotbarAllowed && leftGrabDown && aDown && !driving, hotbarPreviousDown_, hotbarPreviousKey_, -1,
        "controller LGRAB+A");
    updateHotbar(hotbarAllowed && leftGrabDown && bDown && !driving, hotbarNextDown_, hotbarNextKey_, 1,
        "controller LGRAB+B");

    const bool meleeHeld = UpdateMotionMelee(displayTime, inputSeconds, guiVisible);
    if (!guiVisible)
        UpdatePhysicalStance(displayTime);
    UpdateFireHaptics();
    const bool desiredLeftMouse = meleeHeld || (!driving && rightTriggerState.isActive &&
        rightTriggerState.currentState > 0.55f);
    const bool desiredRightMouse = !driving && leftTriggerState.isActive &&
        leftTriggerState.currentState > 0.55f;
    // A menu the proxy did not capture (the death screen, server browser) renders in the
    // world and owns the engine's game focus; UAFire does nothing there, so the trigger
    // falls back to a real click while the desktop foreground allows it.
    const bool menuClick = keysAllowed && dayz::input_hooks::MenuOwnsInput();
    if (direct && !menuClick)
    {
        // Fire and raise as engine actions (UAFire = attack, UATempRaiseWeapon = the
        // right mouse button's raise); any emulated mouse button still down goes up.
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
        if (actions)
        {
            WriteDirectAction(4, "UAFire", desiredLeftMouse ? 1.0f : 0.0f, desiredLeftMouse);
            WriteDirectAction(5, "UATempRaiseWeapon", desiredRightMouse ? 1.0f : 0.0f, desiredRightMouse);
        }
    }
    else if (keysAllowed)
    {
        if (desiredLeftMouse != leftMouseDown_)
        {
            SendMouseButton(false, desiredLeftMouse);
            leftMouseDown_ = desiredLeftMouse;
        }
        if (desiredRightMouse != rightMouseDown_)
        {
            SendMouseButton(true, desiredRightMouse);
            rightMouseDown_ = desiredRightMouse;
        }
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


// ---- RenderFrame input sync (openxr_host.cpp) ----
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

// ---- RenderFrame axis/ray layers (openxr_host.cpp) ----
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

// ---- RenderFrame axis/ammo layer submit (openxr_host.cpp) ----
    for (std::uint32_t index = 0; layerCount && index < axisLayerCount && layerCount < layers.size(); ++index)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &axisLayers[index]);
    XrCompositionLayerQuad ammoLayer{};
    if (layerCount && !guiVisible && layerCount < layers.size() && PrepareAmmoLayer(ammoLayer))
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ammoLayer);

// ---- Shutdown controller resources (openxr_host.cpp) ----
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
