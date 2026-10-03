#pragma once
#include <Unknwn.h>
#include <dxgi.h>
namespace hooks { void AttachToFactory(IUnknown* factory) noexcept; void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept; }
