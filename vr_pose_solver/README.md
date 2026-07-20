# vr_pose_solver

Small engine-agnostic C++20 pose/IK library intended for later integration into the DayZ VR `dxgi` project.

Current scope:

- vector and quaternion math;
- model-space two-bone IK corrections;
- target reach clamping without bone stretching;
- elbow pole and twist;
- configurable bend limits and soft reach;
- weighted blending from the input pose;
- end/hand orientation matching;
- twist distribution for arm roll bones;
- deterministic numerical tests and randomized stress coverage.

The library does not include DayZ, OpenXR, rendering or networking code. A future adapter must convert DayZ evaluated bone transforms to `TwoBoneChain`, call `SolveTwoBone`, convert the returned model-space delta quaternions into the verified setter space, and apply them on the game thread.

Correction order:

1. Apply `start_correction` to the upper-arm joint and its descendants.
2. Apply `middle_correction` to the forearm joint and its descendants.
3. Apply `end_correction` to the hand after inherited corrections.

`pole_position` is a point in the same model space as the chain. Bone indices, axes and matrix conventions intentionally remain outside this library.

Build and test from a Visual Studio developer environment:

```powershell
msbuild vr_pose_solver/tests/vr_pose_solver_tests.vcxproj /p:Configuration=Release /p:Platform=x64
./bin/Release/vr_pose_solver_tests.exe
```
