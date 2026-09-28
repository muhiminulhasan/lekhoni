#pragma once

#include <windows.h>

namespace lekho::win {

extern HMODULE g_module;

} // namespace lekho::win

extern "C" ULONG STDMETHODCALLTYPE DllAddRef();
extern "C" ULONG STDMETHODCALLTYPE DllRelease();
extern "C" HRESULT STDMETHODCALLTYPE DllCanUnloadNow();
