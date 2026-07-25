# DayZ VR server launcher

`dayz_server_launcher.exe` is a process wrapper for a private DayZ server. It forwards ordinary server arguments, applies two in-memory compatibility patches to `DayZServer_x64.exe` before its first instruction runs, waits for the child process, and returns its exit code.

By default the launcher first looks beside itself and then in the current working directory. This permits either copying it beside the DayZ executables or running the build output from the DayZ installation directory:

```powershell
.\dayz_server_launcher.exe -config=serverDZ.cfg -port=2302 -profiles=profiles
```

Diagnostic server mode:

```powershell
.\dayz_server_launcher.exe --launcher-diag -config=serverDZ.cfg -port=2302
```

The diagnostic mode starts sibling `DayZDiag_x64.exe` and adds `-server` unless it is already present. An explicit target can be selected with:

```powershell
.\dayz_server_launcher.exe --launcher-target "G:\DayZ\DayZServer_x64.exe" -- -config=serverDZ.cfg
```

Use `--launcher-dry-run` to verify quoting and the resolved command without starting a process. `--` stops launcher-option parsing and forwards all remaining arguments.

The compatibility patches are applied only when the selected executable is named `DayZServer_x64.exe`. `DayZDiag_x64.exe` is launched unchanged. If either byte signature is missing or ambiguous, the suspended server process is terminated instead of being started partially patched. The launcher does not modify the executable on disk or inject a native module.
