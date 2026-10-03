// Parked: the HMD-to-game aim path (synthetic mouse, closed loop, direct aim through
// the input hooks), axis locks, vehicle view lock, hotkey and script-bridge calls cut
// out of common/dayz_runtime_probe.cpp. Not compiled; an excerpt, not a standalone
// unit. The full file before the cut is in git history.
#include "dayz_input_hooks.hpp"
#include "hmd_aim_loop.hpp"
#include "script_bridge.hpp"
#include "dayz_hotkeys.hpp"

// ---- aim globals (dayz_runtime_probe.cpp) ----
    bool g_hmdNativeAimEnabled{true};
    float g_hmdMouseYawScale{-600.0f};
    float g_hmdMousePitchScale{-600.0f};
    // Locked axes never reach DayZ's mouse camera; the render camera still
    // follows the HMD on them so the view moves while the aim stays put.
    bool g_lockHmdYaw{};
    bool g_lockHmdPitch{};
    // [vehicle] lock_view: while the bridge reports the player in a vehicle both axes
    // behave as locked, so the head looks around the cabin without the aim loop
    // dragging DayZ's (vehicle-relative) camera after a world-space HMD direction.
    bool g_vehicleLockView{true};
    // [stereo] aim_residual_render: the closed loop can only move DayZ's camera a frame
    // late and in whole mouse counts, so the world image would lag the head by the
    // loop's remaining error while the game is focused (seen as the world, and so the
    // hands drawn in head space, shaking). Rendering the residual on the render side
    // keeps the eyes exactly on the head while DayZ's aim catches up underneath.
    bool g_aimResidualRender{true};

// ---- aim loop state (dayz_runtime_probe.cpp) ----
    bool VehicleViewLocked() noexcept
    {
        return g_vehicleLockView && dayz::script_bridge::GetGameState().inVehicle;
    }
    bool YawLocked() noexcept { return g_lockHmdYaw || VehicleViewLocked(); }
    bool PitchLocked() noexcept { return g_lockHmdPitch || VehicleViewLocked(); }
    // Closed-loop head aim (see hmd_aim_loop.hpp). The camera yaw captured at
    // recenter plays the role the HMD centre plays for the render camera.
    bool g_hmdAimClosedLoop{true};
    // Aim DayZ's camera with the right controller instead of the head; the
    // rendered view then shows the head direction (decoupled aim).
    bool g_controllerAim{};
    bool g_controllerAimActive{};
    float g_aimLoopDamping{0.5f};
    float g_aimLoopMaxCounts{400.0f};
    dayz::aim_loop::State g_aimLoop{};
    float g_aimCameraYawCenter{};
    float g_aimHmdYawCenter{};
    bool g_haveAimCenter{};
    bool g_aimYawWasLocked{};
    float g_aimYawError{};
    std::atomic<float> g_aimYawOffset{0.0f};
    float g_aimPitchError{};
    bool g_haveNativeHmdAngles{};
    float g_previousHmdYaw{};
    float g_previousHmdPitch{};
    double g_pendingMouseX{};
    double g_pendingMouseY{};

