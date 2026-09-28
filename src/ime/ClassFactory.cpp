#include "ClassFactory.h"

#include "Ids.h"
#include "DllMain.h"
#include "DisplayAttribute.h"
#include "TextService.h"

#include <algorithm>
#include <new>

namespace lekho::win {

ClassFactory::ClassFactory() {
    DllAddRef();
}

ClassFactory::~ClassFactory() {
    DllRelease();
}

HRESULT STDMETHODCALLTYPE ClassFactory::QueryInterface(REFIID iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ClassFactory::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

ULONG STDMETHODCALLTYPE ClassFactory::Release() {
    const LONG count = InterlockedDecrement(&referenceCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>((std::max)(count, 0L));
}

HRESULT STDMETHODCALLTYPE ClassFactory::CreateInstance(IUnknown* outer, REFIID iid,
                                                      void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }
    if (iid == IID_NULL) {
        return E_NOINTERFACE;
    }

    // The TSF manager activates the registered display attribute provider with
    // the same class identifier as the text service.
    if (iid == __uuidof(ITfDisplayAttributeProvider) ||
        iid == __uuidof(ITfDisplayAttributeInfo)) {
        auto* provider = new (std::nothrow) DisplayAttributeProvider();
        if (provider == nullptr) {
            return E_OUTOFMEMORY;
        }
        const HRESULT result = provider->QueryInterface(iid, object);
        provider->Release();
        return result;
    }

    auto* service = new (std::nothrow) TextService();
    if (service == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT result = service->QueryInterface(iid, object);
    service->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE ClassFactory::LockServer(BOOL lock) {
    if (lock) {
        DllAddRef();
    } else {
        DllRelease();
    }
    return S_OK;
}

} // namespace lekho::win
