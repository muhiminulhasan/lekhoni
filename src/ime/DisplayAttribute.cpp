#include "DisplayAttribute.h"

#include "DllMain.h"
#include "Ids.h"

#include <algorithm>
#include <new>

#include <oleauto.h>

namespace lekho::win {
namespace {

// Enumerates the single attribute published by DisplayAttributeProvider. A
// provider owns exactly one attribute, so the enumerator only tracks whether
// that attribute has been handed out yet.
class DisplayAttributeEnumerator final : public IEnumTfDisplayAttributeInfo {
public:
    explicit DisplayAttributeEnumerator(ITfDisplayAttributeInfo* info) : info_(info) {
        if (info_ != nullptr) {
            info_->AddRef();
        }
    }

    ~DisplayAttributeEnumerator() {
        if (info_ != nullptr) {
            info_->Release();
        }
    }

    DisplayAttributeEnumerator(const DisplayAttributeEnumerator&) = delete;
    DisplayAttributeEnumerator& operator=(const DisplayAttributeEnumerator&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IEnumTfDisplayAttributeInfo)) {
            *object = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&referenceCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>((std::max)(count, 0L));
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** enumerator) override {
        if (enumerator == nullptr) {
            return E_POINTER;
        }
        *enumerator = nullptr;
        if (info_ == nullptr) {
            return E_FAIL;
        }
        auto* clone = new (std::nothrow) DisplayAttributeEnumerator(info_);
        if (clone == nullptr) {
            return E_OUTOFMEMORY;
        }
        clone->position_ = position_;
        *enumerator = clone;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo** info,
                                   ULONG* fetched) override {
        if (info == nullptr || fetched == nullptr) {
            return E_POINTER;
        }
        *fetched = 0;
        if (count == 0) {
            return S_OK;
        }
        if (info_ == nullptr || position_ != 0) {
            return S_FALSE;
        }
        info[0] = info_;
        info[0]->AddRef();
        position_ = 1;
        *fetched = 1;
        return count == 1 ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        position_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        if (count != 0) {
            // Only one attribute exists, so any skip exhausts the enumerator.
            position_ = 1;
            return S_FALSE;
        }
        return S_OK;
    }

private:
    volatile LONG referenceCount_ = 1;
    ITfDisplayAttributeInfo* info_ = nullptr;
    ULONG position_ = 0;
};

} // namespace

TF_DISPLAYATTRIBUTE compositionDisplayAttribute() noexcept {
    TF_DISPLAYATTRIBUTE attribute{};
    // TF_CT_NONE leaves the application text and background colors in place.
    attribute.crText.type = TF_CT_NONE;
    attribute.crBk.type = TF_CT_NONE;
    attribute.lsStyle = kCompositionLineStyle;
    attribute.fBoldLine = kCompositionBoldLine ? TRUE : FALSE;
    attribute.crLine.type = TF_CT_COLORREF;
    attribute.crLine.cr = kCompositionUnderlineColor;
    attribute.bAttr = TF_ATTR_OTHER;
    return attribute;
}

DisplayAttributeProvider::DisplayAttributeProvider() {
    DllAddRef();
}

DisplayAttributeProvider::~DisplayAttributeProvider() {
    DllRelease();
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::QueryInterface(REFIID iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (iid == __uuidof(IUnknown)) {
        *object = static_cast<ITfDisplayAttributeProvider*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfDisplayAttributeProvider)) {
        *object = static_cast<ITfDisplayAttributeProvider*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfDisplayAttributeInfo)) {
        *object = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DisplayAttributeProvider::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

ULONG STDMETHODCALLTYPE DisplayAttributeProvider::Release() {
    const LONG count = InterlockedDecrement(&referenceCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>((std::max)(count, 0L));
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::EnumDisplayAttributeInfo(
    IEnumTfDisplayAttributeInfo** enumerator) {
    if (enumerator == nullptr) {
        return E_POINTER;
    }
    *enumerator = nullptr;
    auto* instance = new (std::nothrow) DisplayAttributeEnumerator(
        static_cast<ITfDisplayAttributeInfo*>(this));
    if (instance == nullptr) {
        return E_OUTOFMEMORY;
    }
    *enumerator = instance;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::GetDisplayAttributeInfo(
    REFGUID guid, ITfDisplayAttributeInfo** info) {
    if (info == nullptr) {
        return E_POINTER;
    }
    *info = nullptr;
    if (!IsEqualGUID(guid, kCompositionDisplayAttributeGuid)) {
        return E_INVALIDARG;
    }
    *info = static_cast<ITfDisplayAttributeInfo*>(this);
    AddRef();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::GetGUID(GUID* guid) {
    if (guid == nullptr) {
        return E_POINTER;
    }
    *guid = kCompositionDisplayAttributeGuid;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::GetDescription(BSTR* description) {
    if (description == nullptr) {
        return E_POINTER;
    }
    *description = SysAllocString(kCompositionAttributeDescription);
    return *description != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::GetAttributeInfo(
    TF_DISPLAYATTRIBUTE* attribute) {
    if (attribute == nullptr) {
        return E_POINTER;
    }
    *attribute = compositionDisplayAttribute();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::SetAttributeInfo(
    const TF_DISPLAYATTRIBUTE* attribute) {
    // The composition style is fixed, so a client cannot change it.
    (void)attribute;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DisplayAttributeProvider::Reset() {
    // Nothing is mutable, so the object is always in its default state.
    return S_OK;
}

} // namespace lekho::win