// ---- RenderOnlyRotation (dayz_runtime_probe.cpp) ----
    // Rebuilds the HMD rotation from the yaw/pitch/roll decomposition used by
    // UpdateNativeHmdAim, keeping roll and only the axes DayZ does not receive.
    // extraYaw/extraPitch (radians, same convention as the HMD decomposition) are added
    // on an axis that is not kept: the closed loop's residual error.
    Quaternion RenderOnlyRotation(const Quaternion& relative, bool keepYaw,
        bool keepPitch, float extraYaw = 0.0f, float extraPitch = 0.0f) noexcept
    {
        const float yaw = keepYaw ? std::atan2(
            2.0f * (relative.w * relative.y + relative.x * relative.z),
            1.0f - 2.0f * (relative.x * relative.x + relative.y * relative.y)) : extraYaw;
        const float pitch = keepPitch ? std::asin((std::clamp)(
            2.0f * (relative.w * relative.x - relative.z * relative.y), -1.0f, 1.0f)) : extraPitch;
        const float roll = std::atan2(
            2.0f * (relative.w * relative.z + relative.x * relative.y),
            1.0f - 2.0f * (relative.x * relative.x + relative.z * relative.z));
        const Quaternion yawRotation{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        const Quaternion pitchRotation{std::sin(pitch * 0.5f), 0.0f, 0.0f,
            std::cos(pitch * 0.5f)};
        const Quaternion rollRotation{0.0f, 0.0f, std::sin(roll * 0.5f),
            std::cos(roll * 0.5f)};
        return Normalize(Multiply(Multiply(yawRotation, pitchRotation), rollRotation));
    }


// ---- ApplyHmdRotationToCamera native-aim branch (dayz_runtime_probe.cpp) ----
        else if (g_hmdNativeAimEnabled && !guiLook)
        {
            // DayZ receives HMD yaw/pitch through its native mouse path so all
            // gameplay systems share them. Only roll remains a render-space
            // transform because a conventional mouse camera has no roll axis.
            // A locked axis stays on the render side instead, so the head still
            // looks around that axis while DayZ's aim direction ignores it.
            const bool residual = g_aimResidualRender && g_hmdAimClosedLoop && g_haveAimCenter &&
                !g_controllerAimActive;
            renderRotation = RenderOnlyRotation(relative, YawLocked(), PitchLocked(),
                residual ? g_aimYawError : 0.0f, residual ? g_aimPitchError : 0.0f);
            if (g_controllerAimActive && g_haveAimCenter)
            {
                // The game camera follows the controller; undo its rotation
                // relative to the centre so the eyes still see the head direction.
                const Vec3 native = DirectionToOpenXr(g_baseCameraBasis.forward);
                const float cameraYaw = dayz::aim_loop::WrapAngle(
                    std::atan2(-native.x, -native.z) - g_aimCameraYawCenter);
                const float cameraPitch = std::asin((std::clamp)(native.y, -1.0f, 1.0f));
                const Quaternion yawRotation{0.0f, std::sin(cameraYaw * 0.5f), 0.0f,
                    std::cos(cameraYaw * 0.5f)};
                const Quaternion pitchRotation{std::sin(cameraPitch * 0.5f), 0.0f, 0.0f,
                    std::cos(cameraPitch * 0.5f)};
                const Quaternion cameraRotation = Normalize(Multiply(yawRotation, pitchRotation));
                const Quaternion inverseCamera{-cameraRotation.x, -cameraRotation.y,
                    -cameraRotation.z, cameraRotation.w};
                renderRotation = Normalize(Multiply(inverseCamera, relative));
            }
        }

// ---- UpdateClosedLoopAim / UpdateNativeHmdAim (dayz_runtime_probe.cpp) ----
    void UpdateClosedLoopAim(float hmdYaw, float hmdPitch) noexcept
    {
        const dayz::stereo_state::CameraDirections directions =
            dayz::stereo_state::GetCameraDirections();
        if (!directions.valid)
            return;
        // OpenXR space: forward is -Z, yaw is positive to the left, same as the
        // HMD decomposition above, so the two angles compare directly.
        const float cameraYaw = std::atan2(-directions.nativeX, -directions.nativeZ);
        const float cameraPitch = std::asin((std::clamp)(directions.nativeY, -1.0f, 1.0f));
        const bool lockYaw = YawLocked();
        const bool lockPitch = PitchLocked();
        if (!g_haveAimCenter || (g_aimYawWasLocked && !lockYaw))
        {
            g_aimCameraYawCenter = cameraYaw;
            g_aimHmdYawCenter = hmdYaw;
            g_haveAimCenter = true;
            dayz::aim_loop::Config config{};
            config.yawCountsPerRadian = g_hmdMouseYawScale;
            config.pitchCountsPerRadian = g_hmdMousePitchScale;
            dayz::aim_loop::Reset(g_aimLoop, config);
        }
        g_aimYawWasLocked = lockYaw;
        float targetYaw = hmdYaw;
        float targetPitch = hmdPitch;
        bool controllerActive = false;
        if (g_controllerAim)
        {
            const dayz::stereo_state::HmdOrientation aim = dayz::stereo_state::GetAimOrientation();
            if (aim.valid)
            {
                const Quaternion q = Normalize({aim.x, aim.y, aim.z, aim.w});
                targetYaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                targetPitch = std::asin((std::clamp)(2.0f * (q.w * q.x - q.z * q.y), -1.0f, 1.0f));
                controllerActive = true;
            }
        }
        g_controllerAimActive = controllerActive;
        dayz::aim_loop::Config config{};
        config.yawCountsPerRadian = g_hmdMouseYawScale;
        config.pitchCountsPerRadian = g_hmdMousePitchScale;
        config.damping = g_aimLoopDamping;
        config.maxCountsPerFrame = g_aimLoopMaxCounts;
        const float desiredYaw = g_aimCameraYawCenter +
            dayz::aim_loop::WrapAngle(targetYaw - g_aimHmdYawCenter) +
            g_aimYawOffset.load(std::memory_order_relaxed);
        const dayz::aim_loop::Output out = dayz::aim_loop::Step(g_aimLoop, config,
            lockYaw ? cameraYaw : desiredYaw, lockPitch ? cameraPitch : targetPitch,
            cameraYaw, cameraPitch);
        g_aimYawError = out.yawError;
        g_aimPitchError = out.pitchError;
        if (!lockYaw)
            g_pendingMouseX += out.yawCounts;
        if (!lockPitch)
            g_pendingMouseY += out.pitchCounts;
    }

    void UpdateNativeHmdAim() noexcept
    {
        if (!g_hmdRotationEnabled || !g_hmdNativeAimEnabled)
        {
            dayz::aim_loop::Suspend(g_aimLoop, g_pendingMouseX, g_pendingMouseY);
            return;
        }
        const dayz::stereo_state::HmdOrientation orientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!orientation.valid)
        {
            dayz::aim_loop::Suspend(g_aimLoop, g_pendingMouseX, g_pendingMouseY);
            return;
        }
        const Quaternion current = Normalize({orientation.x, orientation.y, orientation.z,
            orientation.w});
        const float yaw = std::atan2(
            2.0f * (current.w * current.y + current.x * current.z),
            1.0f - 2.0f * (current.x * current.x + current.y * current.y));
        const float pitch = std::asin((std::clamp)(
            2.0f * (current.w * current.x - current.z * current.y), -1.0f, 1.0f));
        if (!g_haveNativeHmdAngles)
        {
            g_previousHmdYaw = yaw;
            g_previousHmdPitch = pitch;
            g_haveNativeHmdAngles = true;
            return;
        }

        const float yawDelta = (std::clamp)(
            std::remainder(yaw - g_previousHmdYaw, 2.0f * 3.14159265358979323846f),
            -0.35f, 0.35f);
        const float pitchDelta = (std::clamp)(pitch - g_previousHmdPitch, -0.35f, 0.35f);
        g_previousHmdYaw = yaw;
        g_previousHmdPitch = pitch;

        if (dayz::input_hooks::DirectAimEnabled())
        {
            // Head aim through the engine's aim axis ([input] direct_aim): the HMD
            // delta of this frame is handed to the action hooks, which turn it into
            // the exact per-frame aim change. No mouse counts, no closed loop, no
            // foreground requirement; menus still suspend it.
            dayz::aim_loop::Suspend(g_aimLoop, g_pendingMouseX, g_pendingMouseY);
            g_controllerAimActive = false;
            g_aimYawError = 0.0f;
            g_aimPitchError = 0.0f;
            if (g_gameWindow && !RawGuiCursorModeActive())
                dayz::input_hooks::AddAimDelta(YawLocked() ? 0.0f : yawDelta,
                    PitchLocked() ? 0.0f : pitchDelta);
            return;
        }
        // Use the raw cursor state: debounce is useful for the quad, but the
        // first inventory frame already stops DayZ consuming gameplay input.
        if (!g_gameWindow || RealForegroundWindowImpl() != g_gameWindow ||
            RawGuiCursorModeActive())
        {
            dayz::aim_loop::Suspend(g_aimLoop, g_pendingMouseX, g_pendingMouseY);
            g_controllerAimActive = false;
            g_aimYawError = 0.0f;
            g_aimPitchError = 0.0f;
            return;
        }

        if (g_hmdAimClosedLoop)
            UpdateClosedLoopAim(yaw, pitch);
        else
        {
            if (!YawLocked())
                g_pendingMouseX += static_cast<double>(yawDelta) * g_hmdMouseYawScale;
            if (!PitchLocked())
                g_pendingMouseY += static_cast<double>(pitchDelta) * g_hmdMousePitchScale;
        }
        const LONG mouseX = static_cast<LONG>(std::trunc(g_pendingMouseX));
        const LONG mouseY = static_cast<LONG>(std::trunc(g_pendingMouseY));
        g_pendingMouseX -= mouseX;
        g_pendingMouseY -= mouseY;
        if (!mouseX && !mouseY)
            return;

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = mouseX;
        input.mi.dy = mouseY;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        if (SendInput(1, &input, sizeof(input)) != 1)
            dayz::aim_loop::Suspend(g_aimLoop, g_pendingMouseX, g_pendingMouseY);
    }


