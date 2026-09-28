#pragma once

#include <windows.h>
#include <objbase.h>

namespace lekho::win {

class ClassFactory final : public IClassFactory {
public:
    ClassFactory();
    ~ClassFactory();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
                                              void** object) override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override;

private:
    volatile LONG referenceCount_ = 1;
};

} // namespace lekho::win
