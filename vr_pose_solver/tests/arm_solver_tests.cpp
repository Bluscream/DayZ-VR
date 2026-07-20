#include <vr_pose_solver/arm_solver.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>

namespace
{
    using namespace vr::pose;

    int failures{};

    void Check(bool condition, std::string_view message)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }

    bool Near(float lhs, float rhs, float tolerance = 1.0e-4f)
    {
        return std::fabs(lhs - rhs) <= tolerance;
    }

    bool Near(const Vec3& lhs, const Vec3& rhs, float tolerance = 1.0e-4f)
    {
        return (lhs - rhs).Length() <= tolerance;
    }

    float QuaternionAgreement(const Quaternion& lhs, const Quaternion& rhs)
    {
        const Quaternion a = lhs.Normalized();
        const Quaternion b = rhs.Normalized();
        return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    }

    TwoBoneChain DefaultChain()
    {
        return {{{0, 0, 0}, {}}, {{1, 0, 0}, {}}, {{2, 0, 0}, {}}};
    }

    void CheckLengths(const TwoBoneResult& result, float tolerance = 1.0e-4f,
        const Vec3& start = {})
    {
        Check(Near((result.solved_middle_position - start).Length(), result.upper_length, tolerance),
            "upper bone length must be preserved");
        Check(Near((result.solved_end_position - result.solved_middle_position).Length(),
            result.lower_length, tolerance), "lower bone length must be preserved");
    }

    void TestReachableTarget()
    {
        const TwoBoneResult result = SolveTwoBone(DefaultChain(),
            {{1.2f, 1.0f, 0.0f}, {}, {0, 0, 1}});
        Check(result.Succeeded(), "reachable solve succeeds");
        Check(result.target_reachable, "reachable target is reported reachable");
        Check(Near(result.solved_end_position, {1.2f, 1.0f, 0.0f}),
            "reachable end reaches target");
        CheckLengths(result);
    }

    void TestQuaternionEdgeCases()
    {
        const Quaternion opposite = Quaternion::FromTo({1, 0, 0}, {-1, 0, 0});
        Check(Near(opposite.Rotate({1, 0, 0}), {-1, 0, 0}),
            "from-to handles antiparallel vectors");

        TwoBoneTarget invalid{{1, 0, 0}, {}, {0, 1, 0}};
        invalid.position.x = std::numeric_limits<float>::quiet_NaN();
        Check(SolveTwoBone(DefaultChain(), invalid).status == SolveStatus::invalid_input,
            "non-finite input is rejected");
    }

    void TestPoleSide()
    {
        const auto positive = SolveTwoBone(DefaultChain(), {{1.0f, 0.0f, 0.0f}, {}, {0, 1, 0}});
        const auto negative = SolveTwoBone(DefaultChain(), {{1.0f, 0.0f, 0.0f}, {}, {0, -1, 0}});
        Check(positive.solved_middle_position.y > 0.0f, "positive pole bends positive");
        Check(negative.solved_middle_position.y < 0.0f, "negative pole bends negative");
    }

    void TestFarAndNearTargets()
    {
        const auto far = SolveTwoBone(DefaultChain(), {{10, 0, 0}, {}, {0, 1, 0}});
        Check(far.Succeeded() && !far.target_reachable, "far target is clamped");
        Check(far.solved_end_position.x < 2.0001f, "far target does not stretch bones");
        CheckLengths(far);

        TwoBoneChain unequal{{{0, 0, 0}, {}}, {{2, 0, 0}, {}}, {{2.5f, 0, 0}, {}}};
        const auto near = SolveTwoBone(unequal, {{0.1f, 0, 0}, {}, {0, 1, 0}});
        Check(near.Succeeded() && !near.target_reachable, "inside unreachable radius is clamped");
        Check((near.solved_end_position - unequal.start.translation).Length() >= 1.499f,
            "unequal chain respects inner reach");
        CheckLengths(near);
    }

    void TestWeightAndOrientation()
    {
        TwoBoneTarget zero{{1, 1, 0}, Quaternion::FromAxisAngle({0, 0, 1}, 1.0f), {0, 0, 1}};
        zero.weight = 0.0f;
        const auto unchanged = SolveTwoBone(DefaultChain(), zero);
        Check(Near(unchanged.solved_middle_position, {1, 0, 0}), "zero weight preserves middle");
        Check(Near(unchanged.solved_end_position, {2, 0, 0}), "zero weight preserves end");
        Check(QuaternionAgreement(unchanged.end_correction, Quaternion::Identity()) > 0.99999f,
            "zero weight preserves hand orientation");

        TwoBoneTarget full = zero;
        full.weight = 1.0f;
        const auto solved = SolveTwoBone(DefaultChain(), full);
        const Quaternion inherited = (solved.middle_correction * solved.start_correction).Normalized();
        const Quaternion finalHand = (solved.end_correction * inherited).Normalized();
        Check(QuaternionAgreement(finalHand, full.orientation) > 0.9999f,
            "full weight matches requested hand orientation");
    }

    void TestTwistAndLimits()
    {
        TwoBoneTarget untwisted{{1, 0, 0}, {}, {0, 1, 0}};
        TwoBoneTarget twisted = untwisted;
        twisted.twist_radians = kPi;
        const auto a = SolveTwoBone(DefaultChain(), untwisted);
        const auto b = SolveTwoBone(DefaultChain(), twisted);
        Check(a.solved_middle_position.y > 0.0f && b.solved_middle_position.y < 0.0f,
            "pi twist rotates the bend plane");

        TwoBoneConstraints limited;
        limited.minimum_bend_radians = 0.5f;
        const auto constrained = SolveTwoBone(DefaultChain(), {{2, 0, 0}, {}, {0, 1, 0}}, limited);
        Check(constrained.bend_radians >= 0.4999f, "minimum bend is enforced");
        Check(constrained.effective_distance < 2.0f, "minimum bend reduces extension");

        const TwistDistribution split = DistributeTwist({1, 0, 0}, 1.0f, 0.25f);
        const Vec3 reference{0, 1, 0};
        const Vec3 combined = (split.distal * split.proximal).Rotate(reference);
        const Vec3 expected = Quaternion::FromAxisAngle({1, 0, 0}, 1.0f).Rotate(reference);
        Check(Near(combined, expected), "roll-bone twist split preserves total rotation");
    }

    void TestSoftReachAndDegenerateInput()
    {
        TwoBoneConstraints soft;
        soft.soften_start_ratio = 0.8f;
        const auto softened = SolveTwoBone(DefaultChain(), {{2, 0, 0}, {}, {0, 1, 0}}, soft);
        Check(softened.effective_distance < 1.99f, "soft reach falls behind full extension");

        TwoBoneChain degenerate = DefaultChain();
        degenerate.middle.translation = degenerate.start.translation;
        const auto bad = SolveTwoBone(degenerate, {{1, 0, 0}, {}, {0, 1, 0}});
        Check(bad.status == SolveStatus::degenerate_chain, "zero-length bone is rejected");
    }

    void TestRotationAndTranslationInvariance()
    {
        const TwoBoneChain base = DefaultChain();
        const TwoBoneTarget target{{1.0f, 0.8f, 0.2f}, {}, {0, 0, 1}};
        const auto baseResult = SolveTwoBone(base, target);

        const Quaternion rotation = Quaternion::FromAxisAngle({0.2f, 1.0f, -0.1f}, 1.1f);
        const Vec3 translation{4, -2, 7};
        const auto map = [&](const Vec3& value) { return translation + rotation.Rotate(value); };
        TwoBoneChain mapped = base;
        mapped.start.translation = map(base.start.translation);
        mapped.middle.translation = map(base.middle.translation);
        mapped.end.translation = map(base.end.translation);
        TwoBoneTarget mappedTarget = target;
        mappedTarget.position = map(target.position);
        mappedTarget.pole_position = map(target.pole_position);
        const auto mappedResult = SolveTwoBone(mapped, mappedTarget);
        Check(Near(mappedResult.solved_middle_position, map(baseResult.solved_middle_position), 3.0e-4f),
            "solver is rotation/translation invariant at middle");
        Check(Near(mappedResult.solved_end_position, map(baseResult.solved_end_position), 3.0e-4f),
            "solver is rotation/translation invariant at end");
    }

    void TestRandomStress()
    {
        std::mt19937 random(0xD4A2u);
        std::uniform_real_distribution<float> value(-4.0f, 4.0f);
        for (int iteration = 0; iteration < 20000; ++iteration)
        {
            const float upper = 0.1f + std::fabs(value(random));
            const float lower = 0.1f + std::fabs(value(random));
            const Quaternion rotation = Quaternion::FromAxisAngle(
                NormalizeOr({value(random), value(random), value(random)}, {0, 1, 0}), value(random));
            const Vec3 translation{value(random), value(random), value(random)};
            const auto map = [&](const Vec3& point) { return translation + rotation.Rotate(point); };
            TwoBoneChain chain{{map({0, 0, 0}), rotation},
                {map({upper, 0, 0}), rotation}, {map({upper + lower, 0, 0}), rotation}};
            TwoBoneTarget target{map({value(random), value(random), value(random)}), rotation,
                map({value(random), value(random), value(random)})};
            target.twist_radians = value(random);
            const auto result = SolveTwoBone(chain, target);
            Check(result.Succeeded(), "random solve succeeds");
            Check(result.solved_middle_position.IsFinite() && result.solved_end_position.IsFinite() &&
                result.start_correction.IsFinite() && result.middle_correction.IsFinite(),
                "random solve remains finite");
            CheckLengths(result, 1.0e-3f, chain.start.translation);
            if (failures > 20)
                return;
        }
    }
}

int main()
{
    TestReachableTarget();
    TestQuaternionEdgeCases();
    TestPoleSide();
    TestFarAndNearTargets();
    TestWeightAndOrientation();
    TestTwistAndLimits();
    TestSoftReachAndDegenerateInput();
    TestRotationAndTranslationInvariance();
    TestRandomStress();

    if (failures != 0)
    {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "vr_pose_solver: all tests passed (including 20000 randomized solves)\n";
    return 0;
}
