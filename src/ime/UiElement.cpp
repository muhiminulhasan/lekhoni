#include "UiElement.h"

#include "Ids.h"
#include "TextService.h"

#include <oleauto.h>

namespace lekho::win {

CandidateListUiElement::CandidateListUiElement(TextService* owner) : owner_(owner) {
    if (owner_ != nullptr) {
        owner_->AddRef();
    }
}

CandidateListUiElement::~CandidateListUiElement() {
    if (owner_ != nullptr) {
        owner_->Release();
    }
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::QueryInterface(REFIID iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;

    if (iid == __uuidof(IUnknown)) {
        *object = static_cast<ITfCandidateListUIElementBehavior*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfUIElement)) {
        *object = static_cast<ITfUIElement*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfCandidateListUIElement)) {
        *object = static_cast<ITfCandidateListUIElement*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfCandidateListUIElementBehavior)) {
        *object = static_cast<ITfCandidateListUIElementBehavior*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CandidateListUiElement::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

ULONG STDMETHODCALLTYPE CandidateListUiElement::Release() {
    const LONG count = InterlockedDecrement(&referenceCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count > 0 ? count : 0);
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetDescription(BSTR* description) {
    if (description == nullptr) {
        return E_POINTER;
    }
    *description = SysAllocString(L"Lekhoni Candidate List");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetGUID(GUID* guid) {
    if (guid == nullptr) {
        return E_POINTER;
    }
    *guid = kCandidateListUiElementGuid;
    return S_OK;
}

// The system candidate UI calls this when it shows or hides the list, for
// example when focus moves away; the state is only tracked, the system owns
// the rendering.
HRESULT STDMETHODCALLTYPE CandidateListUiElement::Show(BOOL show) {
    shown_ = show != FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::IsShown(BOOL* shown) {
    if (shown == nullptr) {
        return E_POINTER;
    }
    *shown = shown_ ? TRUE : FALSE;
    return S_OK;
}

// The system re-reads every property after UpdateUIElement, so reporting the
// full set is always correct.
HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetUpdatedFlags(DWORD* flags) {
    if (flags == nullptr) {
        return E_POINTER;
    }
    *flags = TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING |
             TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetDocumentMgr(ITfDocumentMgr** documentManager) {
    if (documentManager == nullptr) {
        return E_POINTER;
    }
    *documentManager = nullptr;
    if (owner_ == nullptr) {
        return E_UNEXPECTED;
    }
    ITfContext* context = owner_->compositionContext();
    if (context == nullptr) {
        return S_OK;
    }
    return context->GetDocumentMgr(documentManager);
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetCount(UINT* count) {
    if (count == nullptr) {
        return E_POINTER;
    }
    *count = static_cast<UINT>(owner_ != nullptr ? owner_->candidateWindow_.count() : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetSelection(UINT* index) {
    if (index == nullptr) {
        return E_POINTER;
    }
    *index = static_cast<UINT>(owner_ != nullptr ? owner_->candidateWindow_.selectedIndex() : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetString(UINT index, BSTR* text) {
    if (text == nullptr) {
        return E_POINTER;
    }
    *text = nullptr;
    if (owner_ == nullptr || index >= owner_->candidateWindow_.count()) {
        return E_INVALIDARG;
    }
    *text = SysAllocString(owner_->candidateWindow_.at(index).c_str());
    return *text != nullptr ? S_OK : E_OUTOFMEMORY;
}

// A single page holding the whole list; the system scrolls or paginates only
// with more entries than Lekhoni ever offers.
HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetPageIndex(UINT* pages, UINT size,
                                                              UINT* pageCount) {
    if (pageCount == nullptr) {
        return E_POINTER;
    }
    if (pages != nullptr && size >= 1) {
        pages[0] = 0;
    }
    *pageCount = 1;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::SetPageIndex(UINT* pages, UINT pageCount) {
    (void)pages;
    (void)pageCount;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::GetCurrentPage(UINT* page) {
    if (page == nullptr) {
        return E_POINTER;
    }
    *page = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::SetSelection(UINT index) {
    if (owner_ == nullptr) {
        return E_UNEXPECTED;
    }
    owner_->setCandidateSelection(index);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::Finalize() {
    if (owner_ == nullptr) {
        return E_UNEXPECTED;
    }
    owner_->onCandidateClicked(owner_->candidateWindow_.selectedIndex());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CandidateListUiElement::Abort() {
    if (owner_ == nullptr) {
        return E_UNEXPECTED;
    }
    return owner_->cancelComposition();
}

} // namespace lekho::win
