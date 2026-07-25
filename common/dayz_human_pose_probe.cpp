#include "dayz_human_pose_probe.hpp"

#include "dayz_offsets.generated.hpp"
#include "logging.hpp"
#include "stereo_state.hpp"

#include <vr_pose_solver/arm_solver.hpp>

#include <MinHook.h>
#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace
{
    std::ptrdiff_t kProviderBoneMapOffset{};
    std::ptrdiff_t kProviderParentMapOffset{};
    std::ptrdiff_t kProviderBoneCountOffset{};
    std::ptrdiff_t kProviderExtraBoneCountOffset{};
    std::ptrdiff_t kProviderModelTransformsOffset{};
    std::ptrdiff_t kProviderSkinningTransformsOffset{};
    std::ptrdiff_t kProviderPoseDirtyOffset{};
    constexpr unsigned kMaximumPublicBoneIndex = 249;
    constexpr unsigned kRightArmBone = 113;
    constexpr unsigned kRightForeArmBone = 117;
    constexpr unsigned kRightHandBone = 122;
    constexpr unsigned kRightHandDummyBone = 123;
    constexpr unsigned kLeftArmBone = 67;
    constexpr unsigned kLeftForeArmBone = 71;
    constexpr unsigned kLeftHandBone = 76;
    constexpr unsigned kLeftHandDummyBone = 77;
    constexpr unsigned kSpine3Bone = 14;
    constexpr unsigned kHeadBone = 17;

    enum class ProbeMode { translation, rightArmIk };

    using SkinningExportFn = std::int64_t(__fastcall*)(void*, std::int64_t*,
        std::int64_t, std::int64_t);
    using HumanAnimationUpdateFn = void(__fastcall*)(void*, float, std::int64_t,
        bool, bool);
    using FinalPlayerSimulationFn = void(__fastcall*)(void*, float);
    using SetEntityTransformFn = char(__fastcall*)(void*, const float*);
    using EntityAttachmentTransformFn = float*(__fastcall*)(void*, float*,
        std::int64_t, void*);
    using PlayerProxyTransformFn = std::int64_t(__fastcall*)(void*, std::int64_t,
        std::int64_t, std::int64_t*, unsigned, float*, float*, unsigned);
    using PlayerProxyResolverFn = void(__fastcall*)(void*, std::int64_t,
        std::int64_t*, bool);
    using PlayerProxyLocalTransformFn = float*(__fastcall*)(void*, float*,
        std::int64_t, float*, unsigned);
    using GetLocalTransformFn = void(__fastcall*)(void*, unsigned, float*);
    using SetLocalTransformFn = void(__fastcall*)(void*, unsigned, const float*);
    using RebuildModelTransformsFn = void(__fastcall*)(void*);

    std::uintptr_t g_moduleBase{};
    const dayz::offsets::BuildProfile* g_buildProfile{};
    SkinningExportFn g_skinningExport{};
    HumanAnimationUpdateFn g_humanAnimationUpdate{};
    FinalPlayerSimulationFn g_finalPlayerSimulation{};
    SetEntityTransformFn g_setEntityTransform{};
    EntityAttachmentTransformFn g_entityAttachmentTransform{};
    PlayerProxyTransformFn g_playerProxyTransform{};
    PlayerProxyResolverFn g_playerProxyResolver{};
    PlayerProxyLocalTransformFn g_playerProxyLocalTransform{};
    std::atomic_bool g_active{};
    std::atomic_bool g_loggedFirstWrite{};
    std::atomic_bool g_loggedFirstRebuild{};
    std::atomic_bool g_loggedExportMapping{};
    std::atomic_bool g_loggedSimulationStageWrite{};
    std::atomic_bool g_loggedHeldItemWorldCorrection{};
    std::atomic_bool g_loggedHeldItemProxyCorrection{};
    std::atomic_uint64_t g_writeCount{};
    unsigned g_boneIndex{117};
    float g_translationX{};
    float g_translationY{0.10f};
    float g_translationZ{};
    float g_frequencyHz{0.75f};
    ProbeMode g_mode{ProbeMode::translation};
    float g_ikTargetScale{1.0f};
    float g_ikWeight{1.0f};
    vr::pose::Vec3 g_ikTargetOffset{};
    vr::pose::Vec3 g_gripToWristOffset{};
    vr::pose::Quaternion g_gripRotationOffset{};
    std::atomic<std::uintptr_t> g_ikProvider{};
    std::atomic_uint64_t g_ikProviderLastSeenMs{};
    std::atomic_flag g_wristCalibrationLock = ATOMIC_FLAG_INIT;
    std::array<std::uintptr_t, 2> g_wristCalibrationProviders{};
    std::array<vr::pose::Vec3, 2> g_controllerLocalPoleDirections{{
        {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}}};
    std::array<std::atomic<std::uintptr_t>, 2> g_loggedIkProviders{};

    struct SimulationSocketPose
    {
        void* player{};
        std::uintptr_t provider{};
        vr::pose::Transform vanilla{};
        vr::pose::Transform corrected{};
        bool valid{};
    };

    struct HeldItemCalibration
    {
        std::uintptr_t item{};
        std::uintptr_t provider{};
        vr::pose::Transform socketToItem{};
        bool forceWorldTransform{};
        bool valid{};
    };

    thread_local void* g_currentSimulationPlayer{};
    thread_local SimulationSocketPose g_simulationSocketPose{};
    thread_local HeldItemCalibration g_heldItemCalibration{};
    thread_local std::uintptr_t g_loggedAttachmentAttemptProvider{};
    thread_local void* g_loggedMissingSocketPlayer{};
    std::mutex g_latestSocketPoseMutex;
    SimulationSocketPose g_latestSocketPose{};
    std::atomic<std::uintptr_t> g_loggedProxyItem{};
    std::atomic_uint g_attachmentHookCalls{};
    std::atomic_uint g_nearbyAttachmentLogs{};
    thread_local void* g_lastNearbyAttachmentParent{};
    thread_local void* g_lastNearbyAttachmentChild{};
    std::atomic_uint g_playerProxyCalls{};
    std::atomic_uint g_playerProxyCandidateLogs{};
    std::atomic_bool g_loggedPlayerProxyCorrection{};
    thread_local std::uintptr_t g_lastPlayerProxyResult{};
    thread_local unsigned g_lastPlayerProxySelection{UINT32_MAX};
    thread_local unsigned g_lastPlayerProxyIndex{UINT32_MAX};
    std::atomic_uint g_playerProxyResolverLogs{};
    thread_local std::uintptr_t g_lastResolvedProxyPrimary{};
    thread_local std::uintptr_t g_lastResolvedProxySecondary{};
    thread_local void* g_currentResolvedProxyPlayer{};
    thread_local std::uintptr_t g_currentResolvedProxyPrimary{};
    thread_local std::uintptr_t g_currentResolvedProxySecondary{};
    thread_local bool g_currentResolvedProxyRenderPath{};
    std::atomic_uint g_playerProxyLocalTransformLogs{};
    std::atomic_bool g_loggedPlayerProxyLocalCorrection{};
    struct TwoHandCoupling
    {
        vr::pose::Transform rightDummyToLeftHand{};
        vr::pose::Transform leftHandTarget{};
        bool candidate{};
        bool active{};
    };
    thread_local TwoHandCoupling g_twoHandCoupling{};
    std::atomic<std::uintptr_t> g_loggedTwoHandProfile{};

    bool SelectIkProvider(std::uintptr_t provider) noexcept
    {
        constexpr std::uint64_t kProviderSceneTimeoutMs = 1000;
        const std::uint64_t now = GetTickCount64();
        std::uintptr_t selected = g_ikProvider.load(std::memory_order_acquire);
        if (selected == provider)
        {
            g_ikProviderLastSeenMs.store(now, std::memory_order_release);
            return true;
        }
        if (!selected)
        {
            if (g_ikProvider.compare_exchange_strong(selected, provider,
                    std::memory_order_acq_rel))
            {
                g_ikProviderLastSeenMs.store(now, std::memory_order_release);
                logging::Info("Right-arm IK selected its initial pose provider");
                return true;
            }
            return selected == provider;
        }

        const std::uint64_t lastSeen = g_ikProviderLastSeenMs.load(
            std::memory_order_acquire);
        if (now - lastSeen <= kProviderSceneTimeoutMs)
            return false;
        const std::uintptr_t previous = selected;
        if (!g_ikProvider.compare_exchange_strong(selected, provider,
                std::memory_order_acq_rel))
            return selected == provider;
        g_ikProviderLastSeenMs.store(now, std::memory_order_release);
        std::ostringstream message;
        message << "Right-arm IK switched pose provider after scene transition: old="
            << reinterpret_cast<void*>(previous) << " new="
            << reinterpret_cast<void*>(provider);
        logging::Info(message.str());
        return true;
    }

    struct CalibratedHandTarget
    {
        vr::pose::Vec3 position{};
        vr::pose::Quaternion orientation{};
        vr::pose::Vec3 polePosition{};
    };

    CalibratedHandTarget CalibrateHandTarget(std::uintptr_t provider,
        unsigned armSlot,
        const vr::pose::Quaternion& controllerOrientation,
        const vr::pose::Quaternion& desiredHandOrientation,
        const vr::pose::Vec3& shoulderPosition,
        const vr::pose::Vec3& elbowPosition,
        const vr::pose::Vec3& desiredHandPosition,
        const vr::pose::Vec3& anatomicalPoleDirection) noexcept
    {
        while (g_wristCalibrationLock.test_and_set(std::memory_order_acquire))
            YieldProcessor();
        if (g_wristCalibrationProviders[armSlot] != provider)
        {
            const vr::pose::Vec3 initialReach = vr::pose::NormalizeOr(
                desiredHandPosition - shoulderPosition, {0.0f, 0.0f, 1.0f});
            const vr::pose::Vec3 initialElbow = elbowPosition - shoulderPosition;
            const vr::pose::Vec3 initialPole = vr::pose::NormalizeOr(initialElbow -
                initialReach * vr::pose::Dot(initialElbow, initialReach),
                vr::pose::StableOrthogonal(initialReach));
            g_controllerLocalPoleDirections[armSlot] =
                controllerOrientation.Inverse().Rotate(
                initialPole);
            g_wristCalibrationProviders[armSlot] = provider;
            logging::Info(armSlot == 1
                ? "Right-arm IK calibrated controller-local elbow pole"
                : "Left-arm IK calibrated controller-local elbow pole");
        }
        CalibratedHandTarget result{};
        result.orientation = desiredHandOrientation;
        result.position = desiredHandPosition;
        const vr::pose::Vec3 reachDirection = vr::pose::NormalizeOr(
            result.position - shoulderPosition, {0.0f, 0.0f, 1.0f});
        const vr::pose::Vec3 currentElbow = elbowPosition - shoulderPosition;
        const vr::pose::Vec3 currentPole = vr::pose::NormalizeOr(currentElbow -
            reachDirection * vr::pose::Dot(currentElbow, reachDirection),
            vr::pose::StableOrthogonal(reachDirection));
        const vr::pose::Vec3 rotatedPole = controllerOrientation.Rotate(
            g_controllerLocalPoleDirections[armSlot]);
        const vr::pose::Vec3 controllerPole = vr::pose::NormalizeOr(rotatedPole -
            reachDirection * vr::pose::Dot(rotatedPole, reachDirection), currentPole);
        const vr::pose::Vec3 anatomicalPole = vr::pose::NormalizeOr(
            anatomicalPoleDirection - reachDirection *
                vr::pose::Dot(anatomicalPoleDirection, reachDirection),
            currentPole);
        // Wrist roll must not be allowed to rotate the whole elbow around the
        // shoulder-hand axis. Prefer an outward/downward anatomical plane, retain
        // some of the authored animation, and use controller roll only as a small
        // secondary hint.
        const vr::pose::Vec3 safePole = vr::pose::NormalizeOr(
            anatomicalPole * 0.80f + currentPole * 0.20f, anatomicalPole);
        const vr::pose::Vec3 projectedPole = vr::pose::NormalizeOr(
            safePole * 0.90f + controllerPole * 0.10f, safePole);
        const float poleDistance = (std::max)(0.5f,
            (result.position - shoulderPosition).Length());
        result.polePosition = shoulderPosition + projectedPole * poleDistance;
        g_wristCalibrationLock.clear(std::memory_order_release);
        return result;
    }

    vr::pose::Quaternion TwistAroundAxis(const vr::pose::Quaternion& input,
        const vr::pose::Vec3& axis) noexcept
    {
        const vr::pose::Quaternion q = input.Normalized();
        const vr::pose::Vec3 unitAxis = vr::pose::NormalizeOr(axis, {0.0f, 0.0f, 1.0f});
        const float projection = q.x * unitAxis.x + q.y * unitAxis.y +
            q.z * unitAxis.z;
        return vr::pose::Quaternion{unitAxis.x * projection,
            unitAxis.y * projection, unitAxis.z * projection, q.w}.Normalized();
    }

    float QuaternionAngleDegrees(const vr::pose::Quaternion& input) noexcept
    {
        const float w = (std::clamp)(std::fabs(input.Normalized().w), 0.0f, 1.0f);
        return 2.0f * std::acos(w) * 57.2957795131f;
    }

    vr::pose::Quaternion EulerDegrees(float x, float y, float z) noexcept
    {
        constexpr float kDegreesToRadians = 0.0174532925199f;
        const vr::pose::Quaternion qx = vr::pose::Quaternion::FromAxisAngle(
            {1.0f, 0.0f, 0.0f}, x * kDegreesToRadians);
        const vr::pose::Quaternion qy = vr::pose::Quaternion::FromAxisAngle(
            {0.0f, 1.0f, 0.0f}, y * kDegreesToRadians);
        const vr::pose::Quaternion qz = vr::pose::Quaternion::FromAxisAngle(
            {0.0f, 0.0f, 1.0f}, z * kDegreesToRadians);
        return (qz * qy * qx).Normalized();
    }

    std::wstring ConfigurationFile() noexcept
    {
        wchar_t executablePath[32768]{};
        if (!GetModuleFileNameW(nullptr, executablePath,
                static_cast<DWORD>(std::size(executablePath))))
            return L"dayz_openxr.ini";
        wchar_t* separator = wcsrchr(executablePath, L'\\');
        if (!separator)
            separator = wcsrchr(executablePath, L'/');
        if (!separator)
            return L"dayz_openxr.ini";
        wcscpy_s(separator + 1, std::size(executablePath) -
            (separator + 1 - executablePath), L"dayz_openxr.ini");
        return executablePath;
    }

    bool ReadBoolean(const wchar_t* key, bool fallback) noexcept
    {
        wchar_t value[16]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(L"human_pose_probe", key,
            fallback ? L"true" : L"false", value, static_cast<DWORD>(std::size(value)),
            path.c_str());
        return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 ||
            _wcsicmp(value, L"on") == 0 || wcscmp(value, L"1") == 0;
    }

    float ReadFloat(const wchar_t* key, float fallback) noexcept
    {
        wchar_t fallbackText[32]{};
        swprintf_s(fallbackText, L"%.4f", fallback);
        wchar_t value[32]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(L"human_pose_probe", key, fallbackText, value,
            static_cast<DWORD>(std::size(value)), path.c_str());
        wchar_t* end{};
        const float parsed = std::wcstof(value, &end);
        return end != value && std::isfinite(parsed) ? parsed : fallback;
    }

    std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) noexcept
    {
        wchar_t value[64]{};
        const std::wstring path = ConfigurationFile();
        GetPrivateProfileStringW(L"human_pose_probe", key, fallback, value,
            static_cast<DWORD>(std::size(value)), path.c_str());
        return value;
    }

    bool Match(std::uintptr_t rva, const std::uint8_t* expected,
        std::size_t size) noexcept
    {
        return std::equal(expected, expected + size,
            reinterpret_cast<const std::uint8_t*>(g_moduleBase + rva));
    }

    bool ValidateInternals() noexcept
    {
        constexpr std::array<std::uint8_t, 22> finalSimulationSignature{
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
            0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
            0x89, 0x78, 0x20, 0x41, 0x56, 0x48};
        constexpr std::array<std::uint8_t, 21> humanAnimationSignature{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
            0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C,
            0x89, 0x74, 0x24, 0x20, 0x55};
        constexpr std::array<std::uint8_t, 16> skinningExportSignature{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
        constexpr std::array<std::uint8_t, 14> getLocalSignature{
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x8B, 0xC2,
            0x49, 0x8B, 0xD8, 0x8B, 0x94, 0x81};
        constexpr std::array<std::uint8_t, 16> setLocalSignature{
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x8B,
            0x40, 0x24, 0x48, 0x8B, 0xD9, 0x44, 0x8B, 0xD2};
        constexpr std::array<std::uint8_t, 16> rebuildSignature{
            0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x48, 0x8D,
            0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00};
        constexpr std::array<std::uint8_t, 20> setEntityTransformSignature{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
            0x48, 0x83, 0xEC, 0x60};
        constexpr std::array<std::uint8_t, 24> entityAttachmentSignature{
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
            0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
        constexpr std::array<std::uint8_t, 32> playerProxySignature{
            0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41,
            0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
            0x20, 0x45, 0x33, 0xFF, 0x48, 0x8B, 0xF9, 0x4C,
            0x89, 0x3A, 0x45, 0x0F, 0xB6, 0xE1, 0x4C, 0x89};
        constexpr std::array<std::uint8_t, 36> playerProxyLocalSignature{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0x01, 0x49, 0x8B, 0xD9,
            0x44, 0x8B, 0x4C, 0x24, 0x50, 0x48, 0x8B, 0xFA,
            0x48, 0x8B, 0xD3, 0xFF, 0x90, 0xE8, 0x05, 0x00,
            0x00, 0x8B, 0x03, 0x89};
        if (!Match(g_buildProfile->finalPlayerSimulationRva,
                finalSimulationSignature.data(), finalSimulationSignature.size()) ||
            !Match(g_buildProfile->humanAnimationUpdateRva,
                humanAnimationSignature.data(), humanAnimationSignature.size()) ||
            !Match(g_buildProfile->setEntityTransformRva,
                setEntityTransformSignature.data(), setEntityTransformSignature.size()) ||
            (g_buildProfile->entityAttachmentTransformRva &&
                !Match(g_buildProfile->entityAttachmentTransformRva,
                    entityAttachmentSignature.data(),
                    entityAttachmentSignature.size())) ||
            (g_buildProfile->playerProxyTransformRva &&
                !Match(g_buildProfile->playerProxyTransformRva,
                    playerProxySignature.data(), playerProxySignature.size())) ||
            (g_buildProfile->playerProxyLocalTransformRva &&
                !Match(g_buildProfile->playerProxyLocalTransformRva,
                    playerProxyLocalSignature.data(),
                    playerProxyLocalSignature.size())) ||
            !Match(g_buildProfile->skinningExportRva, skinningExportSignature.data(),
                skinningExportSignature.size()) ||
            !Match(g_buildProfile->getLocalTransformRva, getLocalSignature.data(),
                getLocalSignature.size()) ||
            !Match(g_buildProfile->setLocalTransformRva, setLocalSignature.data(),
                setLocalSignature.size()) ||
            !Match(g_buildProfile->rebuildModelTransformsRva, rebuildSignature.data(),
                rebuildSignature.size()))
            return false;

        const auto* vtable = reinterpret_cast<const std::uintptr_t*>(
            g_moduleBase + g_buildProfile->poseProviderVtableRva);
        return vtable[14] == g_moduleBase + g_buildProfile->getLocalTransformRva &&
            vtable[17] == g_moduleBase + g_buildProfile->setLocalTransformRva;
    }

    std::uint32_t InternalBone(std::uintptr_t provider, unsigned publicBone) noexcept
    {
        if (publicBone > kMaximumPublicBoneIndex)
            return UINT32_MAX;
        return *reinterpret_cast<const std::uint32_t*>(provider + kProviderBoneMapOffset +
            sizeof(std::uint32_t) * publicBone);
    }

    const float* ModelTransform(std::uintptr_t provider, std::uint32_t internalBone) noexcept
    {
        const auto transforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderModelTransformsOffset);
        return transforms && internalBone != UINT32_MAX
            ? reinterpret_cast<const float*>(transforms + sizeof(float) * 12 * internalBone)
            : nullptr;
    }

    vr::pose::Vec3 Translation(const float* matrix) noexcept
    {
        return matrix ? vr::pose::Vec3{matrix[9], matrix[10], matrix[11]} : vr::pose::Vec3{};
    }

    vr::pose::Quaternion Rotation(const float* matrix) noexcept
    {
        if (!matrix)
            return {};
        const float trace = matrix[0] + matrix[4] + matrix[8];
        vr::pose::Quaternion result{};
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            result = {(matrix[5] - matrix[7]) / s, (matrix[6] - matrix[2]) / s,
                (matrix[1] - matrix[3]) / s, 0.25f * s};
        }
        else if (matrix[0] > matrix[4] && matrix[0] > matrix[8])
        {
            const float s = std::sqrt(1.0f + matrix[0] - matrix[4] - matrix[8]) * 2.0f;
            result = {0.25f * s, (matrix[3] + matrix[1]) / s,
                (matrix[6] + matrix[2]) / s, (matrix[5] - matrix[7]) / s};
        }
        else if (matrix[4] > matrix[8])
        {
            const float s = std::sqrt(1.0f + matrix[4] - matrix[0] - matrix[8]) * 2.0f;
            result = {(matrix[3] + matrix[1]) / s, 0.25f * s,
                (matrix[7] + matrix[5]) / s, (matrix[6] - matrix[2]) / s};
        }
        else
        {
            const float s = std::sqrt(1.0f + matrix[8] - matrix[0] - matrix[4]) * 2.0f;
            result = {(matrix[6] + matrix[2]) / s, (matrix[7] + matrix[5]) / s,
                0.25f * s, (matrix[1] - matrix[3]) / s};
        }
        return result.Normalized();
    }

    void StoreRotation(float* matrix, const vr::pose::Quaternion& input) noexcept
    {
        const vr::pose::Quaternion q = input.Normalized();
        const float xx = q.x * q.x;
        const float yy = q.y * q.y;
        const float zz = q.z * q.z;
        const float xy = q.x * q.y;
        const float xz = q.x * q.z;
        const float yz = q.y * q.z;
        const float wx = q.w * q.x;
        const float wy = q.w * q.y;
        const float wz = q.w * q.z;
        // DayZ stores this 3x3 orientation column-major; translation remains at 9..11.
        matrix[0] = 1.0f - 2.0f * (yy + zz);
        matrix[1] = 2.0f * (xy + wz);
        matrix[2] = 2.0f * (xz - wy);
        matrix[3] = 2.0f * (xy - wz);
        matrix[4] = 1.0f - 2.0f * (xx + zz);
        matrix[5] = 2.0f * (yz + wx);
        matrix[6] = 2.0f * (xz + wy);
        matrix[7] = 2.0f * (yz - wx);
        matrix[8] = 1.0f - 2.0f * (xx + yy);
    }

    bool IsBoneOrDescendant(std::uintptr_t provider, std::uint32_t bone,
        std::uint32_t ancestor, std::uint32_t boneCount) noexcept
    {
        for (std::uint32_t depth = 0; depth <= boneCount && bone < boneCount; ++depth)
        {
            if (bone == ancestor)
                return true;
            bone = *reinterpret_cast<const std::uint32_t*>(provider +
                kProviderParentMapOffset + sizeof(std::uint32_t) * bone);
        }
        return false;
    }

    void RotateAffineAround(float* matrix, const vr::pose::Quaternion& rotation,
        const vr::pose::Vec3& pivot) noexcept
    {
        if (!matrix)
            return;
        for (unsigned column = 0; column < 3; ++column)
        {
            const unsigned offset = column * 3;
            const vr::pose::Vec3 rotated = rotation.Rotate(
                {matrix[offset], matrix[offset + 1], matrix[offset + 2]});
            matrix[offset] = rotated.x;
            matrix[offset + 1] = rotated.y;
            matrix[offset + 2] = rotated.z;
        }
        const vr::pose::Vec3 translated = pivot + rotation.Rotate(Translation(matrix) - pivot);
        matrix[9] = translated.x;
        matrix[10] = translated.y;
        matrix[11] = translated.z;
    }

    bool HasActiveItemIkProfile(void* player) noexcept
    {
        if (!player)
            return false;
        constexpr std::ptrdiff_t kPlayerItemAccessorOffset = 10064;
        constexpr std::ptrdiff_t kAccessorActiveProfileOffset = 32;
        constexpr std::ptrdiff_t kProfilePrimaryIkOffset = 32;
        constexpr std::ptrdiff_t kProfileSecondaryIkOffset = 40;
        const auto accessor = *reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<std::uintptr_t>(player) +
                kPlayerItemAccessorOffset);
        if (!accessor)
            return false;
        const auto profile = *reinterpret_cast<const std::uintptr_t*>(
            accessor + kAccessorActiveProfileOffset);
        if (!profile)
            return false;
        const bool hasIk =
            *reinterpret_cast<const std::uintptr_t*>(
                profile + kProfilePrimaryIkOffset) ||
            *reinterpret_cast<const std::uintptr_t*>(
                profile + kProfileSecondaryIkOffset);
        if (hasIk && g_loggedTwoHandProfile.exchange(profile,
                std::memory_order_relaxed) != profile)
        {
            std::ostringstream message;
            message << "Detected active item IK profile: player=" << player
                << " accessor=" << reinterpret_cast<void*>(accessor)
                << " profile=" << reinterpret_cast<void*>(profile)
                << " primary_ik="
                << *reinterpret_cast<void* const*>(
                    profile + kProfilePrimaryIkOffset)
                << " secondary_ik="
                << *reinterpret_cast<void* const*>(
                    profile + kProfileSecondaryIkOffset);
            logging::Info(message.str());
        }
        return hasIk;
    }

    bool ApplySingleArmIk(std::uintptr_t provider, bool rightArm) noexcept
    {
        const unsigned armSlot = rightArm ? 1u : 0u;
        const dayz::stereo_state::ControllerPose controller =
            dayz::stereo_state::GetControllerPose(armSlot);
        const dayz::stereo_state::HmdPosition hmd = dayz::stereo_state::GetHmdPosition();
        const dayz::stereo_state::HmdOrientation hmdOrientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!controller.valid || !hmd.valid || !hmdOrientation.valid)
            return false;

        const std::uint32_t arm = InternalBone(provider,
            rightArm ? kRightArmBone : kLeftArmBone);
        const std::uint32_t foreArm = InternalBone(provider,
            rightArm ? kRightForeArmBone : kLeftForeArmBone);
        const std::uint32_t hand = InternalBone(provider,
            rightArm ? kRightHandBone : kLeftHandBone);
        const std::uint32_t handDummy = InternalBone(provider,
            rightArm ? kRightHandDummyBone : kLeftHandDummyBone);
        const std::uint32_t spine3 = InternalBone(provider, kSpine3Bone);
        const std::uint32_t head = InternalBone(provider, kHeadBone);
        if (arm == UINT32_MAX || foreArm == UINT32_MAX || hand == UINT32_MAX ||
            spine3 == UINT32_MAX || head == UINT32_MAX)
            return false;

        const float* armModel = ModelTransform(provider, arm);
        const float* foreArmModel = ModelTransform(provider, foreArm);
        const float* handModel = ModelTransform(provider, hand);
        const float* handDummyModel = handDummy == UINT32_MAX
            ? nullptr : ModelTransform(provider, handDummy);
        const float* spine3Model = ModelTransform(provider, spine3);
        const float* headModel = ModelTransform(provider, head);
        if (!armModel || !foreArmModel || !handModel || !spine3Model || !headModel)
            return false;
        vr::pose::Transform vanillaDummyPose{};
        if (handDummyModel)
        {
            vanillaDummyPose.translation = Translation(handDummyModel);
            vanillaDummyPose.rotation = Rotation(handDummyModel);
        }

        const vr::pose::Quaternion hmdQ{hmdOrientation.x, hmdOrientation.y,
            hmdOrientation.z, hmdOrientation.w};
        const vr::pose::Quaternion controllerQ{controller.orientationX,
            controller.orientationY, controller.orientationZ,
            controller.orientationW};
        // A bone's rotation is its bind/local frame, not the model's cardinal
        // frame. Construct an anatomical model basis from joint positions instead:
        // +X points toward the right shoulder, +Y toward the head and +Z forward.
        const vr::pose::Vec3 spinePosition = Translation(spine3Model);
        const vr::pose::Vec3 headPosition = Translation(headModel);
        const vr::pose::Vec3 up = vr::pose::NormalizeOr(
            headPosition - spinePosition, {0.0f, 1.0f, 0.0f});
        const vr::pose::Vec3 shoulderDirection =
            Translation(armModel) - spinePosition;
        vr::pose::Vec3 right = vr::pose::NormalizeOr(
            (shoulderDirection - up * vr::pose::Dot(shoulderDirection, up)) *
                (rightArm ? 1.0f : -1.0f),
            {1.0f, 0.0f, 0.0f});
        const vr::pose::Vec3 forward = vr::pose::NormalizeOr(
            vr::pose::Cross(right, up), {0.0f, 0.0f, 1.0f});
        right = vr::pose::NormalizeOr(vr::pose::Cross(up, forward), right);
        const float modelBasisMatrix[12]{
            right.x, right.y, right.z,
            up.x, up.y, up.z,
            forward.x, forward.y, forward.z,
            0.0f, 0.0f, 0.0f};
        const vr::pose::Quaternion modelBasis = Rotation(modelBasisMatrix);

        // Express the controller relative to the HMD first. OpenXR uses -Z as
        // forward, while the anatomical DayZ basis above uses +Z as forward.
        const vr::pose::Quaternion controllerRelative =
            (hmdQ.Inverse() * controllerQ).Normalized();
        const vr::pose::Quaternion mappedControllerRelative{
            -controllerRelative.x, -controllerRelative.y,
            controllerRelative.z, controllerRelative.w};
        const vr::pose::Quaternion controllerModelOrientation =
            (modelBasis * mappedControllerRelative).Normalized();
        const auto mapToModelBasis = [&](const vr::pose::Vec3& value) noexcept
        {
            return right * value.x + up * value.y + forward * value.z;
        };
        const auto controllerTargetPosition =
            [&](const dayz::stereo_state::ControllerPose& pose) noexcept
        {
            const vr::pose::Vec3 delta{pose.positionX - hmd.x,
                pose.positionY - hmd.y, pose.positionZ - hmd.z};
            const vr::pose::Vec3 relative =
                hmdQ.Inverse().Rotate(delta) * g_ikTargetScale;
            return headPosition + mapToModelBasis(g_ikTargetOffset) +
                mapToModelBasis({relative.x, relative.y, -relative.z});
        };
        const vr::pose::Vec3 rawTargetPosition =
            controllerTargetPosition(controller);

        vr::pose::TwoBoneChain chain{};
        chain.start.translation = Translation(armModel);
        chain.start.rotation = Rotation(armModel);
        chain.middle.translation = Translation(foreArmModel);
        chain.middle.rotation = Rotation(foreArmModel);
        chain.end.translation = Translation(handModel);
        chain.end.rotation = Rotation(handModel);
        vr::pose::Quaternion desiredDummyOrientation =
            (controllerModelOrientation * g_gripRotationOffset).Normalized();
        const vr::pose::Vec3 desiredDummyPosition = rawTargetPosition +
            controllerModelOrientation.Rotate(g_gripToWristOffset);
        vr::pose::Quaternion desiredHandOrientation = desiredDummyOrientation;
        vr::pose::Vec3 desiredHandPosition = desiredDummyPosition;
        if (rightArm && handDummyModel && g_currentSimulationPlayer &&
            HasActiveItemIkProfile(g_currentSimulationPlayer))
        {
            const std::uint32_t leftHand = InternalBone(provider, kLeftHandBone);
            const float* leftHandModel = leftHand == UINT32_MAX
                ? nullptr : ModelTransform(provider, leftHand);
            const dayz::stereo_state::ControllerPose leftController =
                dayz::stereo_state::GetControllerPose(0);
            if (leftHandModel && leftController.valid)
            {
                const vr::pose::Transform vanillaRightDummy{
                    Translation(handDummyModel), Rotation(handDummyModel)};
                const vr::pose::Transform vanillaLeftHand{
                    Translation(leftHandModel), Rotation(leftHandModel)};
                const vr::pose::Quaternion inverseRightDummy =
                    vanillaRightDummy.rotation.Inverse();
                g_twoHandCoupling.rightDummyToLeftHand.translation =
                    inverseRightDummy.Rotate(vanillaLeftHand.translation -
                        vanillaRightDummy.translation);
                g_twoHandCoupling.rightDummyToLeftHand.rotation =
                    (inverseRightDummy * vanillaLeftHand.rotation).Normalized();
                const float gripSpan =
                    g_twoHandCoupling.rightDummyToLeftHand.translation.Length();
                g_twoHandCoupling.candidate =
                    gripSpan >= 0.12f && gripSpan <= 0.90f;
                if (g_twoHandCoupling.candidate)
                {
                    const vr::pose::Vec3 leftControllerTarget =
                        controllerTargetPosition(leftController);
                    const vr::pose::Vec3 authoredGripDirection =
                        desiredDummyOrientation.Rotate(
                            g_twoHandCoupling.rightDummyToLeftHand.translation);
                    const vr::pose::Vec3 trackedGripDirection =
                        leftControllerTarget - desiredDummyPosition;
                    if (authoredGripDirection.LengthSquared() > 0.01f &&
                        trackedGripDirection.LengthSquared() > 0.01f)
                    {
                        const vr::pose::Quaternion twoHandDirection =
                            vr::pose::Quaternion::FromTo(authoredGripDirection,
                                trackedGripDirection);
                        const vr::pose::Quaternion weightedDirection =
                            vr::pose::Quaternion::Slerp(
                                vr::pose::Quaternion::Identity(),
                                twoHandDirection, 0.85f);
                        desiredDummyOrientation = (weightedDirection *
                            desiredDummyOrientation).Normalized();
                    }
                }
            }
        }
        if (!rightArm && g_twoHandCoupling.active)
        {
            desiredHandOrientation =
                g_twoHandCoupling.leftHandTarget.rotation;
            desiredHandPosition =
                g_twoHandCoupling.leftHandTarget.translation;
        }
        else if (handDummyModel)
        {
            // The hand dummy is DayZ's item/grip socket. Preserve its
            // evaluated rigid offset from RightHand and solve the wrist pose that
            // makes the socket coincide with the OpenXR grip pose.
            const vr::pose::Quaternion handToDummyOrientation =
                (chain.end.rotation.Inverse() * Rotation(handDummyModel)).Normalized();
            const vr::pose::Vec3 handToDummyPosition =
                chain.end.rotation.Inverse().Rotate(
                    Translation(handDummyModel) - chain.end.translation);
            desiredHandOrientation = (desiredDummyOrientation *
                handToDummyOrientation.Inverse()).Normalized();
            desiredHandPosition = desiredDummyPosition -
                desiredHandOrientation.Rotate(handToDummyPosition);
        }
        const vr::pose::Vec3 outward = right * (rightArm ? 1.0f : -1.0f);
        const vr::pose::Vec3 anatomicalPoleDirection =
            vr::pose::NormalizeOr(outward * 0.75f - up * 0.65f -
                forward * 0.15f, outward);
        const CalibratedHandTarget calibratedTarget = CalibrateHandTarget(provider,
            armSlot,
            controllerModelOrientation, desiredHandOrientation, chain.start.translation,
            chain.middle.translation, desiredHandPosition,
            anatomicalPoleDirection);
        vr::pose::TwoBoneTarget target{};
        target.position = calibratedTarget.position;
        target.orientation = calibratedTarget.orientation;
        target.pole_position = calibratedTarget.polePosition;
        target.weight = g_ikWeight;
        target.match_end_orientation = true;
        vr::pose::TwoBoneConstraints constraints{};
        constraints.minimum_bend_radians = 0.08f;
        constraints.maximum_bend_radians = 2.45f;
        constraints.maximum_start_correction_radians = 1.92f;
        constraints.soften_start_ratio = 0.95f;
        const vr::pose::TwoBoneResult solved = vr::pose::SolveTwoBone(chain, target,
            constraints);
        if (!solved.Succeeded())
            return false;

        const std::uint32_t boneCount =
            *reinterpret_cast<const std::uint32_t*>(provider + kProviderBoneCountOffset) +
            *reinterpret_cast<const std::uint32_t*>(provider + kProviderExtraBoneCountOffset);
        const auto modelTransforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderModelTransformsOffset);
        const auto skinningTransforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderSkinningTransformsOffset);
        if (!modelTransforms || !skinningTransforms || boneCount == 0 || boneCount > 512)
            return false;

        const vr::pose::Vec3 solvedForeArmAxis = solved.solved_end_position -
            solved.solved_middle_position;
        const vr::pose::Quaternion fullForeArmTwist = TwistAroundAxis(
            solved.end_correction, solvedForeArmAxis);
        const vr::pose::Quaternion distributedForeArmTwist =
            vr::pose::Quaternion::Slerp(vr::pose::Quaternion::Identity(),
                fullForeArmTwist, 0.70f);
        const vr::pose::Quaternion residualHandCorrection =
            (solved.end_correction * distributedForeArmTwist.Inverse()).Normalized();

        // Apply the identical model-space corrections to both representations.
        // Rendering exports the skinning palette, while held-item attachment and
        // gameplay bone consumers read evaluated model transforms.
        const auto applyCorrections = [&](std::uintptr_t transforms) noexcept
        {
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (IsBoneOrDescendant(provider, bone, arm, boneCount) ||
                    bone == handDummy)
                {
                    auto* matrix = reinterpret_cast<float*>(transforms +
                        sizeof(float) * 12 * bone);
                    RotateAffineAround(matrix, solved.start_correction,
                        chain.start.translation);
                }
            }
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (IsBoneOrDescendant(provider, bone, foreArm, boneCount) ||
                    bone == handDummy)
                {
                    auto* matrix = reinterpret_cast<float*>(transforms +
                        sizeof(float) * 12 * bone);
                    RotateAffineAround(matrix, solved.middle_correction,
                        solved.solved_middle_position);
                }
            }
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (IsBoneOrDescendant(provider, bone, foreArm, boneCount) ||
                    bone == handDummy)
                {
                    auto* matrix = reinterpret_cast<float*>(transforms +
                        sizeof(float) * 12 * bone);
                    RotateAffineAround(matrix, distributedForeArmTwist,
                        solved.solved_middle_position);
                }
            }
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (IsBoneOrDescendant(provider, bone, hand, boneCount) ||
                    bone == handDummy)
                {
                    auto* matrix = reinterpret_cast<float*>(transforms +
                        sizeof(float) * 12 * bone);
                    RotateAffineAround(matrix, residualHandCorrection,
                        solved.solved_end_position);
                }
            }
        };
        applyCorrections(modelTransforms);
        applyCorrections(skinningTransforms);

        if (rightArm && g_twoHandCoupling.candidate && handDummyModel)
        {
            const vr::pose::Transform correctedRightDummy{
                Translation(handDummyModel), Rotation(handDummyModel)};
            g_twoHandCoupling.leftHandTarget.translation =
                correctedRightDummy.translation +
                correctedRightDummy.rotation.Rotate(
                    g_twoHandCoupling.rightDummyToLeftHand.translation);
            g_twoHandCoupling.leftHandTarget.rotation =
                (correctedRightDummy.rotation *
                    g_twoHandCoupling.rightDummyToLeftHand.rotation).Normalized();
            g_twoHandCoupling.active = true;
        }

        if (rightArm && g_currentSimulationPlayer && handDummyModel)
        {
            g_simulationSocketPose.player = g_currentSimulationPlayer;
            g_simulationSocketPose.provider = provider;
            g_simulationSocketPose.vanilla = vanillaDummyPose;
            g_simulationSocketPose.corrected.translation =
                Translation(handDummyModel);
            g_simulationSocketPose.corrected.rotation = Rotation(handDummyModel);
            g_simulationSocketPose.valid = true;
        }

        if (g_loggedIkProviders[armSlot].exchange(provider,
                std::memory_order_relaxed) != provider)
        {
            std::ostringstream message;
            message << (rightArm ? "Right-arm" : "Left-arm")
                << " IK provider solve: provider=" << provider
                << " internal=" << arm << ',' << foreArm << ',' << hand
                << " shoulder=" << chain.start.translation.x << ','
                << chain.start.translation.y << ',' << chain.start.translation.z
                << " raw_target=" << rawTargetPosition.x << ',' << rawTargetPosition.y
                << ',' << rawTargetPosition.z << " target="
                << calibratedTarget.position.x << ',' << calibratedTarget.position.y
                << ',' << calibratedTarget.position.z << " dummy=" << handDummy
                << " lengths=" << solved.upper_length << ','
                << solved.lower_length << " reachable=" << solved.target_reachable
                << " correction_deg="
                << QuaternionAngleDegrees(solved.start_correction) << ','
                << QuaternionAngleDegrees(solved.middle_correction) << ','
                << QuaternionAngleDegrees(distributedForeArmTwist) << ','
                << QuaternionAngleDegrees(residualHandCorrection)
                << " basis_right=" << right.x << ',' << right.y << ',' << right.z
                << " basis_up=" << up.x << ',' << up.y << ',' << up.z
                << " basis_forward=" << forward.x << ',' << forward.y << ','
                << forward.z
                << " mapping=hmd_relative_anatomical_basis grip="
                << (rightArm ? "right_hand_dummy" : "left_hand_dummy")
                << " orientation=anatomical_pole_with_limited_controller_hint"
                << " write=model_and_skinning_palettes";
            logging::Info(message.str());
        }
        return true;
    }

    bool ApplyBothArmsIk(std::uintptr_t provider,
        RebuildModelTransformsFn rebuild) noexcept
    {
        if (!SelectIkProvider(provider))
            return false;

        // Start both independent chains from one clean vanilla pose. Applying the
        // right arm does not alter the left chain or the torso basis.
        *reinterpret_cast<std::uint8_t*>(provider + kProviderPoseDirtyOffset) = 1;
        rebuild(reinterpret_cast<void*>(provider));
        g_twoHandCoupling = {};
        const bool rightSolved = ApplySingleArmIk(provider, true);
        const bool leftSolved = ApplySingleArmIk(provider, false);
        return rightSolved || leftSolved;
    }

    bool ApplyProbe(void* providerPointer) noexcept
    {
        if (!providerPointer)
            return false;
        const auto provider = reinterpret_cast<std::uintptr_t>(providerPointer);
        if (!provider || *reinterpret_cast<std::uintptr_t*>(provider) !=
                g_moduleBase + g_buildProfile->poseProviderVtableRva)
            return false;

        const std::uint32_t mappedIndex = *reinterpret_cast<const std::uint32_t*>(
            provider + kProviderBoneMapOffset + sizeof(std::uint32_t) * g_boneIndex);
        const std::uint32_t boneCount = *reinterpret_cast<const std::uint32_t*>(
            provider + kProviderBoneCountOffset);
        const std::uint32_t extraBoneCount = *reinterpret_cast<const std::uint32_t*>(
            provider + kProviderExtraBoneCountOffset);
        if (mappedIndex == UINT32_MAX || mappedIndex >= boneCount + extraBoneCount)
            return false;

        const auto* vtable = *reinterpret_cast<std::uintptr_t* const*>(provider);
        if (vtable[14] != g_moduleBase + g_buildProfile->getLocalTransformRva ||
            vtable[17] != g_moduleBase + g_buildProfile->setLocalTransformRva)
            return false;
        const auto getLocal = reinterpret_cast<GetLocalTransformFn>(vtable[14]);
        const auto setLocal = reinterpret_cast<SetLocalTransformFn>(vtable[17]);
        const auto rebuildModelTransforms = reinterpret_cast<RebuildModelTransformsFn>(
            g_moduleBase + g_buildProfile->rebuildModelTransformsRva);

        if (g_mode == ProbeMode::rightArmIk)
            return ApplyBothArmsIk(provider, rebuildModelTransforms);

        // The vanilla update may leave the provider dirty. Build its current pose first so
        // the diagnostic deltas and the subsequent override start from the same frame.
        rebuildModelTransforms(reinterpret_cast<void*>(provider));
        float localTransform[12]{};
        getLocal(reinterpret_cast<void*>(provider), g_boneIndex, localTransform);
        const float originalLocalTranslation[3]{
            localTransform[9], localTransform[10], localTransform[11]};

        const auto modelTransforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderModelTransformsOffset);
        const auto skinningTransforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderSkinningTransformsOffset);
        if (!modelTransforms || !skinningTransforms)
            return false;
        const auto modelTransform = reinterpret_cast<const float*>(
            modelTransforms + sizeof(float) * 12 * mappedIndex);
        const auto skinningTransform = reinterpret_cast<const float*>(
            skinningTransforms + sizeof(float) * 12 * mappedIndex);
        const float originalModelTranslation[3]{
            modelTransform[9], modelTransform[10], modelTransform[11]};
        const float originalSkinningTranslation[3]{
            skinningTransform[9], skinningTransform[10], skinningTransform[11]};

        const float seconds = static_cast<float>(GetTickCount64() % 600000u) / 1000.0f;
        const float wave = std::sin(seconds * g_frequencyHz * 6.28318530718f);
        localTransform[9] += g_translationX * wave;
        localTransform[10] += g_translationY * wave;
        localTransform[11] += g_translationZ * wave;
        setLocal(reinterpret_cast<void*>(provider), g_boneIndex, localTransform);
        rebuildModelTransforms(reinterpret_cast<void*>(provider));

        if (!g_loggedFirstRebuild.exchange(true, std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Human pose rebuild verified: internal_bone=" << mappedIndex
                << " local_delta=" << localTransform[9] - originalLocalTranslation[0] << ','
                << localTransform[10] - originalLocalTranslation[1] << ','
                << localTransform[11] - originalLocalTranslation[2]
                << " model_delta=" << modelTransform[9] - originalModelTranslation[0] << ','
                << modelTransform[10] - originalModelTranslation[1] << ','
                << modelTransform[11] - originalModelTranslation[2]
                << " skin_delta=" << skinningTransform[9] - originalSkinningTranslation[0]
                << ',' << skinningTransform[10] - originalSkinningTranslation[1] << ','
                << skinningTransform[11] - originalSkinningTranslation[2];
            logging::Info(message.str());
        }
        return true;
    }

    void LogExportMapping(void* providerPointer, std::int64_t* destination,
        std::int64_t exportContext) noexcept
    {
        if (g_loggedExportMapping.load(std::memory_order_relaxed) || !providerPointer ||
            !destination || !*destination)
            return;
        const auto provider = reinterpret_cast<std::uintptr_t>(providerPointer);
        const std::uint32_t internalBone = *reinterpret_cast<const std::uint32_t*>(
            provider + kProviderBoneMapOffset + sizeof(std::uint32_t) * g_boneIndex);
        const auto cache = *reinterpret_cast<const std::uintptr_t*>(provider + 3096);
        const std::uint32_t cacheCount = *reinterpret_cast<const std::uint32_t*>(
            provider + 3108);
        if (!cache || internalBone == UINT32_MAX)
            return;
        for (std::uint32_t cacheIndex = 0; cacheIndex < cacheCount; ++cacheIndex)
        {
            const auto entry = cache + 24u * cacheIndex;
            if (*reinterpret_cast<const std::int64_t*>(entry) != exportContext)
                continue;
            const std::uint32_t pairCount = *reinterpret_cast<const std::uint32_t*>(entry + 8);
            const auto pairs = *reinterpret_cast<const std::uintptr_t*>(entry + 16);
            if (!pairs)
                return;
            for (std::uint32_t pairIndex = 0; pairIndex < pairCount; ++pairIndex)
            {
                const auto pair = reinterpret_cast<const std::uint32_t*>(
                    pairs + 8u * pairIndex);
                if (pair[0] != internalBone)
                    continue;
                const std::uint32_t destinationIndex = pair[1];
                const auto outputMatrix = reinterpret_cast<const float*>(
                    static_cast<std::uintptr_t>(*destination) +
                    sizeof(float) * 12 * destinationIndex);
                const auto skinningTransforms = *reinterpret_cast<const std::uintptr_t*>(
                    provider + kProviderSkinningTransformsOffset);
                const auto sourceMatrix = reinterpret_cast<const float*>(
                    skinningTransforms + sizeof(float) * 12 * internalBone);
                if (!g_loggedExportMapping.exchange(true, std::memory_order_relaxed))
                {
                    std::ostringstream message;
                    message << "Human skinning export contains probe bone: internal="
                        << internalBone << " destination=" << destinationIndex
                        << " source_t=" << sourceMatrix[9] << ',' << sourceMatrix[10] << ','
                        << sourceMatrix[11] << " output_t=" << outputMatrix[9] << ','
                        << outputMatrix[10] << ',' << outputMatrix[11];
                    logging::Info(message.str());
                }
                return;
            }
            return;
        }
    }

    std::int64_t __fastcall HookedSkinningExport(void* provider, std::int64_t* destination,
        std::int64_t selection, std::int64_t context)
    {
        if (!g_active.load(std::memory_order_relaxed) || !ApplyProbe(provider))
            return g_skinningExport(provider, destination, selection, context);
        const std::uint64_t writes = g_writeCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!g_loggedFirstWrite.exchange(true, std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Human pose probe made its first pre-export write: provider=" << provider
                << " bone=" << g_boneIndex << " provider setter=DayZ+0x" << std::hex
                << g_buildProfile->setLocalTransformRva;
            logging::Info(message.str());
        }
        if (writes % 36000u == 0)
        {
            std::ostringstream message;
            message << "Human pose probe remains active; writes=" << writes;
            logging::Info(message.str());
        }
        const std::int64_t result = g_skinningExport(provider, destination, selection, context);
        LogExportMapping(provider, destination, context);
        return result;
    }

    void __fastcall HookedHumanAnimationUpdate(void* player, float deltaTime,
        std::int64_t updateContext, bool localSimulation, bool serializeState)
    {
        g_humanAnimationUpdate(player, deltaTime, updateContext, localSimulation,
            serializeState);
        if (!g_active.load(std::memory_order_relaxed) ||
            g_mode != ProbeMode::rightArmIk || !player)
            return;

        // Human animation has just rebuilt the vanilla pose. Player simulation
        // consumes the palette later in this same call chain to update held-item
        // attachments, before the render-only skinning export hook runs.
        constexpr std::uintptr_t kPlayerHumanStateOffset = 0x7E0;
        constexpr std::uintptr_t kHumanStatePoseProviderOffset = 0x110;
        const auto humanState = *reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<std::uintptr_t>(player) + kPlayerHumanStateOffset);
        const auto provider = humanState
            ? *reinterpret_cast<const std::uintptr_t*>(
                humanState + kHumanStatePoseProviderOffset)
            : 0;
        if (!provider || !ApplyProbe(reinterpret_cast<void*>(provider)))
            return;

        if (!g_loggedSimulationStageWrite.exchange(true, std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Right-arm IK applied at post-animation simulation stage: player="
                << player << " provider=" << reinterpret_cast<void*>(provider)
                << " consumer=held_item_attachment";
            logging::Info(message.str());
        }
    }

    vr::pose::Transform Compose(const vr::pose::Transform& parent,
        const vr::pose::Transform& local) noexcept
    {
        return {
            parent.translation + parent.rotation.Rotate(local.translation),
            (parent.rotation * local.rotation).Normalized()};
    }

    std::uintptr_t HeldItem(void* player) noexcept
    {
        if (!player)
            return 0;
        const auto* playerVtable =
            *reinterpret_cast<std::uintptr_t* const*>(player);
        if (!playerVtable || !playerVtable[238])
            return 0;
        using GetHandsOwnerFn = std::uintptr_t(__fastcall*)(void*);
        const std::uintptr_t handsOwner =
            reinterpret_cast<GetHandsOwnerFn>(playerVtable[238])(player);
        return handsOwner
            ? *reinterpret_cast<const std::uintptr_t*>(handsOwner + 432)
            : 0;
    }

    const float* EntityTransform(void* entity) noexcept
    {
        if (!entity)
            return nullptr;
        const auto* vtable = *reinterpret_cast<std::uintptr_t* const*>(entity);
        if (!vtable || !vtable[12])
            return nullptr;
        using GetEntityTransformFn = const float*(__fastcall*)(void*);
        return reinterpret_cast<GetEntityTransformFn>(vtable[12])(entity);
    }

    bool EntityRenderTransform(void* entity, float* output) noexcept
    {
        if (!entity || !output)
            return false;
        const auto* vtable = *reinterpret_cast<std::uintptr_t* const*>(entity);
        if (!vtable || !vtable[13])
            return false;
        using GetEntityRenderTransformFn = void(__fastcall*)(void*, float*);
        reinterpret_cast<GetEntityRenderTransformFn>(vtable[13])(entity, output);
        return Translation(output).IsFinite() && Rotation(output).IsFinite();
    }

    float* __fastcall HookedEntityAttachmentTransform(void* parent, float* output,
        std::int64_t link, void* child)
    {
        float* result = g_entityAttachmentTransform(parent, output, link, child);
        if (!g_active.load(std::memory_order_relaxed) ||
            g_mode != ProbeMode::rightArmIk || !parent || !result || !child)
            return result;

        const unsigned hookCall =
            g_attachmentHookCalls.fetch_add(1, std::memory_order_relaxed);
        if (hookCall == 0)
        {
            std::ostringstream message;
            message << "Entity attachment-transform hook received its first call: parent="
                << parent << " child=" << child << " link="
                << reinterpret_cast<void*>(link) << " caller=DayZ+0x" << std::hex
                << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase);
            logging::Info(message.str());
        }

        SimulationSocketPose socketPose{};
        if (g_currentSimulationPlayer == parent &&
            g_simulationSocketPose.valid)
        {
            socketPose = g_simulationSocketPose;
        }
        else
        {
            std::lock_guard<std::mutex> lock(g_latestSocketPoseMutex);
            socketPose = g_latestSocketPose;
        }
        if (!socketPose.valid || !socketPose.player)
            return result;

        const float* playerMatrix = EntityTransform(socketPose.player);
        if (!playerMatrix)
            return result;
        const vr::pose::Transform playerWorld{
            Translation(playerMatrix), Rotation(playerMatrix)};
        const vr::pose::Transform attachmentWorld{
            Translation(result), Rotation(result)};
        if (!playerWorld.translation.IsFinite() ||
            !playerWorld.rotation.IsFinite() ||
            !attachmentWorld.translation.IsFinite() ||
            !attachmentWorld.rotation.IsFinite())
            return result;

        const vr::pose::Transform vanillaSocketWorld =
            Compose(playerWorld, socketPose.vanilla);
        const vr::pose::Transform correctedSocketWorld =
            Compose(playerWorld, socketPose.corrected);
        const float vanillaDistance = (attachmentWorld.translation -
            vanillaSocketWorld.translation).Length();
        const float correctedDistance = (attachmentWorld.translation -
            correctedSocketWorld.translation).Length();
        const bool staleVanillaProxy = vanillaDistance < 1.0f &&
            vanillaDistance + 0.03f < correctedDistance;
        const std::uintptr_t heldItem = HeldItem(socketPose.player);
        const bool parentMatches = socketPose.player == parent;
        const bool childMatches =
            heldItem == reinterpret_cast<std::uintptr_t>(child);

        if ((parentMatches || childMatches ||
                (std::min)(vanillaDistance, correctedDistance) < 1.5f) &&
            (g_lastNearbyAttachmentParent != parent ||
                g_lastNearbyAttachmentChild != child) &&
            g_nearbyAttachmentLogs.fetch_add(1,
                std::memory_order_relaxed) < 32)
        {
            g_lastNearbyAttachmentParent = parent;
            g_lastNearbyAttachmentChild = child;
            const auto* parentVtable =
                *reinterpret_cast<std::uintptr_t* const*>(parent);
            std::ostringstream message;
            message << "Nearby entity attachment-transform call: player="
                << socketPose.player << " held_item="
                << reinterpret_cast<void*>(heldItem) << " parent=" << parent
                << " child=" << child << " link=" << reinterpret_cast<void*>(link)
                << " parent_match=" << parentMatches
                << " child_match=" << childMatches
                << " vanilla_distance=" << vanillaDistance
                << " corrected_distance=" << correctedDistance
                << " parent_vtable=DayZ+0x" << std::hex
                << (reinterpret_cast<std::uintptr_t>(parentVtable) - g_moduleBase)
                << " caller=DayZ+0x"
                << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase);
            logging::Info(message.str());
        }

        if (!parentMatches || !childMatches)
            return result;

        const std::uintptr_t itemValue =
            reinterpret_cast<std::uintptr_t>(child);
        if (g_loggedProxyItem.exchange(itemValue, std::memory_order_relaxed) !=
            itemValue)
        {
            std::ostringstream message;
            message << "Held-item proxy attachment classification: player="
                << parent << " item=" << child << " provider="
                << reinterpret_cast<void*>(socketPose.provider)
                << " vanilla_distance=" << vanillaDistance
                << " corrected_distance=" << correctedDistance
                << " correct_proxy=" << staleVanillaProxy;
            logging::Info(message.str());
        }
        if (!staleVanillaProxy)
            return result;

        const vr::pose::Quaternion inverseVanilla =
            vanillaSocketWorld.rotation.Inverse();
        const vr::pose::Transform socketToAttachment{
            inverseVanilla.Rotate(attachmentWorld.translation -
                vanillaSocketWorld.translation),
            (inverseVanilla * attachmentWorld.rotation).Normalized()};
        const vr::pose::Transform targetAttachment =
            Compose(correctedSocketWorld, socketToAttachment);
        StoreRotation(result, targetAttachment.rotation);
        result[9] = targetAttachment.translation.x;
        result[10] = targetAttachment.translation.y;
        result[11] = targetAttachment.translation.z;

        if (!g_loggedHeldItemProxyCorrection.exchange(true,
                std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Right-arm IK corrected held-item proxy attachment: player="
                << parent << " item=" << child << " provider="
                << reinterpret_cast<void*>(socketPose.provider)
                << " source=Entity::GetAttachmentTransform";
            logging::Info(message.str());
        }
        return result;
    }

    std::int64_t __fastcall HookedPlayerProxyTransform(void* player,
        std::int64_t argument2, std::int64_t modelOutput,
        std::int64_t* entityOutput, unsigned selection, float* proxyTransform,
        float* parentTransform, unsigned proxyIndex)
    {
        const std::int64_t result = g_playerProxyTransform(player, argument2,
            modelOutput, entityOutput, selection, proxyTransform, parentTransform,
            proxyIndex);
        if (!g_active.load(std::memory_order_relaxed) ||
            g_mode != ProbeMode::rightArmIk || !player || !proxyTransform)
            return result;

        const unsigned call =
            g_playerProxyCalls.fetch_add(1, std::memory_order_relaxed);
        if (call == 0)
        {
            std::ostringstream message;
            message << "DayZPlayer proxy-transform hook received its first call: player="
                << player << " selection=" << selection
                << " proxy_index=" << proxyIndex << " result="
                << reinterpret_cast<void*>(result) << " caller=DayZ+0x" << std::hex
                << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase);
            logging::Info(message.str());
        }

        SimulationSocketPose socketPose{};
        {
            std::lock_guard<std::mutex> lock(g_latestSocketPoseMutex);
            socketPose = g_latestSocketPose;
        }
        if (!socketPose.valid || socketPose.player != player)
            return result;

        const float* playerMatrix = EntityTransform(player);
        if (!playerMatrix)
            return result;
        const vr::pose::Transform playerWorld{
            Translation(playerMatrix), Rotation(playerMatrix)};
        const vr::pose::Transform proxyWorld{
            Translation(proxyTransform), Rotation(proxyTransform)};
        if (!playerWorld.translation.IsFinite() ||
            !playerWorld.rotation.IsFinite() ||
            !proxyWorld.translation.IsFinite() ||
            !proxyWorld.rotation.IsFinite())
            return result;

        const vr::pose::Transform vanillaSocketWorld =
            Compose(playerWorld, socketPose.vanilla);
        const vr::pose::Transform correctedSocketWorld =
            Compose(playerWorld, socketPose.corrected);
        const float vanillaDistance = (proxyWorld.translation -
            vanillaSocketWorld.translation).Length();
        const float correctedDistance = (proxyWorld.translation -
            correctedSocketWorld.translation).Length();
        const std::uintptr_t heldItem = HeldItem(player);
        const std::uintptr_t returnedEntity =
            static_cast<std::uintptr_t>(result);
        const std::uintptr_t outputEntity = entityOutput
            ? static_cast<std::uintptr_t>(*entityOutput) : 0;
        const bool entityMatches = heldItem &&
            (returnedEntity == heldItem || outputEntity == heldItem);

        if ((g_lastPlayerProxyResult != returnedEntity ||
                g_lastPlayerProxySelection != selection ||
                g_lastPlayerProxyIndex != proxyIndex) &&
            g_playerProxyCandidateLogs.fetch_add(1,
                std::memory_order_relaxed) < 48)
        {
            g_lastPlayerProxyResult = returnedEntity;
            g_lastPlayerProxySelection = selection;
            g_lastPlayerProxyIndex = proxyIndex;
            std::ostringstream message;
            message << "DayZPlayer proxy-transform candidate: player=" << player
                << " held_item=" << reinterpret_cast<void*>(heldItem)
                << " returned_entity=" << reinterpret_cast<void*>(returnedEntity)
                << " output_entity=" << reinterpret_cast<void*>(outputEntity)
                << " selection=" << selection << " proxy_index=" << proxyIndex
                << " entity_match=" << entityMatches
                << " vanilla_distance=" << vanillaDistance
                << " corrected_distance=" << correctedDistance
                << " caller=DayZ+0x" << std::hex
                << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase);
            logging::Info(message.str());
        }

        const bool staleVanillaProxy = vanillaDistance < 1.0f &&
            vanillaDistance + 0.03f < correctedDistance;
        if (!entityMatches || !staleVanillaProxy)
            return result;

        const vr::pose::Quaternion inverseVanilla =
            vanillaSocketWorld.rotation.Inverse();
        const vr::pose::Transform socketToProxy{
            inverseVanilla.Rotate(proxyWorld.translation -
                vanillaSocketWorld.translation),
            (inverseVanilla * proxyWorld.rotation).Normalized()};
        const vr::pose::Transform targetProxy =
            Compose(correctedSocketWorld, socketToProxy);
        StoreRotation(proxyTransform, targetProxy.rotation);
        proxyTransform[9] = targetProxy.translation.x;
        proxyTransform[10] = targetProxy.translation.y;
        proxyTransform[11] = targetProxy.translation.z;

        if (!g_loggedPlayerProxyCorrection.exchange(true,
                std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Right-arm IK corrected DayZPlayer held-item model proxy: player="
                << player << " item=" << reinterpret_cast<void*>(heldItem)
                << " selection=" << selection << " proxy_index=" << proxyIndex;
            logging::Info(message.str());
        }
        return result;
    }

    float* __fastcall HookedPlayerProxyLocalTransform(void* player, float* output,
        std::int64_t selection, float* scratch, unsigned proxyIndex)
    {
        float* result = g_playerProxyLocalTransform(player, output, selection,
            scratch, proxyIndex);
        if (!g_active.load(std::memory_order_relaxed) ||
            g_mode != ProbeMode::rightArmIk || !player || !result ||
            g_currentResolvedProxyPlayer != player ||
            !g_currentResolvedProxyRenderPath)
            return result;

        const std::uintptr_t heldItem = HeldItem(player);
        const bool heldMatch = heldItem &&
            (heldItem == g_currentResolvedProxyPrimary ||
                heldItem == g_currentResolvedProxySecondary);
        if (!heldMatch)
            return result;

        SimulationSocketPose socketPose{};
        {
            std::lock_guard<std::mutex> lock(g_latestSocketPoseMutex);
            socketPose = g_latestSocketPose;
        }
        if (!socketPose.valid || socketPose.player != player)
            return result;

        const vr::pose::Transform proxyLocal{
            Translation(result), Rotation(result)};
        if (!proxyLocal.translation.IsFinite() ||
            !proxyLocal.rotation.IsFinite())
            return result;

        const float vanillaDistance =
            (proxyLocal.translation - socketPose.vanilla.translation).Length();
        const float correctedDistance =
            (proxyLocal.translation - socketPose.corrected.translation).Length();
        if (g_playerProxyLocalTransformLogs.fetch_add(1,
                std::memory_order_relaxed) < 48)
        {
            std::ostringstream message;
            message << "Held-item render proxy local transform: player=" << player
                << " item=" << reinterpret_cast<void*>(heldItem)
                << " primary="
                << reinterpret_cast<void*>(g_currentResolvedProxyPrimary)
                << " secondary="
                << reinterpret_cast<void*>(g_currentResolvedProxySecondary)
                << " selection=" << selection << " proxy_index=" << proxyIndex
                << " vanilla_distance=" << vanillaDistance
                << " corrected_distance=" << correctedDistance
                << " caller=DayZ+0x" << std::hex
                << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
                    g_moduleBase);
            logging::Info(message.str());
        }

        const bool staleVanillaProxy = vanillaDistance < 1.0f &&
            vanillaDistance + 0.03f < correctedDistance;
        if (!staleVanillaProxy)
            return result;

        // Preserve the proxy's authored offset from the vanilla hand socket, but
        // move that offset into the corrected VR hand socket.
        const vr::pose::Quaternion inverseVanilla =
            socketPose.vanilla.rotation.Inverse();
        const vr::pose::Transform socketToProxy{
            inverseVanilla.Rotate(proxyLocal.translation -
                socketPose.vanilla.translation),
            (inverseVanilla * proxyLocal.rotation).Normalized()};
        const vr::pose::Transform targetProxy =
            Compose(socketPose.corrected, socketToProxy);
        if (!targetProxy.translation.IsFinite() ||
            !targetProxy.rotation.IsFinite())
            return result;

        StoreRotation(result, targetProxy.rotation);
        result[9] = targetProxy.translation.x;
        result[10] = targetProxy.translation.y;
        result[11] = targetProxy.translation.z;
        if (!g_loggedPlayerProxyLocalCorrection.exchange(true,
                std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Right-arm IK corrected held-item render-proxy matrix: player="
                << player << " item=" << reinterpret_cast<void*>(heldItem)
                << " selection=" << selection << " proxy_index=" << proxyIndex
                << " source=DayZPlayer::BuildProxyLocalTransform";
            logging::Info(message.str());
        }
        return result;
    }

    void __fastcall HookedPlayerProxyResolver(void* player, std::int64_t output,
        std::int64_t* descriptor, bool renderPath)
    {
        g_playerProxyResolver(player, output, descriptor, renderPath);
        if (!player || !output)
            return;

        const std::uintptr_t primary =
            *reinterpret_cast<const std::uintptr_t*>(output);
        const std::uintptr_t secondary =
            *reinterpret_cast<const std::uintptr_t*>(output + 8);
        g_currentResolvedProxyPlayer = player;
        g_currentResolvedProxyPrimary = primary;
        g_currentResolvedProxySecondary = secondary;
        g_currentResolvedProxyRenderPath = renderPath;

        if (!g_active.load(std::memory_order_relaxed) ||
            g_mode != ProbeMode::rightArmIk)
            return;

        SimulationSocketPose socketPose{};
        {
            std::lock_guard<std::mutex> lock(g_latestSocketPoseMutex);
            socketPose = g_latestSocketPose;
        }
        if (!socketPose.valid || socketPose.player != player)
            return;

        if (primary == g_lastResolvedProxyPrimary &&
            secondary == g_lastResolvedProxySecondary)
            return;
        g_lastResolvedProxyPrimary = primary;
        g_lastResolvedProxySecondary = secondary;
        if (g_playerProxyResolverLogs.fetch_add(1,
                std::memory_order_relaxed) >= 48)
            return;

        const std::uintptr_t heldItem = HeldItem(player);
        const auto* playerVtable =
            *reinterpret_cast<std::uintptr_t* const*>(player);
        std::ostringstream message;
        message << "DayZPlayer proxy resolver: player=" << player
            << " held_item=" << reinterpret_cast<void*>(heldItem)
            << " primary=" << reinterpret_cast<void*>(primary)
            << " secondary=" << reinterpret_cast<void*>(secondary)
            << " descriptor=" << descriptor
            << " render_path=" << renderPath
            << " held_match=" << (heldItem &&
                (heldItem == primary || heldItem == secondary))
            << " player_vtable=DayZ+0x" << std::hex
            << (reinterpret_cast<std::uintptr_t>(playerVtable) - g_moduleBase)
            << " slots_116_117_189=DayZ+0x"
            << (playerVtable[116] - g_moduleBase) << ",DayZ+0x"
            << (playerVtable[117] - g_moduleBase) << ",DayZ+0x"
            << (playerVtable[189] - g_moduleBase)
            << " caller=DayZ+0x"
            << (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase);
        logging::Info(message.str());
    }

    bool CorrectHeldItemWorldTransform(void* player,
        const SimulationSocketPose& socketPose) noexcept
    {
        if (!player || !socketPose.valid || socketPose.player != player ||
            !g_setEntityTransform)
            return false;
        const bool logDiagnostic =
            g_loggedAttachmentAttemptProvider != socketPose.provider;
        if (logDiagnostic)
            g_loggedAttachmentAttemptProvider = socketPose.provider;
        const auto logFailure = [&](const char* reason) noexcept
        {
            if (!logDiagnostic)
                return;
            std::ostringstream message;
            message << "Held-item attachment diagnostic: player=" << player
                << " provider=" << reinterpret_cast<void*>(socketPose.provider)
                << " result=skipped reason=" << reason;
            logging::Info(message.str());
        };

        const auto* playerVtable =
            *reinterpret_cast<std::uintptr_t* const*>(player);
        if (!playerVtable || !playerVtable[238])
        {
            logFailure("missing_player_hands_method");
            return false;
        }
        using GetHandsOwnerFn = std::uintptr_t(__fastcall*)(void*);
        const std::uintptr_t handsOwner =
            reinterpret_cast<GetHandsOwnerFn>(playerVtable[238])(player);
        const std::uintptr_t item = handsOwner
            ? *reinterpret_cast<const std::uintptr_t*>(handsOwner + 432)
            : 0;
        if (!item)
        {
            g_heldItemCalibration = {};
            logFailure(handsOwner ? "hands_owner_has_no_entity" :
                "missing_hands_owner");
            return false;
        }

        const float* playerMatrix = EntityTransform(player);
        const float* itemMatrix = EntityTransform(reinterpret_cast<void*>(item));
        float itemRenderMatrix[12]{};
        if (!playerMatrix || !itemMatrix ||
            !EntityRenderTransform(reinterpret_cast<void*>(item), itemRenderMatrix))
        {
            logFailure(!playerMatrix ? "missing_player_transform" :
                (!itemMatrix ? "missing_item_transform" :
                    "missing_item_render_transform"));
            return false;
        }
        const vr::pose::Transform playerWorld{
            Translation(playerMatrix), Rotation(playerMatrix)};
        const vr::pose::Transform itemWorld{
            Translation(itemMatrix), Rotation(itemMatrix)};
        const vr::pose::Transform itemRenderWorld{
            Translation(itemRenderMatrix), Rotation(itemRenderMatrix)};
        if (!playerWorld.translation.IsFinite() ||
            !itemWorld.translation.IsFinite() ||
            !itemRenderWorld.translation.IsFinite() ||
            !playerWorld.rotation.IsFinite() || !itemWorld.rotation.IsFinite() ||
            !itemRenderWorld.rotation.IsFinite())
        {
            logFailure("non_finite_entity_transform");
            return false;
        }

        const vr::pose::Transform vanillaSocketWorld =
            Compose(playerWorld, socketPose.vanilla);
        const vr::pose::Transform correctedSocketWorld =
            Compose(playerWorld, socketPose.corrected);
        if (!g_heldItemCalibration.valid ||
            g_heldItemCalibration.item != item ||
            g_heldItemCalibration.provider != socketPose.provider)
        {
            const float logicalVanillaDistance =
                (itemWorld.translation - vanillaSocketWorld.translation).Length();
            const float logicalCorrectedDistance =
                (itemWorld.translation - correctedSocketWorld.translation).Length();
            g_heldItemCalibration = {};
            g_heldItemCalibration.item = item;
            g_heldItemCalibration.provider = socketPose.provider;
            const vr::pose::Transform& logicalSocket =
                logicalCorrectedDistance <= logicalVanillaDistance
                    ? correctedSocketWorld : vanillaSocketWorld;
            const vr::pose::Quaternion inverseSocket =
                logicalSocket.rotation.Inverse();
            g_heldItemCalibration.socketToItem.translation =
                inverseSocket.Rotate(itemWorld.translation -
                    logicalSocket.translation);
            g_heldItemCalibration.socketToItem.rotation =
                (inverseSocket * itemWorld.rotation).Normalized();
            const vr::pose::Transform expectedVanillaItem = Compose(
                vanillaSocketWorld, g_heldItemCalibration.socketToItem);
            const vr::pose::Transform expectedCorrectedItem = Compose(
                correctedSocketWorld, g_heldItemCalibration.socketToItem);
            const float renderVanillaDistance = (itemRenderWorld.translation -
                expectedVanillaItem.translation).Length();
            const float renderCorrectedDistance = (itemRenderWorld.translation -
                expectedCorrectedItem.translation).Length();
            g_heldItemCalibration.forceWorldTransform =
                renderVanillaDistance + 0.03f < renderCorrectedDistance;
            g_heldItemCalibration.valid = true;

            std::ostringstream message;
            message << "Held-item attachment classification: player=" << player
                << " provider=" << reinterpret_cast<void*>(socketPose.provider)
                << " hands_owner=" << reinterpret_cast<void*>(handsOwner)
                << " item=" << reinterpret_cast<void*>(item)
                << " logical_vanilla_distance=" << logicalVanillaDistance
                << " logical_corrected_distance=" << logicalCorrectedDistance
                << " render_vanilla_distance=" << renderVanillaDistance
                << " render_corrected_distance=" << renderCorrectedDistance
                << " force_world_transform="
                << g_heldItemCalibration.forceWorldTransform;
            logging::Info(message.str());
        }

        // Menu-preview attachment already consumes the corrected model pose. Only
        // force the entity when its current transform is measurably closer to the
        // vanilla socket, which is the in-world behavior observed at runtime.
        if (!g_heldItemCalibration.forceWorldTransform)
        {
            logFailure("entity_classified_as_already_corrected");
            return false;
        }

        const vr::pose::Transform targetItemWorld = Compose(correctedSocketWorld,
            g_heldItemCalibration.socketToItem);
        float targetMatrix[12]{};
        StoreRotation(targetMatrix, targetItemWorld.rotation);
        targetMatrix[9] = targetItemWorld.translation.x;
        targetMatrix[10] = targetItemWorld.translation.y;
        targetMatrix[11] = targetItemWorld.translation.z;
        bool transformUpdated =
            g_setEntityTransform(reinterpret_cast<void*>(item), targetMatrix) != 0;
        if (!transformUpdated)
        {
            // The logical transform can already equal the target while the render
            // interpolation cache is stale. Force the normal engine update path
            // without exposing an intermediate pose to rendering.
            float nudgeMatrix[12]{};
            std::copy_n(targetMatrix, 12, nudgeMatrix);
            nudgeMatrix[9] += 0.001f;
            const bool nudged = g_setEntityTransform(
                reinterpret_cast<void*>(item), nudgeMatrix) != 0;
            const bool restored = g_setEntityTransform(
                reinterpret_cast<void*>(item), targetMatrix) != 0;
            transformUpdated = nudged && restored;
        }
        if (!transformUpdated)
        {
            logFailure("engine_set_transform_rejected");
            return false;
        }

        if (!g_loggedHeldItemWorldCorrection.exchange(true,
                std::memory_order_relaxed))
        {
            std::ostringstream message;
            message << "Right-arm IK corrected held-item world transform: player="
                << player << " item=" << reinterpret_cast<void*>(item)
                << " provider=" << reinterpret_cast<void*>(socketPose.provider)
                << " stage=post_player_simulation";
            logging::Info(message.str());
        }
        return true;
    }

    void __fastcall HookedFinalPlayerSimulation(void* player, float deltaTime)
    {
        void* previousPlayer = g_currentSimulationPlayer;
        const SimulationSocketPose previousPose = g_simulationSocketPose;
        g_currentSimulationPlayer = player;
        g_simulationSocketPose = {};
        g_finalPlayerSimulation(player, deltaTime);
        if (g_active.load(std::memory_order_relaxed) &&
            g_mode == ProbeMode::rightArmIk)
        {
            if (g_simulationSocketPose.valid)
            {
                std::lock_guard<std::mutex> lock(g_latestSocketPoseMutex);
                g_latestSocketPose = g_simulationSocketPose;
            }
            if (!g_simulationSocketPose.valid &&
                g_loggedMissingSocketPlayer != player)
            {
                g_loggedMissingSocketPlayer = player;
                std::ostringstream message;
                message << "Held-item attachment diagnostic: player=" << player
                    << " result=skipped reason=no_socket_pose_from_nested_human_update";
                logging::Info(message.str());
            }
            CorrectHeldItemWorldTransform(player, g_simulationSocketPose);
        }
        g_currentSimulationPlayer = previousPlayer;
        g_simulationSocketPose = previousPose;
    }
}

