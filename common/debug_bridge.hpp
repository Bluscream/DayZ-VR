#pragma once

namespace dayz::debug_bridge
{
    // Loads the optional debug plugin named by [debug] in dayz_openxr.ini and hands
    // it the host callback table. Safe to call repeatedly; only the first call acts.
    void Start() noexcept;
}
