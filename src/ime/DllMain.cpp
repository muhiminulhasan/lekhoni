#include "DllMain.h"

#include "ClassFactory.h"
#include "Ids.h"

#include <atomic>
#include <new>

namespace lekho::win {

HMODULE g_module = nullptr;

namespace {
// Server lock: one count per class factory and per live COM object handed out
// by this DLL. DllCanUnloadNow must report S_FALSE while any count is
// outstanding; unloading under live sink pointers is what crashed Explorer
// (faulting module LekhoniIME.dll_unloaded) before the query was made pure.
std::atomic_long g_lockCount{0};
}

} // namespace lekho::win

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        lekho::win::g_module = module;
        DisableThreadLibraryCalls(module);
        break;
    case DLL_PROCESS_DETACH:
        lekho::win::g_module = nullptr;
        break;
    default:
        break;
    }
    return TRUE;
}

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID classId, REFIID iid,
                                                       void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualGUID(classId, lekho::win::kTextServiceClsid)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if (iid != IID_IUnknown && iid != IID_IClassFactory) {
        return E_NOINTERFACE;
    }

    auto* factory = new (std::nothrow) lekho::win::ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

extern "C" ULONG STDMETHODCALLTYPE DllAddRef() {
    return static_cast<ULONG>(lekho::win::g_lockCount.fetch_add(1, std::memory_order_relaxed) + 1);
}

extern "C" ULONG STDMETHODCALLTYPE DllRelease() {
    return static_cast<ULONG>(lekho::win::g_lockCount.fetch_sub(1, std::memory_order_acq_rel) - 1);
}

extern "C" HRESULT STDMETHODCALLTYPE DllCanUnloadNow() {
    // Pure query: COM calls this whenever the host sweeps unused libraries
    // (CoFreeUnusedLibraries), so it must never change the lock count.
    return lekho::win::g_lockCount.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}
