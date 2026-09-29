#pragma once

#include <msctf.h>

namespace lekho::win {

class TextService;

// Publishes the candidate list through TSF's UI element mechanism. Hosts that
// cannot position a foreign HWND (Chromium contenteditables, UWP, immersive
// surfaces) render the list themselves with the system candidate UI - the
// same mechanism the built-in keyboards use - by reading the data below.
class CandidateListUiElement final : public ITfCandidateListUIElementBehavior {
public:
    // The element holds a reference to the text service for its whole
    // lifetime, mirroring the Microsoft IME sample presenter.
    explicit CandidateListUiElement(TextService* owner);
    ~CandidateListUiElement();

    CandidateListUiElement(const CandidateListUiElement&) = delete;
    CandidateListUiElement& operator=(const CandidateListUiElement&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ITfUIElement
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* shown) override;

    // ITfCandidateListUIElement
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* flags) override;
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** documentManager) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* text) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* pages, UINT size, UINT* pageCount) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* pages, UINT pageCount) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override;

    // ITfCandidateListUIElementBehavior
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override;
    HRESULT STDMETHODCALLTYPE Finalize() override;
    HRESULT STDMETHODCALLTYPE Abort() override;

private:
    volatile LONG referenceCount_ = 1;
    TextService* owner_ = nullptr;
    bool shown_ = true;
};

} // namespace lekho::win
