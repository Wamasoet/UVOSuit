#ifdef _WIN32

#include "layer_config.hpp"
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE /*hinstDLL*/, DWORD fdwReason, LPVOID /*lpReserved*/)
{
	if  (fdwReason == DLL_PROCESS_ATTACH)
		OutputDebugStringA("OpenXR API Layer " XR_THISLAYER_NAME " dynamically loaded library initialized.\n");

	return TRUE;
}

#endif