namespace dayz::human_pose_probe
{
    bool Initialize(std::uintptr_t moduleBase, std::uint32_t peTimestamp,
        std::uint32_t imageSize) noexcept
    {
        if (!ReadBoolean(L"enabled", false))
        {
            logging::Info("Human pose probe disabled by [human_pose_probe]");
            return false;
        }
        for (const auto& profile : dayz::offsets::kBuildProfiles)
            if (profile.peTimestamp == peTimestamp && profile.imageSize == imageSize)
            {
                g_buildProfile = &profile;
                break;
            }
        if (!g_buildProfile)
        {
            logging::Error("Human pose probe rejected unsupported DayZ executable");
            return false;
        }
        kProviderBoneMapOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerBoneMapOffset);
        kProviderParentMapOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerParentMapOffset);
        kProviderBoneCountOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerBoneCountOffset);
        kProviderExtraBoneCountOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerExtraBoneCountOffset);
        kProviderModelTransformsOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerModelTransformsOffset);
        kProviderSkinningTransformsOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerSkinningTransformsOffset);
        kProviderPoseDirtyOffset = static_cast<std::ptrdiff_t>(
            g_buildProfile->providerPoseDirtyOffset);
        g_moduleBase = moduleBase;
        if (!g_moduleBase || !ValidateInternals())
        {
            logging::Error("Human pose probe rejected internals: signature/vtable mismatch");
            return false;
        }
        g_setEntityTransform = reinterpret_cast<SetEntityTransformFn>(
            g_moduleBase + g_buildProfile->setEntityTransformRva);

        const int configuredBone = GetPrivateProfileIntW(L"human_pose_probe", L"bone_index",
            117, ConfigurationFile().c_str());
        if (configuredBone < 0 || configuredBone >
                static_cast<int>(kMaximumPublicBoneIndex))
        {
            logging::Error("Human pose probe rejected bone_index outside 0..249");
            return false;
        }
        g_boneIndex = static_cast<unsigned>(configuredBone);
        const std::wstring mode = ReadString(L"mode", L"translation");
        if (_wcsicmp(mode.c_str(), L"right_arm_ik") == 0)
            g_mode = ProbeMode::rightArmIk;
        else if (_wcsicmp(mode.c_str(), L"translation") == 0)
            g_mode = ProbeMode::translation;
        else
        {
            logging::Error("Human pose probe rejected unknown mode");
            return false;
        }
        g_translationX = (std::clamp)(ReadFloat(L"translation_x", 0.0f), -0.25f, 0.25f);
        g_translationY = (std::clamp)(ReadFloat(L"translation_y", 0.10f), -0.25f, 0.25f);
        g_translationZ = (std::clamp)(ReadFloat(L"translation_z", 0.0f), -0.25f, 0.25f);
        g_frequencyHz = (std::clamp)(ReadFloat(L"frequency_hz", 0.75f), 0.05f, 4.0f);
        g_ikTargetScale = (std::clamp)(ReadFloat(L"ik_target_scale", 1.0f), 0.25f, 2.0f);
        g_ikWeight = (std::clamp)(ReadFloat(L"ik_weight", 1.0f), 0.0f, 1.0f);
        g_ikTargetOffset = {ReadFloat(L"ik_offset_x", 0.0f),
            ReadFloat(L"ik_offset_y", 0.0f), ReadFloat(L"ik_offset_z", 0.0f)};
        g_gripToWristOffset = {ReadFloat(L"ik_grip_to_wrist_x", 0.0f),
            ReadFloat(L"ik_grip_to_wrist_y", 0.0f),
            ReadFloat(L"ik_grip_to_wrist_z", 0.0f)};
        const float gripRotationX = ReadFloat(L"ik_grip_rotation_x", 0.0f);
        const float gripRotationY = ReadFloat(L"ik_grip_rotation_y", 0.0f);
        const float gripRotationZ = ReadFloat(L"ik_grip_rotation_z", 0.0f);
        g_gripRotationOffset = EulerDegrees(gripRotationX, gripRotationY, gripRotationZ);
        if (g_mode == ProbeMode::translation && g_translationX == 0.0f &&
            g_translationY == 0.0f && g_translationZ == 0.0f)
        {
            logging::Error("Human pose probe rejected a zero translation amplitude");
            return false;
        }

        void* exportTarget = reinterpret_cast<void*>(
            g_moduleBase + g_buildProfile->skinningExportRva);
        const MH_STATUS created = MH_CreateHook(exportTarget, HookedSkinningExport,
            reinterpret_cast<void**>(&g_skinningExport));
        if (created != MH_OK)
        {
            logging::Error("Human pose probe could not create the skinning export hook");
            return false;
        }
        void* animationTarget{};
        void* finalSimulationTarget{};
        void* attachmentTransformTarget{};
        void* playerProxyTransformTarget{};
        if (g_mode == ProbeMode::rightArmIk)
        {
            if (!g_buildProfile->entityAttachmentTransformRva ||
                !g_buildProfile->playerProxyTransformRva ||
                !g_buildProfile->playerProxyLocalTransformRva)
            {
                MH_RemoveHook(exportTarget);
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe has no proxy-transform RVA for this build");
                return false;
            }
            animationTarget = reinterpret_cast<void*>(
                g_moduleBase + g_buildProfile->humanAnimationUpdateRva);
            const MH_STATUS animationCreated = MH_CreateHook(animationTarget,
                HookedHumanAnimationUpdate,
                reinterpret_cast<void**>(&g_humanAnimationUpdate));
            if (animationCreated != MH_OK)
            {
                MH_RemoveHook(exportTarget);
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not create the Human animation hook");
                return false;
            }
            finalSimulationTarget = reinterpret_cast<void*>(
                g_moduleBase + g_buildProfile->finalPlayerSimulationRva);
            const MH_STATUS finalSimulationCreated = MH_CreateHook(
                finalSimulationTarget, HookedFinalPlayerSimulation,
                reinterpret_cast<void**>(&g_finalPlayerSimulation));
            if (finalSimulationCreated != MH_OK)
            {
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_humanAnimationUpdate = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not create the final player-simulation hook");
                return false;
            }
            attachmentTransformTarget = reinterpret_cast<void*>(
                g_moduleBase + g_buildProfile->entityAttachmentTransformRva);
            const MH_STATUS attachmentCreated = MH_CreateHook(
                attachmentTransformTarget, HookedEntityAttachmentTransform,
                reinterpret_cast<void**>(&g_entityAttachmentTransform));
            if (attachmentCreated != MH_OK)
            {
                MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not create the entity attachment-transform hook");
                return false;
            }
            playerProxyTransformTarget = reinterpret_cast<void*>(
                g_moduleBase + g_buildProfile->playerProxyTransformRva);
            const MH_STATUS playerProxyCreated = MH_CreateHook(
                playerProxyTransformTarget, HookedPlayerProxyResolver,
                reinterpret_cast<void**>(&g_playerProxyResolver));
            if (playerProxyCreated != MH_OK)
            {
                MH_RemoveHook(attachmentTransformTarget);
                MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_entityAttachmentTransform = nullptr;
                g_playerProxyResolver = nullptr;
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not create the player proxy-transform hook");
                return false;
            }
        }
        const MH_STATUS enabled = MH_EnableHook(exportTarget);
        if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        {
            if (playerProxyTransformTarget)
                MH_RemoveHook(playerProxyTransformTarget);
            if (attachmentTransformTarget)
                MH_RemoveHook(attachmentTransformTarget);
            if (finalSimulationTarget)
                MH_RemoveHook(finalSimulationTarget);
            if (animationTarget)
                MH_RemoveHook(animationTarget);
            MH_RemoveHook(exportTarget);
            g_finalPlayerSimulation = nullptr;
            g_humanAnimationUpdate = nullptr;
            g_entityAttachmentTransform = nullptr;
            g_playerProxyTransform = nullptr;
            g_playerProxyResolver = nullptr;
            g_skinningExport = nullptr;
            logging::Error("Human pose probe could not enable the skinning export hook");
            return false;
        }
        if (animationTarget)
        {
            const MH_STATUS animationEnabled = MH_EnableHook(animationTarget);
            if (animationEnabled != MH_OK && animationEnabled != MH_ERROR_ENABLED)
            {
                MH_DisableHook(exportTarget);
                if (playerProxyTransformTarget)
                    MH_RemoveHook(playerProxyTransformTarget);
                if (attachmentTransformTarget)
                    MH_RemoveHook(attachmentTransformTarget);
                if (finalSimulationTarget)
                    MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_entityAttachmentTransform = nullptr;
                g_playerProxyTransform = nullptr;
                g_playerProxyResolver = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not enable the Human animation hook");
                return false;
            }
        }
        if (finalSimulationTarget)
        {
            const MH_STATUS finalSimulationEnabled =
                MH_EnableHook(finalSimulationTarget);
            if (finalSimulationEnabled != MH_OK &&
                finalSimulationEnabled != MH_ERROR_ENABLED)
            {
                MH_DisableHook(animationTarget);
                MH_DisableHook(exportTarget);
                if (playerProxyTransformTarget)
                    MH_RemoveHook(playerProxyTransformTarget);
                if (attachmentTransformTarget)
                    MH_RemoveHook(attachmentTransformTarget);
                MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_entityAttachmentTransform = nullptr;
                g_playerProxyTransform = nullptr;
                g_playerProxyResolver = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not enable the final player-simulation hook");
                return false;
            }
        }
        if (attachmentTransformTarget)
        {
            const MH_STATUS attachmentEnabled =
                MH_EnableHook(attachmentTransformTarget);
            if (attachmentEnabled != MH_OK &&
                attachmentEnabled != MH_ERROR_ENABLED)
            {
                MH_DisableHook(finalSimulationTarget);
                MH_DisableHook(animationTarget);
                MH_DisableHook(exportTarget);
                MH_RemoveHook(attachmentTransformTarget);
                MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_entityAttachmentTransform = nullptr;
                if (playerProxyTransformTarget)
                    MH_RemoveHook(playerProxyTransformTarget);
                g_playerProxyTransform = nullptr;
                g_playerProxyResolver = nullptr;
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not enable the entity attachment-transform hook");
                return false;
            }
        }
        if (playerProxyTransformTarget)
        {
            const MH_STATUS playerProxyEnabled =
                MH_EnableHook(playerProxyTransformTarget);
            if (playerProxyEnabled != MH_OK &&
                playerProxyEnabled != MH_ERROR_ENABLED)
            {
                MH_DisableHook(attachmentTransformTarget);
                MH_DisableHook(finalSimulationTarget);
                MH_DisableHook(animationTarget);
                MH_DisableHook(exportTarget);
                MH_RemoveHook(playerProxyTransformTarget);
                MH_RemoveHook(attachmentTransformTarget);
                MH_RemoveHook(finalSimulationTarget);
                MH_RemoveHook(animationTarget);
                MH_RemoveHook(exportTarget);
                g_playerProxyTransform = nullptr;
                g_playerProxyResolver = nullptr;
                g_entityAttachmentTransform = nullptr;
                g_finalPlayerSimulation = nullptr;
                g_humanAnimationUpdate = nullptr;
                g_skinningExport = nullptr;
                logging::Error(
                    "Human pose probe could not enable the player proxy-transform hook");
                return false;
            }
        }
        if (g_mode == ProbeMode::rightArmIk)
        {
            void* playerProxyLocalTarget = reinterpret_cast<void*>(
                g_moduleBase + g_buildProfile->playerProxyLocalTransformRva);
            const MH_STATUS localCreated = MH_CreateHook(
                playerProxyLocalTarget, HookedPlayerProxyLocalTransform,
                reinterpret_cast<void**>(&g_playerProxyLocalTransform));
            if (localCreated != MH_OK)
            {
                logging::Error(
                    "Human pose probe could not create the render-proxy local-transform hook");
                return false;
            }
            const MH_STATUS localEnabled = MH_EnableHook(playerProxyLocalTarget);
            if (localEnabled != MH_OK && localEnabled != MH_ERROR_ENABLED)
            {
                MH_RemoveHook(playerProxyLocalTarget);
                g_playerProxyLocalTransform = nullptr;
                logging::Error(
                    "Human pose probe could not enable the render-proxy local-transform hook");
                return false;
            }
        }
        g_active.store(true, std::memory_order_release);
        std::ostringstream message;
        message << g_buildProfile->name << " Human pose probe active: skinning_export=DayZ+0x"
            << std::hex << g_buildProfile->skinningExportRva
            << " human_animation=DayZ+0x" << g_buildProfile->humanAnimationUpdateRva
            << " final_player_simulation=DayZ+0x"
            << g_buildProfile->finalPlayerSimulationRva
            << " entity_attachment_transform=DayZ+0x"
            << g_buildProfile->entityAttachmentTransformRva
            << " player_proxy_transform=DayZ+0x"
            << g_buildProfile->playerProxyTransformRva
            << " player_proxy_local_transform=DayZ+0x"
            << g_buildProfile->playerProxyLocalTransformRva
            << std::dec << " mode=" <<
                (g_mode == ProbeMode::rightArmIk ? "right_arm_ik" : "translation")
            << " bone=" << g_boneIndex
            << " translation=" << g_translationX << ',' << g_translationY << ','
            << g_translationZ << " frequency_hz=" << g_frequencyHz
            << " ik_scale=" << g_ikTargetScale << " ik_weight=" << g_ikWeight
            << " ik_offset=" << g_ikTargetOffset.x << ',' << g_ikTargetOffset.y << ','
            << g_ikTargetOffset.z << " grip_to_wrist=" << g_gripToWristOffset.x << ','
            << g_gripToWristOffset.y << ',' << g_gripToWristOffset.z
            << " grip_rotation_deg=" << gripRotationX << ',' << gripRotationY << ','
            << gripRotationZ;
        logging::Info(message.str());
        return true;
    }

    bool IsActive() noexcept
    {
        return g_active.load(std::memory_order_acquire);
    }
}
