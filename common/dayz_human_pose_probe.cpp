#include "dayz_human_pose_probe.hpp"

#include "logging.hpp"
#include "stereo_state.hpp"

#include <vr_pose_solver/arm_solver.hpp>

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace
{
    constexpr std::uint32_t kRetailTimestamp = 0x6A47B9AAu;
    constexpr std::uint32_t kRetailImageSize = 0x04407000u;
    constexpr std::uintptr_t kSkinningExportRva = 0x004D1120;
    constexpr std::uintptr_t kPoseProviderVtableRva = 0x00C85F28;
    constexpr std::uintptr_t kGetLocalTransformRva = 0x004D0CE0;
    constexpr std::uintptr_t kSetLocalTransformRva = 0x004D9150;
    constexpr std::uintptr_t kRebuildModelTransformsRva = 0x004D9D60;

    constexpr std::ptrdiff_t kProviderBoneMapOffset = 2020;
    constexpr std::ptrdiff_t kProviderParentMapOffset = 1020;
    constexpr std::ptrdiff_t kProviderBoneCountOffset = 3084;
    constexpr std::ptrdiff_t kProviderExtraBoneCountOffset = 3088;
    constexpr std::ptrdiff_t kProviderModelTransformsOffset = 3048;
    constexpr std::ptrdiff_t kProviderSkinningTransformsOffset = 3064;
    constexpr std::ptrdiff_t kProviderPoseDirtyOffset = 3152;
    constexpr unsigned kMaximumPublicBoneIndex = 249;
    constexpr unsigned kRightArmBone = 113;
    constexpr unsigned kRightForeArmBone = 117;
    constexpr unsigned kRightHandBone = 122;
    constexpr unsigned kHeadBone = 17;

    enum class ProbeMode { translation, rightArmIk };

    using SkinningExportFn = std::int64_t(__fastcall*)(void*, std::int64_t*,
        std::int64_t, std::int64_t);
    using GetLocalTransformFn = void(__fastcall*)(void*, unsigned, float*);
    using SetLocalTransformFn = void(__fastcall*)(void*, unsigned, const float*);
    using RebuildModelTransformsFn = void(__fastcall*)(void*);

    std::uintptr_t g_moduleBase{};
    SkinningExportFn g_skinningExport{};
    std::atomic_bool g_active{};
    std::atomic_bool g_loggedFirstWrite{};
    std::atomic_bool g_loggedFirstRebuild{};
    std::atomic_bool g_loggedExportMapping{};
    std::atomic_uint64_t g_writeCount{};
    unsigned g_boneIndex{117};
    float g_translationX{};
    float g_translationY{0.10f};
    float g_translationZ{};
    float g_frequencyHz{0.75f};
    ProbeMode g_mode{ProbeMode::translation};
    float g_ikTargetScale{1.0f};
    float g_ikWeight{0.75f};
    vr::pose::Vec3 g_ikTargetOffset{};
    std::atomic<std::uintptr_t> g_ikProvider{};
    std::atomic_uint64_t g_ikProviderLastSeenMs{};
    std::atomic_flag g_wristCalibrationLock = ATOMIC_FLAG_INIT;
    std::uintptr_t g_wristCalibrationProvider{};
    vr::pose::Quaternion g_controllerToHandOffset{};
    vr::pose::Vec3 g_controllerLocalPoleDirection{0.0f, -1.0f, 0.0f};
    std::atomic<std::uintptr_t> g_loggedIkProvider{};

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
        vr::pose::Quaternion orientation{};
        vr::pose::Vec3 polePosition{};
    };

    CalibratedHandTarget CalibrateHandTarget(std::uintptr_t provider,
        const vr::pose::Quaternion& controllerOrientation,
        const vr::pose::Quaternion& currentHandOrientation,
        const vr::pose::Vec3& shoulderPosition,
        const vr::pose::Vec3& elbowPosition,
        const vr::pose::Vec3& targetPosition) noexcept
    {
        while (g_wristCalibrationLock.test_and_set(std::memory_order_acquire))
            YieldProcessor();
        const vr::pose::Vec3 reachDirection = vr::pose::NormalizeOr(
            targetPosition - shoulderPosition, {0.0f, 0.0f, 1.0f});
        const vr::pose::Vec3 currentElbow = elbowPosition - shoulderPosition;
        const vr::pose::Vec3 currentPole = vr::pose::NormalizeOr(currentElbow -
            reachDirection * vr::pose::Dot(currentElbow, reachDirection),
            vr::pose::StableOrthogonal(reachDirection));
        if (g_wristCalibrationProvider != provider)
        {
            g_controllerToHandOffset = (controllerOrientation.Inverse() *
                currentHandOrientation).Normalized();
            g_controllerLocalPoleDirection = controllerOrientation.Inverse().Rotate(
                currentPole);
            g_wristCalibrationProvider = provider;
            logging::Info("Right-arm IK calibrated hand orientation and elbow pole");
        }
        CalibratedHandTarget result{};
        result.orientation = (controllerOrientation *
            g_controllerToHandOffset).Normalized();
        const vr::pose::Vec3 rotatedPole = controllerOrientation.Rotate(
            g_controllerLocalPoleDirection);
        const vr::pose::Vec3 projectedPole = vr::pose::NormalizeOr(rotatedPole -
            reachDirection * vr::pose::Dot(rotatedPole, reachDirection), currentPole);
        const float poleDistance = (std::max)(0.5f,
            (targetPosition - shoulderPosition).Length());
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

    bool ValidateRetailInternals() noexcept
    {
        constexpr std::array<std::uint8_t, 16> skinningExportSignature{
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48};
        constexpr std::array<std::uint8_t, 16> getLocalSignature{
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x8B, 0xC2,
            0x49, 0x8B, 0xD8, 0x8B, 0x94, 0x81, 0xE4, 0x07};
        constexpr std::array<std::uint8_t, 16> setLocalSignature{
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x8B,
            0x40, 0x24, 0x48, 0x8B, 0xD9, 0x44, 0x8B, 0xD2};
        constexpr std::array<std::uint8_t, 16> rebuildSignature{
            0x48, 0x8B, 0xC4, 0x55, 0x41, 0x56, 0x48, 0x8D,
            0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00};
        if (!Match(kSkinningExportRva, skinningExportSignature.data(),
                skinningExportSignature.size()) ||
            !Match(kGetLocalTransformRva, getLocalSignature.data(), getLocalSignature.size()) ||
            !Match(kSetLocalTransformRva, setLocalSignature.data(), setLocalSignature.size()) ||
            !Match(kRebuildModelTransformsRva, rebuildSignature.data(),
                rebuildSignature.size()))
            return false;

        const auto* vtable = reinterpret_cast<const std::uintptr_t*>(
            g_moduleBase + kPoseProviderVtableRva);
        return vtable[14] == g_moduleBase + kGetLocalTransformRva &&
            vtable[17] == g_moduleBase + kSetLocalTransformRva;
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

    bool ApplyRightArmIk(std::uintptr_t provider,
        RebuildModelTransformsFn rebuild) noexcept
    {
        const dayz::stereo_state::ControllerPose controller =
            dayz::stereo_state::GetControllerPose(1);
        const dayz::stereo_state::HmdPosition hmd = dayz::stereo_state::GetHmdPosition();
        const dayz::stereo_state::HmdOrientation hmdOrientation =
            dayz::stereo_state::GetHmdOrientation();
        if (!controller.valid || !hmd.valid || !hmdOrientation.valid)
            return false;

        const std::uint32_t arm = InternalBone(provider, kRightArmBone);
        const std::uint32_t foreArm = InternalBone(provider, kRightForeArmBone);
        const std::uint32_t hand = InternalBone(provider, kRightHandBone);
        const std::uint32_t head = InternalBone(provider, kHeadBone);
        if (arm == UINT32_MAX || foreArm == UINT32_MAX || hand == UINT32_MAX ||
            head == UINT32_MAX)
            return false;

        if (!SelectIkProvider(provider))
            return false;

        rebuild(reinterpret_cast<void*>(provider));
        const float* armModel = ModelTransform(provider, arm);
        const float* foreArmModel = ModelTransform(provider, foreArm);
        const float* handModel = ModelTransform(provider, hand);
        const float* headModel = ModelTransform(provider, head);
        if (!armModel || !foreArmModel || !handModel || !headModel)
            return false;

        const vr::pose::Quaternion hmdQ{hmdOrientation.x, hmdOrientation.y,
            hmdOrientation.z, hmdOrientation.w};
        const vr::pose::Quaternion controllerQ{controller.orientationX,
            controller.orientationY, controller.orientationZ,
            controller.orientationW};
        const vr::pose::Quaternion controllerRelative =
            (hmdQ.Inverse() * controllerQ).Normalized();
        // Position mapping uses (x,y,-z). A rotation crossing that reflection has
        // quaternion vector part (-x,-y,z), with the scalar component unchanged.
        const vr::pose::Quaternion controllerModelOrientation{
            -controllerRelative.x, -controllerRelative.y,
            controllerRelative.z, controllerRelative.w};
        const vr::pose::Vec3 trackingDelta{
            controller.positionX - hmd.x,
            controller.positionY - hmd.y,
            controller.positionZ - hmd.z};
        const vr::pose::Vec3 headRelative = hmdQ.Inverse().Rotate(trackingDelta);
        const vr::pose::Vec3 targetPosition = Translation(headModel) +
            vr::pose::Vec3{headRelative.x, headRelative.y, -headRelative.z} *
                g_ikTargetScale + g_ikTargetOffset;

        vr::pose::TwoBoneChain chain{};
        chain.start.translation = Translation(armModel);
        chain.start.rotation = Rotation(armModel);
        chain.middle.translation = Translation(foreArmModel);
        chain.middle.rotation = Rotation(foreArmModel);
        chain.end.translation = Translation(handModel);
        chain.end.rotation = Rotation(handModel);
        const CalibratedHandTarget calibratedTarget = CalibrateHandTarget(provider,
            controllerModelOrientation, chain.end.rotation, chain.start.translation,
            chain.middle.translation, targetPosition);
        vr::pose::TwoBoneTarget target{};
        target.position = targetPosition;
        target.orientation = calibratedTarget.orientation;
        target.pole_position = calibratedTarget.polePosition;
        target.weight = g_ikWeight;
        target.match_end_orientation = true;
        vr::pose::TwoBoneConstraints constraints{};
        constraints.minimum_bend_radians = 0.03f;
        constraints.maximum_bend_radians = 2.85f;
        constraints.soften_start_ratio = 0.92f;
        const vr::pose::TwoBoneResult solved = vr::pose::SolveTwoBone(chain, target,
            constraints);
        if (!solved.Succeeded())
            return false;

        const std::uint32_t boneCount =
            *reinterpret_cast<const std::uint32_t*>(provider + kProviderBoneCountOffset) +
            *reinterpret_cast<const std::uint32_t*>(provider + kProviderExtraBoneCountOffset);
        const auto skinningTransforms = *reinterpret_cast<const std::uintptr_t*>(
            provider + kProviderSkinningTransformsOffset);
        if (!skinningTransforms || boneCount == 0 || boneCount > 512)
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

        // Work on the exact palette consumed by the hooked export. This avoids the
        // animation graph overwriting local rotations between its setter and rendering.
        // First rotate the complete arm subtree around the shoulder, then rotate the
        // forearm subtree around the already-solved elbow.
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (IsBoneOrDescendant(provider, bone, arm, boneCount))
            {
                auto* matrix = reinterpret_cast<float*>(skinningTransforms +
                    sizeof(float) * 12 * bone);
                RotateAffineAround(matrix, solved.start_correction,
                    chain.start.translation);
            }
        }
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (IsBoneOrDescendant(provider, bone, foreArm, boneCount))
            {
                auto* matrix = reinterpret_cast<float*>(skinningTransforms +
                    sizeof(float) * 12 * bone);
                RotateAffineAround(matrix, solved.middle_correction,
                    solved.solved_middle_position);
            }
        }
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (IsBoneOrDescendant(provider, bone, foreArm, boneCount))
            {
                auto* matrix = reinterpret_cast<float*>(skinningTransforms +
                    sizeof(float) * 12 * bone);
                RotateAffineAround(matrix, distributedForeArmTwist,
                    solved.solved_middle_position);
            }
        }
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (IsBoneOrDescendant(provider, bone, hand, boneCount))
            {
                auto* matrix = reinterpret_cast<float*>(skinningTransforms +
                    sizeof(float) * 12 * bone);
                RotateAffineAround(matrix, residualHandCorrection,
                    solved.solved_end_position);
            }
        }

        if (g_loggedIkProvider.exchange(provider, std::memory_order_relaxed) != provider)
        {
            std::ostringstream message;
            message << "Right-arm IK provider solve: provider=" << provider
                << " internal=" << arm << ',' << foreArm << ',' << hand
                << " shoulder=" << chain.start.translation.x << ','
                << chain.start.translation.y << ',' << chain.start.translation.z
                << " target=" << targetPosition.x << ',' << targetPosition.y << ','
                << targetPosition.z << " lengths=" << solved.upper_length << ','
                << solved.lower_length << " reachable=" << solved.target_reachable
                << " correction_deg="
                << QuaternionAngleDegrees(solved.start_correction) << ','
                << QuaternionAngleDegrees(solved.middle_correction) << ','
                << QuaternionAngleDegrees(distributedForeArmTwist) << ','
                << QuaternionAngleDegrees(residualHandCorrection)
                << " orientation=controller_pole_twist write=skinning_palette";
            logging::Info(message.str());
        }
        return true;
    }

    bool ApplyProbe(void* providerPointer) noexcept
    {
        if (!providerPointer)
            return false;
        const auto provider = reinterpret_cast<std::uintptr_t>(providerPointer);
        if (!provider || *reinterpret_cast<std::uintptr_t*>(provider) !=
                g_moduleBase + kPoseProviderVtableRva)
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
        if (vtable[14] != g_moduleBase + kGetLocalTransformRva ||
            vtable[17] != g_moduleBase + kSetLocalTransformRva)
            return false;
        const auto getLocal = reinterpret_cast<GetLocalTransformFn>(vtable[14]);
        const auto setLocal = reinterpret_cast<SetLocalTransformFn>(vtable[17]);
        const auto rebuildModelTransforms = reinterpret_cast<RebuildModelTransformsFn>(
            g_moduleBase + kRebuildModelTransformsRva);

        if (g_mode == ProbeMode::rightArmIk)
            return ApplyRightArmIk(provider, rebuildModelTransforms);

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
                << kSetLocalTransformRva;
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
        if (g_mode == ProbeMode::rightArmIk)
        {
            // The destination now owns the corrected matrices. Restore the provider's
            // palette immediately so another export cannot accumulate the correction.
            const auto providerAddress = reinterpret_cast<std::uintptr_t>(provider);
            *reinterpret_cast<std::uint8_t*>(providerAddress + kProviderPoseDirtyOffset) = 1;
            const auto rebuild = reinterpret_cast<RebuildModelTransformsFn>(
                g_moduleBase + kRebuildModelTransformsRva);
            rebuild(provider);
        }
        return result;
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
        if (peTimestamp != kRetailTimestamp || imageSize != kRetailImageSize)
        {
            logging::Error("Human pose probe rejected non-retail or unsupported DayZ executable");
            return false;
        }
        g_moduleBase = moduleBase;
        if (!g_moduleBase || !ValidateRetailInternals())
        {
            logging::Error("Human pose probe rejected retail internals: signature/vtable mismatch");
            return false;
        }

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
        g_ikWeight = (std::clamp)(ReadFloat(L"ik_weight", 0.75f), 0.0f, 1.0f);
        g_ikTargetOffset = {ReadFloat(L"ik_offset_x", 0.0f),
            ReadFloat(L"ik_offset_y", 0.0f), ReadFloat(L"ik_offset_z", 0.0f)};
        if (g_mode == ProbeMode::translation && g_translationX == 0.0f &&
            g_translationY == 0.0f && g_translationZ == 0.0f)
        {
            logging::Error("Human pose probe rejected a zero translation amplitude");
            return false;
        }

        void* target = reinterpret_cast<void*>(g_moduleBase + kSkinningExportRva);
        const MH_STATUS created = MH_CreateHook(target, HookedSkinningExport,
            reinterpret_cast<void**>(&g_skinningExport));
        if (created != MH_OK)
        {
            logging::Error("Human pose probe could not create the Human update hook");
            return false;
        }
        const MH_STATUS enabled = MH_EnableHook(target);
        if (enabled != MH_OK && enabled != MH_ERROR_ENABLED)
        {
            MH_RemoveHook(target);
            g_skinningExport = nullptr;
            logging::Error("Human pose probe could not enable the Human update hook");
            return false;
        }
        g_active.store(true, std::memory_order_release);
        std::ostringstream message;
        message << "Retail Human pose probe active: skinning_export=DayZ+0x" << std::hex
            << kSkinningExportRva << std::dec << " mode=" <<
                (g_mode == ProbeMode::rightArmIk ? "right_arm_ik" : "translation")
            << " bone=" << g_boneIndex
            << " translation=" << g_translationX << ',' << g_translationY << ','
            << g_translationZ << " frequency_hz=" << g_frequencyHz
            << " ik_scale=" << g_ikTargetScale << " ik_weight=" << g_ikWeight
            << " ik_offset=" << g_ikTargetOffset.x << ',' << g_ikTargetOffset.y << ','
            << g_ikTargetOffset.z;
        logging::Info(message.str());
        return true;
    }

    bool IsActive() noexcept
    {
        return g_active.load(std::memory_order_acquire);
    }
}