// ---- crash context aim fields (dayz_runtime_probe.cpp) ----
            << " native_aim=" << g_hmdNativeAimEnabled << " pending_mouse=" << g_pendingMouseX
            << ',' << g_pendingMouseY << " keep_focus=" << g_keepFocusEnabled
        dayz::hotkeys::Initialize(ConfigurationFile().c_str());
        dayz::script_bridge::Initialize(ConfigurationFile().c_str());

// ---- Initialize aim ini reads (dayz_runtime_probe.cpp) ----
        g_hmdNativeAimEnabled = ReadBoolean(L"stereo", L"hmd_native_aim", true);
        g_hmdMouseYawScale = ReadFloat(L"stereo", L"hmd_mouse_yaw_scale", -600.0f);
        g_hmdMousePitchScale = ReadFloat(L"stereo", L"hmd_mouse_pitch_scale", -600.0f);
        g_lockHmdYaw = ReadBoolean(L"stereo", L"lock_yaw", false);
        g_lockHmdPitch = ReadBoolean(L"stereo", L"lock_pitch", false);
        g_vehicleLockView = ReadBoolean(L"vehicle", L"lock_view", true);
        g_aimResidualRender = ReadBoolean(L"stereo", L"aim_residual_render", true);

// ---- Initialize closed-loop ini reads (dayz_runtime_probe.cpp) ----
        g_hmdAimClosedLoop = ReadBoolean(L"stereo", L"hmd_aim_closed_loop", true);
        g_controllerAim = ReadBoolean(L"stereo", L"controller_aim", false);
        g_aimLoopDamping = ReadFloat(L"stereo", L"hmd_aim_loop_damping", 0.5f);
        g_aimLoopMaxCounts = ReadFloat(L"stereo", L"hmd_aim_loop_max_counts", 400.0f);
        dayz::input_hooks::Initialize(ConfigurationFile().c_str(), g_moduleBase, kImageSize);

// ---- startup log native aim (dayz_runtime_probe.cpp) ----
                << " hmd_native_aim=" << g_hmdNativeAimEnabled

// ---- startup log aim fields (dayz_runtime_probe.cpp) ----
                << " hmd_mouse_scale=" << g_hmdMouseYawScale << ','
                << g_hmdMousePitchScale
                << " lock_yaw=" << g_lockHmdYaw << " lock_pitch=" << g_lockHmdPitch << " vehicle_lock_view=" << g_vehicleLockView
                << " closed_loop=" << g_hmdAimClosedLoop
        dayz::hotkeys::Poll();
        dayz::script_bridge::Update();
        UpdateNativeHmdAim();

// ---- debug snapshot aim fields (dayz_runtime_probe.cpp) ----
        snapshot.pendingMouseX = g_pendingMouseX;
        snapshot.pendingMouseY = g_pendingMouseY;
        if (g_hmdAimClosedLoop)
        {
            snapshot.aimYawError = g_aimYawError;
            snapshot.aimPitchError = g_aimPitchError;
            snapshot.aimYawGain = g_aimLoop.yaw.countsPerRadian;
            snapshot.aimPitchGain = g_aimLoop.pitch.countsPerRadian;
        }
            {"stereo.hmd_mouse_yaw_scale", TunableKind::Float, &g_hmdMouseYawScale, -5000.0f, 5000.0f},
            {"stereo.hmd_mouse_pitch_scale", TunableKind::Float, &g_hmdMousePitchScale, -5000.0f, 5000.0f},
            {"stereo.hmd_native_aim", TunableKind::Bool, &g_hmdNativeAimEnabled, 0.0f, 1.0f},
            {"stereo.lock_yaw", TunableKind::Bool, &g_lockHmdYaw, 0.0f, 1.0f},
            {"stereo.lock_pitch", TunableKind::Bool, &g_lockHmdPitch, 0.0f, 1.0f},
            {"vehicle.lock_view", TunableKind::Bool, &g_vehicleLockView, 0.0f, 1.0f},
            {"stereo.aim_residual_render", TunableKind::Bool, &g_aimResidualRender, 0.0f, 1.0f},
            {"stereo.hmd_aim_closed_loop", TunableKind::Bool, &g_hmdAimClosedLoop, 0.0f, 1.0f},
            {"stereo.controller_aim", TunableKind::Bool, &g_controllerAim, 0.0f, 1.0f},
            {"stereo.hmd_aim_loop_damping", TunableKind::Float, &g_aimLoopDamping, 0.05f, 1.0f},
            {"stereo.hmd_aim_loop_max_counts", TunableKind::Float, &g_aimLoopMaxCounts, 1.0f, 5000.0f},

// ---- ClosedLoopAimActive / AddAimYawOffset (dayz_runtime_probe.cpp) ----
    bool ClosedLoopAimActive() noexcept
    {
        return g_hmdRotationEnabled && g_hmdNativeAimEnabled && g_hmdAimClosedLoop;
    }

    void AddAimYawOffset(float radians) noexcept
    {
        float current = g_aimYawOffset.load(std::memory_order_relaxed);
        while (!g_aimYawOffset.compare_exchange_weak(current,
            dayz::aim_loop::WrapAngle(current + radians), std::memory_order_relaxed))
        {
        }
    }

        g_haveNativeHmdAngles = false;
        g_haveAimCenter = false;
        g_aimYawOffset.store(0.0f, std::memory_order_relaxed);
        g_pendingMouseX = 0.0;
        g_pendingMouseY = 0.0;
