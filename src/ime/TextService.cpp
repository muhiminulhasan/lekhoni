#include "TextService.h"

#include "DllMain.h"
#include "Ids.h"
#include "UiElement.h"
#include "WinText.h"

#include "lekhoni/AvroLayoutEngine.h"
#include "lekhoni/WordPrefix.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lekho::win {
namespace {

constexpr std::size_t kMaximumDictionaryBytes = 128U * 1024U * 1024U;
// The phrase layer is a small curated table, so it gets a much tighter bound
// than the core dictionary.
constexpr std::size_t kMaximumPhraseBytes = 4U * 1024U * 1024U;
constexpr LONG kMaximumEditCharacters = 1'000'000;

bool isDown(int virtualKey) noexcept {
    return (GetKeyState(virtualKey) & 0x8000) != 0;
}

bool hasCommandModifier() noexcept {
    return isDown(VK_CONTROL) || isDown(VK_LCONTROL) || isDown(VK_RCONTROL) ||
           isDown(VK_MENU) || isDown(VK_LMENU) || isDown(VK_RMENU) ||
           isDown(VK_LWIN) || isDown(VK_RWIN);
}

HRESULT setSelectionAtEnd(ITfContext* context, TfEditCookie cookie, ITfRange* range) {
    if (context == nullptr || range == nullptr) {
        return E_POINTER;
    }

    // GetRange() may return the composition's live range. Collapse a clone for
    // the caret instead of collapsing that range itself; otherwise a shorter
    // recomposition can leave deleted characters in the document.
    ITfRange* selectionRange = nullptr;
    HRESULT result = range->Clone(&selectionRange);
    if (FAILED(result) || selectionRange == nullptr) {
        if (selectionRange != nullptr) {
            selectionRange->Release();
        }
        return FAILED(result) ? result : E_FAIL;
    }
    result = selectionRange->Collapse(cookie, TF_ANCHOR_END);
    if (SUCCEEDED(result)) {
        TF_SELECTION selection{};
        selection.range = selectionRange;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        result = context->SetSelection(cookie, 1, &selection);
    }
    selectionRange->Release();
    return result;
}

// The classic caret of this thread's focused window, in screen coordinates.
// CUAS and other emulated hosts create a real caret even when their views
// cannot answer GetTextExt, which makes it a usable anchor for the candidate
// window there.
bool caretAnchor(int* x, int* y) noexcept {
    if (x == nullptr || y == nullptr) {
        return false;
    }
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetCurrentThreadId(), &info) || info.hwndCaret == nullptr) {
        return false;
    }
    POINT baseline{info.rcCaret.left, info.rcCaret.bottom};
    if (!ClientToScreen(info.hwndCaret, &baseline)) {
        return false;
    }
    *x = baseline.x;
    *y = baseline.y;
    return true;
}

} // namespace

class EditSession final : public ITfEditSession {
public:
    EditSession(TextService* owner, ITfContext* context, TextService::EditMode mode,
                 std::wstring text, std::wstring trailing, std::size_t prefixLength)
        : owner_(owner), context_(context), mode_(mode), text_(std::move(text)),
          trailing_(std::move(trailing)), prefixLength_(prefixLength) {
        if (context_ != nullptr) {
            context_->AddRef();
        }
    }

    ~EditSession() {
        if (context_ != nullptr) {
            context_->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ITfEditSession)) {
            *object = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
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

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        if (owner_ == nullptr || context_ == nullptr ||
            text_.size() > static_cast<std::size_t>(kMaximumEditCharacters)) {
            return E_INVALIDARG;
        }

        try {
            switch (mode_) {
            case TextService::EditMode::Start:
                return startComposition(cookie);
            case TextService::EditMode::Update:
                return updateComposition(cookie);
            case TextService::EditMode::Finish:
                return finishComposition(cookie);
            case TextService::EditMode::Cancel:
                return cancelComposition(cookie);
            case TextService::EditMode::Insert:
                return insertText(cookie, text_ + trailing_);
            case TextService::EditMode::Position:
                return positionForCandidates(cookie);
            case TextService::EditMode::ReadBanglaPrefix:
                return readBanglaPrefix(cookie);
            case TextService::EditMode::ReplaceBanglaPrefix:
                return replaceBanglaPrefix(cookie);
            case TextService::EditMode::ClearCompositionAttribute:
                return clearOwnedCompositionAttribute(cookie);
            default:
                return E_INVALIDARG;
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

private:
    HRESULT startComposition(TfEditCookie cookie) {
        if (owner_->composition_ != nullptr || owner_->context_ != nullptr) {
            return TF_E_ALREADY_EXISTS;
        }

        TF_SELECTION selection{};
        ULONG fetched = 0;
        HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                                 &fetched);
        if (FAILED(result) || fetched != 1 || selection.range == nullptr) {
            return TF_E_NOSELECTION;
        }

        ITfContextComposition* contextComposition = nullptr;
        result = context_->QueryInterface(IID_ITfContextComposition,
                                          reinterpret_cast<void**>(&contextComposition));
        if (FAILED(result) || contextComposition == nullptr) {
            selection.range->Release();
            return result == S_OK ? E_NOINTERFACE : result;
        }

        ITfComposition* composition = nullptr;
        result = contextComposition->StartComposition(
            cookie, selection.range, static_cast<ITfCompositionSink*>(owner_), &composition);
        contextComposition->Release();
        selection.range->Release();
        if (FAILED(result) || composition == nullptr) {
            return FAILED(result) ? result : TF_E_COMPOSITION_REJECTED;
        }

        ITfRange* range = nullptr;
        result = composition->GetRange(&range);
        if (FAILED(result) || range == nullptr) {
            composition->EndComposition(cookie);
            composition->Release();
            return FAILED(result) ? result : E_FAIL;
        }

        result = range->SetText(cookie, 0, text_.c_str(), static_cast<LONG>(text_.size()));
        if (SUCCEEDED(result)) {
            applyCompositionAttribute(cookie, range);
            result = setSelectionAtEnd(context_, cookie, range);
        }
        range->Release();
        if (FAILED(result)) {
            composition->EndComposition(cookie);
            composition->Release();
            return result;
        }

        owner_->composition_ = composition;
        owner_->context_ = context_;
        context_->AddRef();
        owner_->composing_ = true;
        owner_->updateCandidates();
        owner_->positionCandidates(cookie, context_);
        owner_->adviseLayoutSink();
        return S_OK;
    }

    HRESULT updateComposition(TfEditCookie cookie) {
        if (owner_->composition_ == nullptr || owner_->context_ != context_) {
            return TF_E_NOTOWNEDRANGE;
        }
        ITfRange* range = nullptr;
        HRESULT result = owner_->composition_->GetRange(&range);
        if (FAILED(result) || range == nullptr) {
            return FAILED(result) ? result : E_FAIL;
        }

        result = range->SetText(cookie, 0, text_.c_str(), static_cast<LONG>(text_.size()));
        if (SUCCEEDED(result)) {
            // The range changed length, so the attribute is applied again.
            applyCompositionAttribute(cookie, range);
            result = setSelectionAtEnd(context_, cookie, range);
        }
        range->Release();
        if (SUCCEEDED(result)) {
            owner_->updateCandidates();
            owner_->positionCandidates(cookie, context_);
        }
        return result;
    }

    HRESULT finishComposition(TfEditCookie cookie) {
        std::wstring combined = text_ + trailing_;
        if (owner_->composition_ == nullptr) {
            return insertText(cookie, combined);
        }
        if (owner_->context_ != context_) {
            return TF_E_NOTOWNEDRANGE;
        }

        if (combined.size() > static_cast<std::size_t>(kMaximumEditCharacters)) {
            return E_INVALIDARG;
        }

        ITfRange* range = nullptr;
        HRESULT result = owner_->composition_->GetRange(&range);
        if (FAILED(result) || range == nullptr) {
            return FAILED(result) ? result : E_FAIL;
        }

        result = range->SetText(cookie, 0, combined.c_str(), static_cast<LONG>(combined.size()));
        if (SUCCEEDED(result)) {
            // The text is committed, so the composing underline goes away.
            clearCompositionAttribute(cookie, range);
            result = setSelectionAtEnd(context_, cookie, range);
        }
        range->Release();
        if (FAILED(result)) {
            return result;
        }

        result = owner_->composition_->EndComposition(cookie);
        if (SUCCEEDED(result)) {
            owner_->clearCompositionState();
        }
        return result;
    }

    HRESULT cancelComposition(TfEditCookie cookie) {
        if (owner_->composition_ == nullptr) {
            return S_OK;
        }
        if (owner_->context_ != context_) {
            return TF_E_NOTOWNEDRANGE;
        }

        ITfRange* range = nullptr;
        HRESULT result = owner_->composition_->GetRange(&range);
        if (FAILED(result) || range == nullptr) {
            return FAILED(result) ? result : E_FAIL;
        }

        result = range->SetText(cookie, 0, L"", 0);
        if (SUCCEEDED(result)) {
            clearCompositionAttribute(cookie, range);
            result = setSelectionAtEnd(context_, cookie, range);
        }
        range->Release();

        const HRESULT endResult = owner_->composition_->EndComposition(cookie);
        if (FAILED(result)) {
            return result;
        }
        if (SUCCEEDED(endResult)) {
            owner_->clearCompositionState();
        }
        return endResult;
    }

    HRESULT readBanglaPrefix(TfEditCookie cookie) {
        TF_SELECTION selection{};
        ULONG fetched = 0;
        HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                                 &fetched);
        if (FAILED(result) || fetched != 1 || selection.range == nullptr) {
            return TF_E_NOSELECTION;
        }

        ITfRange* range = nullptr;
        result = selection.range->Clone(&range);
        if (FAILED(result) || range == nullptr) {
            selection.range->Release();
            return FAILED(result) ? result : E_FAIL;
        }

        result = range->Collapse(cookie, TF_ANCHOR_START);
        LONG shifted = 0;
        if (SUCCEEDED(result)) {
            result = range->ShiftStart(cookie, -128, &shifted, nullptr);
        }
        if (SUCCEEDED(result) && shifted == 0) {
            result = TF_E_NOTOWNEDRANGE;
        }

        ULONG required = 0;
        if (SUCCEEDED(result)) {
            result = range->GetText(cookie, 0, nullptr, 0, &required);
        }
        if (FAILED(result) || required == 0 || required > 128) {
            range->Release();
            selection.range->Release();
            owner_->clearBanglaSuggestions();
            return SUCCEEDED(result) ? S_OK : result;
        }

        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
        ULONG read = 0;
        result = range->GetText(cookie, 0, buffer.data(), required, &read);
        range->Release();
        selection.range->Release();
        if (FAILED(result) || read == 0) {
            owner_->clearBanglaSuggestions();
            return SUCCEEDED(result) ? S_OK : result;
        }

        buffer.resize(static_cast<std::size_t>(read));
        const std::string utf8 = toUtf8(std::wstring_view(buffer.data(), buffer.size()));
        const bn::WordPrefix prefix = bn::findWordPrefix(utf8, 128U * 4U);
        if (prefix.kind == bn::WordKind::Bangla) {
            owner_->setBanglaSuggestions(prefix.utf8, context_);
            owner_->positionCandidates(cookie, context_);
        } else {
            owner_->clearBanglaSuggestions();
        }
        return S_OK;
    }

    HRESULT replaceBanglaPrefix(TfEditCookie cookie) {
        if (prefixLength_ == 0 || prefixLength_ > 128) {
            return E_INVALIDARG;
        }
        std::wstring combined = text_ + trailing_;
        if (combined.size() > static_cast<std::size_t>(kMaximumEditCharacters)) {
            return E_INVALIDARG;
        }

        TF_SELECTION selection{};
        ULONG fetched = 0;
        HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                                 &fetched);
        if (FAILED(result) || fetched != 1 || selection.range == nullptr) {
            return TF_E_NOSELECTION;
        }

        ITfRange* range = nullptr;
        result = selection.range->Clone(&range);
        if (SUCCEEDED(result) && range != nullptr) {
            result = range->Collapse(cookie, TF_ANCHOR_START);
            LONG shifted = 0;
            if (SUCCEEDED(result)) {
                result = range->ShiftStart(cookie, -static_cast<LONG>(prefixLength_), &shifted,
                                           nullptr);
            }
            const LONG shiftedMagnitude = shifted < 0 ? -shifted : shifted;
            if (SUCCEEDED(result) &&
                shiftedMagnitude == static_cast<LONG>(prefixLength_)) {
                result = range->SetText(cookie, 0, combined.c_str(),
                                        static_cast<LONG>(combined.size()));
            } else if (SUCCEEDED(result)) {
                result = TF_E_NOTOWNEDRANGE;
            }
            if (SUCCEEDED(result)) {
                result = setSelectionAtEnd(context_, cookie, range);
            }
            if (range != nullptr) {
                range->Release();
            }
        }
        selection.range->Release();
        return result;
    }

    // Re-positions the candidate window from a later read-only session. Hosts
    // that lay text out asynchronously reject GetTextExt while the composing
    // edit session is still open; this is the retry after their layout pass.
    HRESULT positionForCandidates(TfEditCookie cookie) {
        if (owner_->composition_ == nullptr && !owner_->banglaSuggestionActive_) {
            return S_OK;
        }
        ITfContext* context = owner_->compositionContext();
        if (context == nullptr) {
            return S_OK;
        }
        owner_->positionCandidates(cookie, context);
        return S_OK;
    }

    HRESULT insertText(TfEditCookie cookie, const std::wstring& combined) {
        if (combined.size() > static_cast<std::size_t>(kMaximumEditCharacters)) {
            return E_INVALIDARG;
        }

        TF_SELECTION selection{};
        ULONG fetched = 0;
        HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                                 &fetched);
        if (FAILED(result) || fetched != 1 || selection.range == nullptr) {
            return TF_E_NOSELECTION;
        }
        result = selection.range->SetText(cookie, 0, combined.c_str(),
                                          static_cast<LONG>(combined.size()));
        if (SUCCEEDED(result)) {
            result = setSelectionAtEnd(context_, cookie, selection.range);
        }
        selection.range->Release();
        return result;
    }

    // The composition range carries the published display attribute so the
    // host draws the subtle underline. Formatting is decoration only, so every
    // failure is ignored and the composing text is never rejected because of it.
    void applyCompositionAttribute(TfEditCookie cookie, ITfRange* range) const noexcept {
        if (range == nullptr || context_ == nullptr ||
            owner_->compositionAttributeAtom_ == TF_INVALID_GUIDATOM) {
            return;
        }
        ITfProperty* property = nullptr;
        if (FAILED(context_->GetProperty(GUID_PROP_ATTRIBUTE, &property)) ||
            property == nullptr) {
            return;
        }
        // Display attributes are published as a registered TfGuidAtom.
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = static_cast<LONG>(owner_->compositionAttributeAtom_);
        (void)property->SetValue(cookie, range, &value);
        property->Release();
    }

    // Removes only the display attribute. GUID_PROP_COMPOSING is owned by TSF
    // and is never touched here.
    void clearCompositionAttribute(TfEditCookie cookie, ITfRange* range) const noexcept {
        if (range == nullptr || context_ == nullptr) {
            return;
        }
        ITfProperty* property = nullptr;
        if (FAILED(context_->GetProperty(GUID_PROP_ATTRIBUTE, &property)) ||
            property == nullptr) {
            return;
        }
        (void)property->Clear(cookie, range);
        property->Release();
    }

    HRESULT clearOwnedCompositionAttribute(TfEditCookie cookie) {
        if (owner_->composition_ == nullptr || owner_->context_ != context_) {
            return S_OK;
        }
        ITfRange* range = nullptr;
        const HRESULT result = owner_->composition_->GetRange(&range);
        if (FAILED(result) || range == nullptr) {
            return FAILED(result) ? result : E_FAIL;
        }
        clearCompositionAttribute(cookie, range);
        range->Release();
        return S_OK;
    }

    volatile LONG referenceCount_ = 1;
    TextService* owner_ = nullptr;
    ITfContext* context_ = nullptr;
    TextService::EditMode mode_ = TextService::EditMode::Insert;
    std::wstring text_;
    std::wstring trailing_;
    std::size_t prefixLength_ = 0;
};

TextService::TextService() {
    DllAddRef();
}

TextService::~TextService() {
    if (active_) {
        Deactivate();
    }
    DllRelease();
}

HRESULT STDMETHODCALLTYPE TextService::QueryInterface(REFIID iid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;

    if (iid == __uuidof(IUnknown)) {
        *object = static_cast<ITfTextInputProcessor*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfTextInputProcessor)) {
        *object = static_cast<ITfTextInputProcessor*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfTextInputProcessorEx)) {
        *object = static_cast<ITfTextInputProcessorEx*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfKeyEventSink)) {
        *object = static_cast<ITfKeyEventSink*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfCompositionSink)) {
        *object = static_cast<ITfCompositionSink*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfThreadMgrEventSink)) {
        *object = static_cast<ITfThreadMgrEventSink*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfThreadFocusSink)) {
        *object = static_cast<ITfThreadFocusSink*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == __uuidof(ITfTextLayoutSink)) {
        *object = static_cast<ITfTextLayoutSink*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TextService::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

ULONG STDMETHODCALLTYPE TextService::Release() {
    const LONG count = InterlockedDecrement(&referenceCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>((std::max)(count, 0L));
}

HRESULT STDMETHODCALLTYPE TextService::Activate(ITfThreadMgr* threadManager, TfClientId clientId) {
    return activateInternal(threadManager, clientId, 0);
}

HRESULT STDMETHODCALLTYPE TextService::ActivateEx(ITfThreadMgr* threadManager,
                                                   TfClientId clientId, DWORD flags) {
    return activateInternal(threadManager, clientId, flags);
}

HRESULT TextService::activateInternal(ITfThreadMgr* threadManager, TfClientId clientId,
                                      DWORD flags) {
    (void)flags;
    // TF_TMAE_COMLESS hosts (console windows and other embedded hosts) are
    // activated too: key handling and edit sessions do not require COM-based
    // UI, and every optional piece below degrades gracefully on failure.
    if (threadManager == nullptr || clientId == TF_CLIENTID_NULL) {
        return E_INVALIDARG;
    }
    if (active_) {
        Deactivate();
    }

    threadManager_ = threadManager;
    threadManager_->AddRef();
    clientId_ = clientId;

    HRESULT result = threadManager_->QueryInterface(IID_ITfKeystrokeMgr,
                                                    reinterpret_cast<void**>(&keystrokeManager_));
    if (FAILED(result) || keystrokeManager_ == nullptr) {
        threadManager_->Release();
        threadManager_ = nullptr;
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    result = keystrokeManager_->AdviseKeyEventSink(clientId_, static_cast<ITfKeyEventSink*>(this),
                                                   TRUE);
    if (FAILED(result)) {
        keystrokeManager_->Release();
        keystrokeManager_ = nullptr;
        threadManager_->Release();
        threadManager_ = nullptr;
        return result;
    }
    keySinkAdvised_ = true;

    // The UI element manager is optional: without it the owned candidate
    // window covers every host, but the system candidate UI never appears.
    (void)threadManager_->QueryInterface(IID_ITfUIElementMgr,
                                         reinterpret_cast<void**>(&uiElementMgr_));

    if (SUCCEEDED(threadManager_->QueryInterface(IID_ITfSource,
                                                 reinterpret_cast<void**>(&source_))) &&
        source_ != nullptr) {
        focusSinkAdvised_ = SUCCEEDED(source_->AdviseSink(
            IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this), &focusCookie_));
        if (!focusSinkAdvised_) {
            focusCookie_ = TF_INVALID_COOKIE;
        }
        threadMgrSinkAdvised_ = SUCCEEDED(source_->AdviseSink(
            IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
            &threadMgrCookie_));
        if (!threadMgrSinkAdvised_) {
            threadMgrCookie_ = TF_INVALID_COOKIE;
        }
    }

    ITfDocumentMgr* initialDocument = nullptr;
    if (SUCCEEDED(threadManager_->GetFocus(&initialDocument)) && initialDocument != nullptr) {
        ITfContext* initialContext = nullptr;
        if (SUCCEEDED(initialDocument->GetTop(&initialContext)) && initialContext != nullptr) {
            focusedContext_ = initialContext;
        }
        initialDocument->Release();
    }

    try {
        candidateWindow_.create(g_module, [this](std::size_t index) { onCandidateClicked(index); });
        loadDictionary(g_module);
    } catch (...) {
        candidateWindow_.destroy();
    }
    registerCompositionAttribute();
    active_ = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::Deactivate() {
    if (!active_) {
        return S_FALSE;
    }

    abandonComposition();
    candidateWindow_.destroy();
    endCandidateUi();
    unadviseLayoutSink();
    saveUserSelections();
    engine_.clearContext();
    engine_.reset();

    if (uiElementMgr_ != nullptr) {
        uiElementMgr_->Release();
        uiElementMgr_ = nullptr;
    }
    if (keystrokeManager_ != nullptr) {
        if (keySinkAdvised_) {
            keystrokeManager_->UnadviseKeyEventSink(clientId_);
        }
        keystrokeManager_->Release();
        keystrokeManager_ = nullptr;
    }
    if (source_ != nullptr) {
        if (focusSinkAdvised_) {
            source_->UnadviseSink(focusCookie_);
        }
        if (threadMgrSinkAdvised_) {
            source_->UnadviseSink(threadMgrCookie_);
        }
        source_->Release();
        source_ = nullptr;
    }
    if (threadManager_ != nullptr) {
        threadManager_->Release();
        threadManager_ = nullptr;
    }
    clearCompositionAttributeRegistration();

    if (focusedContext_ != nullptr) {
        focusedContext_->Release();
        focusedContext_ = nullptr;
    }
    keySinkAdvised_ = false;
    focusSinkAdvised_ = false;
    threadMgrSinkAdvised_ = false;
    focusCookie_ = TF_INVALID_COOKIE;
    threadMgrCookie_ = TF_INVALID_COOKIE;
    clientId_ = TF_CLIENTID_NULL;
    threadFocused_ = true;
    pendingDecisionValid_ = false;
    active_ = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnSetFocus(BOOL foreground) {
    pendingDecisionValid_ = false;
    if (!foreground) {
        candidateWindow_.hide();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnTestKeyDown(ITfContext* context, WPARAM key,
                                                    LPARAM parameter, BOOL* eaten) {
    if (eaten == nullptr) {
        return E_INVALIDARG;
    }
    pendingDecision_ = classify(key, parameter);
    if (context == nullptr && !composing_ && !banglaSuggestionActive_) {
        pendingDecision_ = {};
    }
    pendingKey_ = key;
    pendingParameter_ = parameter;
    pendingDecisionValid_ = true;
    *eaten = pendingDecision_.action != KeyAction::None &&
             pendingDecision_.action != KeyAction::CommitAndForward;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnTestKeyUp(ITfContext* context, WPARAM key,
                                                  LPARAM parameter, BOOL* eaten) {
    (void)context;
    (void)key;
    (void)parameter;
    if (eaten == nullptr) {
        return E_INVALIDARG;
    }
    *eaten = FALSE;
    pendingDecisionValid_ = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnKeyDown(ITfContext* context, WPARAM key,
                                                LPARAM parameter, BOOL* eaten) {
    if (eaten == nullptr) {
        return E_INVALIDARG;
    }
    if (context == nullptr && !composing_ && !banglaSuggestionActive_) {
        *eaten = FALSE;
        return S_OK;
    }

    bool contextChanged = false;
    if (context != nullptr && composing_ && context_ != nullptr && context != context_) {
        contextChanged = true;
        // The host is changing documents; do not carry the old range or
        // composition decoration into the new context.
        abandonComposition();
    }
    if (context != nullptr && suggestionContext_ != nullptr && context != suggestionContext_) {
        clearBanglaSuggestions();
    }

    const bool usePending = !contextChanged && pendingDecisionValid_ && pendingKey_ == key &&
                            pendingParameter_ == parameter;
    const KeyDecision decision = usePending ? pendingDecision_ : classify(key, parameter);
    pendingDecisionValid_ = false;
    if (decision.action == KeyAction::None) {
        *eaten = FALSE;
        return S_OK;
    }
    try {
        return execute(decision, context, eaten);
    } catch (...) {
        engine_.reset();
        clearBanglaSuggestions();
        *eaten = decision.action != KeyAction::CommitAndForward;
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE TextService::OnKeyUp(ITfContext* context, WPARAM key, LPARAM parameter,
                                              BOOL* eaten) {
    (void)context;
    (void)key;
    (void)parameter;
    if (eaten == nullptr) {
        return E_INVALIDARG;
    }
    *eaten = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnPreservedKey(ITfContext* context, REFGUID command,
                                                    BOOL* eaten) {
    (void)context;
    (void)command;
    if (eaten == nullptr) {
        return E_INVALIDARG;
    }
    *eaten = FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnInitDocumentMgr(ITfDocumentMgr* documentManager) {
    (void)documentManager;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnUninitDocumentMgr(ITfDocumentMgr* documentManager) {
    (void)documentManager;
    abandonComposition();
    if (focusedContext_ != nullptr) {
        focusedContext_->Release();
        focusedContext_ = nullptr;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnSetFocus(ITfDocumentMgr* focused,
                                                   ITfDocumentMgr* previous) {
    (void)previous;
    abandonComposition();
    // The anchor belongs to the document that produced it; a new document
    // must not inherit it before it has described a rectangle of its own.
    hasCandidateAnchor_ = false;

    ITfContext* newContext = nullptr;
    if (focused != nullptr) {
        focused->GetTop(&newContext);
    }
    if (focusedContext_ != nullptr) {
        focusedContext_->Release();
    }
    focusedContext_ = newContext;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnPushContext(ITfContext* context) {
    if (!composing_ && context != nullptr) {
        if (focusedContext_ != nullptr) {
            focusedContext_->Release();
        }
        focusedContext_ = context;
        focusedContext_->AddRef();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnPopContext(ITfContext* context) {
    if (focusedContext_ == context) {
        focusedContext_->Release();
        focusedContext_ = nullptr;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnCompositionTerminated(TfEditCookie cookie,
                                                              ITfComposition* composition) {
    (void)cookie;
    if (composition == composition_) {
        // The host closed the composition, so the underline over any surviving
        // text is removed before the state is dropped.
        requestCompositionAttributeClear();
        clearCompositionState();
        engine_.reset();
        candidateWindow_.clear();
        clearBanglaSuggestions();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnSetThreadFocus() {
    threadFocused_ = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE TextService::OnKillThreadFocus() {
    threadFocused_ = false;
    pendingDecisionValid_ = false;
    candidateWindow_.hide();
    abandonComposition();
    hasCandidateAnchor_ = false;
    if (focusedContext_ != nullptr) {
        focusedContext_->Release();
        focusedContext_ = nullptr;
    }
    engine_.reset();
    return S_OK;
}

TextService::KeyDecision TextService::classify(WPARAM key, LPARAM parameter) const noexcept {
    KeyDecision decision;
    const bool controlSpace = key == VK_SPACE && isDown(VK_CONTROL) && !isDown(VK_MENU) &&
                              !isDown(VK_LMENU) && !isDown(VK_RMENU) && !isDown(VK_LWIN) &&
                              !isDown(VK_RWIN);
    // Ctrl+Backspace means "delete the word" and has to be handled before the
    // modifier check below. Letting it through would let the host edit inside
    // the composition range, leaving the phonetic buffer and the document out
    // of sync. The composition is always a single word, so removing all of it
    // is the word-level delete the user asked for.
    const bool controlBackspace = key == VK_BACK && isDown(VK_CONTROL) &&
                                  !isDown(VK_MENU) && !isDown(VK_LMENU) &&
                                  !isDown(VK_RMENU) && !isDown(VK_LWIN) &&
                                  !isDown(VK_RWIN);
    if (controlBackspace && (composing_ || banglaSuggestionActive_)) {
        decision.action = KeyAction::DeleteComposition;
        return decision;
    }
    if (hasCommandModifier() && !controlSpace) {
        return decision;
    }

    const bool candidatesVisible = candidateWindow_.isVisible() && candidateWindow_.count() > 1;
    if (candidatesVisible) {
        switch (key) {
        case VK_UP:
            decision.action = KeyAction::CandidatePrevious;
            return decision;
        case VK_DOWN:
            decision.action = KeyAction::CandidateNext;
            return decision;
        case VK_TAB:
            decision.action = isDown(VK_SHIFT) ? KeyAction::CandidatePrevious
                                               : KeyAction::CandidateNext;
            return decision;
        case VK_PRIOR:
            decision.action = KeyAction::CandidatePagePrevious;
            return decision;
        case VK_NEXT:
            decision.action = KeyAction::CandidatePageNext;
            return decision;
        default:
            break;
        }
        if (key >= '1' && key <= '9' && !isDown(VK_SHIFT) &&
            static_cast<std::size_t>(key - '1') < candidateWindow_.count()) {
            decision.action = KeyAction::CandidateSelect;
            decision.candidateIndex = static_cast<std::size_t>(key - '1');
            return decision;
        }
    }

    switch (key) {
    case VK_BACK:
        if (composing_) {
            decision.action = KeyAction::Backspace;
        } else if (banglaSuggestionActive_) {
            decision.action = KeyAction::CommitAndForward;
        }
        return decision;
    case VK_SPACE:
        if (controlSpace && !composing_ && !banglaSuggestionActive_) {
            decision.action = KeyAction::BanglaSuggest;
        } else if (!controlSpace && (composing_ || banglaSuggestionActive_)) {
            decision.action = KeyAction::Space;
        }
        return decision;
    case VK_RETURN:
        if (composing_ || banglaSuggestionActive_) {
            decision.action = KeyAction::Enter;
        }
        return decision;
    case VK_ESCAPE:
        if (composing_ || banglaSuggestionActive_) {
            decision.action = KeyAction::Escape;
        }
        return decision;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_TAB:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
        if (composing_ || banglaSuggestionActive_) {
            decision.action = KeyAction::CommitAndForward;
        }
        return decision;
    default:
        break;
    }

    const WCHAR character = characterFromKey(key, parameter);
    if (!isInputCharacter(character)) {
        return decision;
    }

    // Avro convention: the main keyboard period is the dari key, and only the
    // numpad decimal types a literal doshomik dot. Away from an open phonetic
    // composition the period would otherwise reach the application as a raw
    // ASCII dot, so it is claimed here; with a Ctrl+Space suggestion open,
    // either key commits the picked word first.
    if (!composing_ && character == L'.' &&
        (banglaSuggestionActive_ || key != VK_DECIMAL)) {
        decision.action = KeyAction::Dari;
        decision.virtualKey = static_cast<UINT>(key);
        return decision;
    }

    if (!composing_ && !banglaSuggestionActive_ &&
        !((character >= L'a' && character <= L'z') ||
          (character >= L'A' && character <= L'Z') ||
          (character >= L'0' && character <= L'9'))) {
        return decision;
    }

    decision.action = KeyAction::Character;
    decision.character = character;
    decision.virtualKey = static_cast<UINT>(key);
    return decision;
}

WCHAR TextService::characterFromKey(WPARAM key, LPARAM parameter) noexcept {
    if (key == VK_PACKET) {
        // Supplementary-plane packets are passed through rather than split
        // into an unusable UTF-16 surrogate in the phonetic buffer.
        return static_cast<WCHAR>(parameter);
    }

    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) {
        return 0;
    }
    static const HKL usLayout = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    const HKL layout = usLayout != nullptr ? usLayout : GetKeyboardLayout(0);
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    WCHAR output[8]{};
    const int count = ToUnicodeEx(static_cast<UINT>(key), scanCode, keyboardState, output,
                                  ARRAYSIZE(output), 0, layout);
    return count == 1 ? output[0] : 0;
}

bool TextService::isInputCharacter(WCHAR character) noexcept {
    if (character >= L'a' && character <= L'z') {
        return true;
    }
    if (character >= L'A' && character <= L'Z') {
        return true;
    }
    if (character >= L'0' && character <= L'9') {
        return true;
    }
    switch (character) {
    case L',':
    case L'.':
    case L';':
    case L':':
    case L'?':
    case L'!':
    case L'\'':
    case L'-':
    case L'_':
    case L'/':
    case L'\\':
    case L'=':
    case L'+':
    case L'^':
    case L'$':
        return true;
    default:
        return false;
    }
}

HRESULT TextService::execute(const KeyDecision& decision, ITfContext* context, BOOL* eaten) {
    *eaten = FALSE;
    switch (decision.action) {
    case KeyAction::None:
        return S_OK;
    case KeyAction::Character:
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            clearBanglaSuggestions();
        }
        // The numpad decimal is the doshomik key: its dot must stay literal
        // even where the layout would rewrite '.' as dari, so it enters the
        // buffer through the backslash escape.
        if (decision.character == L'.' && decision.virtualKey == VK_DECIMAL) {
            if (!engine_.appendCodePoint(U'\\')) {
                return S_OK;
            }
        }
        if (!engine_.appendCodePoint(static_cast<char32_t>(decision.character))) {
            return S_OK;
        }
        if (FAILED(startOrUpdate(context))) {
            if (composing_) {
                (void)cancelComposition();
            } else {
                engine_.reset();
            }
            candidateWindow_.clear();
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::Backspace:
        *eaten = TRUE;
        if (!engine_.backspace()) {
            return cancelComposition();
        }
        if (engine_.rawInput().empty()) {
            if (FAILED(cancelComposition())) {
                engine_.reset();
                candidateWindow_.clear();
                return S_OK;
            }
            *eaten = TRUE;
            return S_OK;
        }
        if (FAILED(startOrUpdate(context))) {
            if (composing_) {
                (void)cancelComposition();
            } else {
                engine_.reset();
            }
            candidateWindow_.clear();
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::Space:
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            if (FAILED(finishBanglaSuggestion(L" "))) {
                return S_OK;
            }
        } else if (FAILED(finishComposition(L" ", false))) {
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::Enter:
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            if (FAILED(finishBanglaSuggestion(L"\r"))) {
                return S_OK;
            }
        } else if (FAILED(finishComposition(L"\r", false))) {
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::Escape:
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            clearBanglaSuggestions();
        } else if (FAILED(cancelComposition())) {
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::DeleteComposition:
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            clearBanglaSuggestions();
        } else if (FAILED(cancelComposition())) {
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::Dari: {
        // The main period becomes the dari (।); the numpad decimal keeps a
        // literal dot. With a Ctrl+Space suggestion open, either key commits
        // the picked word first. A failed edit session degrades to the
        // application's own key handling rather than eating the keystroke.
        const wchar_t* trailing = decision.virtualKey == VK_DECIMAL ? L"." : L"\x0964";
        *eaten = TRUE;
        if (banglaSuggestionActive_) {
            if (FAILED(finishBanglaSuggestion(trailing))) {
                *eaten = FALSE;
            }
            return S_OK;
        }
        ITfContext* resolved = nullptr;
        const HRESULT resolvedResult = resolveContext(context, &resolved);
        if (FAILED(resolvedResult)) {
            *eaten = FALSE;
            return S_OK;
        }
        const HRESULT inserted = requestEdit(resolved, EditMode::Insert, trailing);
        resolved->Release();
        if (FAILED(inserted)) {
            *eaten = FALSE;
        }
        return S_OK;
    }
    case KeyAction::CandidatePrevious:
        candidateWindow_.selectPrevious();
        notifyCandidateUiSelection();
        *eaten = TRUE;
        return S_OK;
    case KeyAction::CandidateNext:
        candidateWindow_.selectNext();
        notifyCandidateUiSelection();
        *eaten = TRUE;
        return S_OK;
    case KeyAction::CandidatePagePrevious:
        candidateWindow_.select(0);
        notifyCandidateUiSelection();
        *eaten = TRUE;
        return S_OK;
    case KeyAction::CandidatePageNext:
        candidateWindow_.select(candidateWindow_.count() - 1);
        notifyCandidateUiSelection();
        *eaten = TRUE;
        return S_OK;
    case KeyAction::CandidateSelect:
        *eaten = TRUE;
        if (decision.candidateIndex >= candidateWindow_.count()) {
            return S_OK;
        }
        candidateWindow_.select(decision.candidateIndex);
        if (banglaSuggestionActive_) {
            if (FAILED(finishBanglaSuggestion({}))) {
                return S_OK;
            }
        } else if (FAILED(finishComposition({}, false))) {
            return S_OK;
        }
        *eaten = TRUE;
        return S_OK;
    case KeyAction::CommitAndForward:
        if (banglaSuggestionActive_) {
            clearBanglaSuggestions();
            *eaten = FALSE;
        } else if (SUCCEEDED(finishComposition({}, true))) {
            *eaten = FALSE;
        } else {
            // Do not let the application move the caret while an old
            // composition is still owned by this text service.
            *eaten = TRUE;
            try {
                cancelComposition();
            } catch (...) {
            }
        }
        return S_OK;
    case KeyAction::BanglaSuggest:
        *eaten = TRUE;
        beginBanglaSuggestions(context);
        return S_OK;
    default:
        return S_OK;
    }
}

HRESULT TextService::requestEdit(ITfContext* context, EditMode mode, const std::wstring& text,
                                const std::wstring& trailing, std::size_t prefixLength) {
    if (context == nullptr || clientId_ == TF_CLIENTID_NULL) {
        return E_INVALIDARG;
    }
    auto* session = new (std::nothrow) EditSession(this, context, mode, text, trailing,
                                                     prefixLength);
    if (session == nullptr) {
        return E_OUTOFMEMORY;
    }

    HRESULT sessionResult = E_FAIL;
    const HRESULT result = context->RequestEditSession(clientId_, session,
                                                       TF_ES_SYNC | TF_ES_READWRITE,
                                                       &sessionResult);
    session->Release();
    if (FAILED(result)) {
        return result;
    }
    return sessionResult;
}

HRESULT TextService::resolveContext(ITfContext* supplied, ITfContext** context) const noexcept {
    if (context == nullptr) {
        return E_POINTER;
    }
    *context = nullptr;
    try {
        if (supplied != nullptr) {
            supplied->AddRef();
            *context = supplied;
            return S_OK;
        }

        if (threadManager_ != nullptr) {
            ITfDocumentMgr* documentManager = nullptr;
            if (SUCCEEDED(threadManager_->GetFocus(&documentManager)) && documentManager != nullptr) {
                const HRESULT result = documentManager->GetTop(context);
                documentManager->Release();
                if (SUCCEEDED(result) && *context != nullptr) {
                    return S_OK;
                }
                *context = nullptr;
            }
        }
        if (focusedContext_ != nullptr) {
            focusedContext_->AddRef();
            *context = focusedContext_;
            return S_OK;
        }
        if (context_ != nullptr) {
            context_->AddRef();
            *context = context_;
            return S_OK;
        }
        return TF_E_NOSELECTION;
    } catch (...) {
        *context = nullptr;
        return E_OUTOFMEMORY;
    }
}

HRESULT TextService::startOrUpdate(ITfContext* supplied) {
    ITfContext* context = nullptr;
    HRESULT result = resolveContext(supplied, &context);
    if (FAILED(result)) {
        return result;
    }

    const std::wstring output = toWide(engine_.directOutput());
    if (output.empty()) {
        context->Release();
        return E_INVALIDARG;
    }
    result = requestEdit(context, composing_ ? EditMode::Update : EditMode::Start, output);
    context->Release();
    if (SUCCEEDED(result)) {
        composing_ = true;
    }
    return result;
}

HRESULT TextService::finishComposition(const std::wstring& trailing, bool forwardKey) {
    (void)forwardKey;
    ITfContext* context = nullptr;
    HRESULT result = S_OK;
    if (context_ != nullptr) {
        context = context_;
        context->AddRef();
    } else {
        result = resolveContext(nullptr, &context);
    }
    if (FAILED(result)) {
        return result;
    }

    std::wstring selected = candidateWindow_.selected();
    if (selected.empty()) {
        selected = toWide(engine_.directOutput());
    }
    const std::string committedWord = toUtf8(selected);
    result = requestEdit(context, EditMode::Finish, selected, trailing);
    context->Release();
    if (SUCCEEDED(result)) {
        // An explicit pick (arrows, digits, mouse) that left slot zero is
        // remembered so the same key offers that word first next time.
        if (candidateWindow_.count() > 1 && candidateWindow_.selectedIndex() != 0 &&
            !engine_.rawInput().empty() && !committedWord.empty()) {
            engine_.setUserDay(currentDayStamp());
            engine_.recordSelection(engine_.rawInput(), committedWord, currentDayStamp());
            ++unsavedSelections_;
            if (unsavedSelections_ >= 8) {
                saveUserSelections();
            }
        }
        clearCompositionState();
        engine_.reset();
        if (!committedWord.empty()) {
            engine_.setContextWord(committedWord);
        }
        candidateWindow_.clear();
    }
    return result;
}

HRESULT TextService::cancelComposition() {
    ITfContext* context = nullptr;
    HRESULT result = S_OK;
    if (context_ != nullptr) {
        context = context_;
        context->AddRef();
    } else {
        result = resolveContext(nullptr, &context);
    }
    if (FAILED(result)) {
        return result;
    }
    result = requestEdit(context, EditMode::Cancel, {});
    context->Release();
    if (SUCCEEDED(result)) {
        clearCompositionState();
        engine_.reset();
        candidateWindow_.clear();
    }
    return result;
}

HRESULT TextService::beginBanglaSuggestions(ITfContext* supplied) {
    ITfContext* context = nullptr;
    const HRESULT result = resolveContext(supplied, &context);
    if (FAILED(result)) {
        return result;
    }
    clearBanglaSuggestions();
    const HRESULT editResult = requestEdit(context, EditMode::ReadBanglaPrefix, {});
    context->Release();
    if (SUCCEEDED(editResult) && !banglaSuggestionActive_) {
        return S_FALSE;
    }
    return editResult;
}

HRESULT TextService::finishBanglaSuggestion(const std::wstring& trailing) {
    if (!banglaSuggestionActive_ || suggestionContext_ == nullptr) {
        return E_UNEXPECTED;
    }
    const std::wstring selected = candidateWindow_.selected();
    if (selected.empty()) {
        return E_UNEXPECTED;
    }

    const std::string committedWord = toUtf8(selected);
    // An explicit pick (arrows, digits, mouse) that left slot zero is
    // remembered under the Bangla prefix, mirroring the phonetic path, so
    // the same prefix suggests that word first next time.
    const bool explicitPick = candidateWindow_.count() > 1 &&
                              candidateWindow_.selectedIndex() != 0;
    const std::string prefix = banglaPrefix_;
    const HRESULT result = requestEdit(suggestionContext_, EditMode::ReplaceBanglaPrefix,
                                        selected, trailing, banglaPrefixLength_);
    if (SUCCEEDED(result)) {
        if (explicitPick && !prefix.empty() && !committedWord.empty()) {
            engine_.setUserDay(currentDayStamp());
            engine_.recordSelection(prefix, committedWord, currentDayStamp());
            ++unsavedSelections_;
            if (unsavedSelections_ >= 8) {
                saveUserSelections();
            }
        }
        clearBanglaSuggestions();
        if (!committedWord.empty()) {
            engine_.setContextWord(committedWord);
        }
    }
    return result;
}

void TextService::clearBanglaSuggestions() noexcept {
    if (suggestionContext_ != nullptr) {
        suggestionContext_->Release();
        suggestionContext_ = nullptr;
    }
    banglaCandidates_.clear();
    banglaPrefixLength_ = 0;
    banglaPrefix_.clear();
    banglaSuggestionActive_ = false;
    endCandidateUi();
    unadviseLayoutSink();
    candidateWindow_.clear();
}

void TextService::setBanglaSuggestions(std::string prefix, ITfContext* context) {
    clearBanglaSuggestions();
    if (context == nullptr || prefix.empty()) {
        return;
    }

    const std::wstring original = toWide(prefix);
    const std::string normalized = normalizeBanglaUtf8(prefix);
    if (original.empty() || normalized.empty()) {
        return;
    }

    banglaCandidates_ = engine_.suggestBangla(normalized, 9);
    if (banglaCandidates_.size() < 2) {
        banglaCandidates_.clear();
        return;
    }

    suggestionContext_ = context;
    suggestionContext_->AddRef();
    banglaPrefixLength_ = original.size();
    // The normalized form is the memory key, matching the read side inside
    // suggestBangla().
    banglaPrefix_ = normalized;
    banglaSuggestionActive_ = true;

    std::vector<std::wstring> display;
    display.reserve(banglaCandidates_.size());
    for (const auto& candidate : banglaCandidates_) {
        std::wstring text = toWide(candidate.utf8);
        if (text.empty()) {
            continue;
        }
        display.push_back(std::move(text));
        if (display.size() == 9) {
            break;
        }
    }
    candidateWindow_.setCandidates(std::move(display));
    syncCandidateUi();
    adviseLayoutSink();
}

void TextService::updateCandidates() {
    std::vector<std::wstring> candidates;
    candidates.reserve(engine_.candidates().size());
    for (const auto& candidate : engine_.candidates()) {
        std::wstring text = toWide(candidate.utf8);
        if (text.empty()) {
            continue;
        }
        candidates.push_back(std::move(text));
        if (candidates.size() == 9) {
            break;
        }
    }
    candidateWindow_.setCandidates(std::move(candidates));
    if (candidateWindow_.count() < 2) {
        candidateWindow_.hide();
        endCandidateUi();
        return;
    }
    syncCandidateUi();
}

void TextService::positionCandidates(TfEditCookie cookie, ITfContext* context) {
    if (context == nullptr || candidateWindow_.count() < 2 ||
        systemRendersCandidates_ ||
        (composition_ == nullptr && !banglaSuggestionActive_)) {
        return;
    }

    ITfRange* range = nullptr;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (composition_ != nullptr) {
        if (FAILED(composition_->GetRange(&range)) || range == nullptr) {
            return;
        }
    } else {
        const HRESULT selectionResult = context->GetSelection(
            cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(selectionResult) || fetched != 1 || selection.range == nullptr) {
            // GetSelection may still hand back a range when it reports fewer
            // than the requested number, so this early exit owns the release.
            if (selection.range != nullptr) {
                selection.range->Release();
            }
            return;
        }
        range = selection.range;
    }

    ITfContextView* view = nullptr;
    RECT rectangle{};
    bool positioned = false;
    if (SUCCEEDED(context->GetActiveView(&view)) && view != nullptr) {
        BOOL clipped = FALSE;
        // A zero-height rectangle is not a placement: some hosts return an
        // empty rect before they have laid the composition out.
        positioned = SUCCEEDED(view->GetTextExt(cookie, range, &rectangle, &clipped)) &&
                     rectangle.bottom > rectangle.top;
        view->Release();
    }
    if (composition_ != nullptr) {
        range->Release();
    } else if (selection.range != nullptr) {
        selection.range->Release();
    }

    // A host that cannot describe the composition rectangle (CUAS emulation,
    // views that lay out on paint) would otherwise keep the window hidden
    // even though the composition itself works. Fall back to the thread's
    // real caret, then to the last rectangle this document did describe.
    if (positioned) {
        lastCandidateX_ = rectangle.left;
        lastCandidateY_ = rectangle.bottom + 5;
        hasCandidateAnchor_ = true;
        candidateWindow_.show(lastCandidateX_, lastCandidateY_);
        return;
    }

    int x = 0;
    int y = 0;
    if (caretAnchor(&x, &y)) {
        candidateWindow_.show(x, y + 5);
        return;
    }
    if (hasCandidateAnchor_) {
        candidateWindow_.show(lastCandidateX_, lastCandidateY_);
        return;
    }
    candidateWindow_.hide();
}

void TextService::clearCompositionState() noexcept {
    endCandidateUi();
    unadviseLayoutSink();
    if (composition_ != nullptr) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (context_ != nullptr) {
        context_->Release();
        context_ = nullptr;
    }
    composing_ = false;
}

void TextService::abandonComposition() noexcept {
    if (composing_ || composition_ != nullptr) {
        bool ended = false;
        try {
            ended = SUCCEEDED(cancelComposition());
            if (!ended && context_ != nullptr) {
                ended = SUCCEEDED(requestEdit(context_, EditMode::Cancel, {}));
            }
        } catch (...) {
            ended = false;
        }
        if (!ended) {
            // The host may refuse a synchronous cancel. Remove the decoration
            // in a fresh session before dropping our local references.
            requestCompositionAttributeClear();
        }
    }
    clearCompositionState();
    clearBanglaSuggestions();
    engine_.reset();
    candidateWindow_.clear();
}

void TextService::registerCompositionAttribute() noexcept {
    clearCompositionAttributeRegistration();
    ITfCategoryMgr* categories = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfCategoryMgr, reinterpret_cast<void**>(&categories))) ||
        categories == nullptr) {
        return;
    }
    categoryManager_ = categories;
    // Hosts resolve GUID_PROP_ATTRIBUTE values through the category manager, so
    // the display attribute has to be a registered GUID atom. A failure only
    // costs the underline, never typing.
    if (FAILED(categories->RegisterGUID(kCompositionDisplayAttributeGuid,
                                        &compositionAttributeAtom_))) {
        compositionAttributeAtom_ = TF_INVALID_GUIDATOM;
    }
}

void TextService::clearCompositionAttributeRegistration() noexcept {
    compositionAttributeAtom_ = TF_INVALID_GUIDATOM;
    if (categoryManager_ != nullptr) {
        categoryManager_->Release();
        categoryManager_ = nullptr;
    }
}

void TextService::requestCompositionAttributeClear() noexcept {
    if (context_ == nullptr || clientId_ == TF_CLIENTID_NULL) {
        return;
    }
    // A composition that is still open holds the underline, and the edit
    // cookies handed to composition callbacks belong to earlier sessions, so
    // the attribute is only ever removed from a current one.
    BOOL inWriteSession = FALSE;
    if (FAILED(context_->InWriteSession(clientId_, &inWriteSession)) ||
        inWriteSession != FALSE) {
        return;
    }
    try {
        (void)requestEdit(context_, EditMode::ClearCompositionAttribute, {});
    } catch (...) {
        // Losing the underline is acceptable; losing the composition is not.
    }
}

void TextService::onCandidateClicked(std::size_t index) {
    try {
        HRESULT result = S_OK;
        if (banglaSuggestionActive_ && index < banglaCandidates_.size()) {
            candidateWindow_.select(index);
            result = finishBanglaSuggestion({});
        } else if (composing_ && index < engine_.candidates().size()) {
            candidateWindow_.select(index);
            result = finishComposition({}, false);
        }
        if (FAILED(result)) {
            abandonComposition();
        }
    } catch (...) {
        abandonComposition();
    }
}

// Publishes the current candidate list through the TSF UI element mechanism.
// Returns true when the system renders the list itself, in which case the
// owned candidate window must stay hidden.
bool TextService::syncCandidateUi() {
    if (uiElementMgr_ == nullptr || candidateWindow_.count() < 2 ||
        (composition_ == nullptr && !banglaSuggestionActive_)) {
        endCandidateUi();
        return false;
    }
    if (candidateUiElement_ == nullptr) {
        auto* element = new (std::nothrow) CandidateListUiElement(this);
        if (element == nullptr) {
            return false;
        }
        // On input TRUE means "the text service would like to draw its own
        // UI"; on return FALSE means the host takes the element and renders
        // the system candidate UI from it instead.
        BOOL show = TRUE;
        const HRESULT result = uiElementMgr_->BeginUIElement(element, &show, &uiElementId_);
        if (FAILED(result)) {
            element->Release();
            uiElementId_ = TF_INVALID_UIELEMENTID;
            return false;
        }
        candidateUiElement_ = element;
        systemRendersCandidates_ = show == FALSE;
        if (systemRendersCandidates_) {
            candidateWindow_.hide();
        }
    } else if (systemRendersCandidates_) {
        uiElementMgr_->UpdateUIElement(uiElementId_);
    }
    return systemRendersCandidates_;
}

void TextService::endCandidateUi() noexcept {
    systemRendersCandidates_ = false;
    if (uiElementMgr_ != nullptr && candidateUiElement_ != nullptr &&
        uiElementId_ != TF_INVALID_UIELEMENTID) {
        uiElementMgr_->EndUIElement(uiElementId_);
    }
    uiElementId_ = TF_INVALID_UIELEMENTID;
    if (candidateUiElement_ != nullptr) {
        candidateUiElement_->Release();
        candidateUiElement_ = nullptr;
    }
}

void TextService::notifyCandidateUiSelection() noexcept {
    if (systemRendersCandidates_ && uiElementMgr_ != nullptr &&
        candidateUiElement_ != nullptr && uiElementId_ != TF_INVALID_UIELEMENTID) {
        uiElementMgr_->UpdateUIElement(uiElementId_);
    }
}

void TextService::setCandidateSelection(std::size_t index) {
    if (index >= candidateWindow_.count()) {
        return;
    }
    candidateWindow_.select(index);
    notifyCandidateUiSelection();
}

ITfContext* TextService::compositionContext() const noexcept {
    return context_ != nullptr ? context_ : suggestionContext_;
}

void TextService::adviseLayoutSink() {
    if (layoutSinkAdvised_ || clientId_ == TF_CLIENTID_NULL) {
        return;
    }
    ITfContext* context = compositionContext();
    if (context == nullptr) {
        return;
    }
    ITfSource* source = nullptr;
    if (FAILED(context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) ||
        source == nullptr) {
        return;
    }
    const HRESULT result = source->AdviseSink(IID_ITfTextLayoutSink,
                                              static_cast<ITfTextLayoutSink*>(this),
                                              &layoutSinkCookie_);
    source->Release();
    if (SUCCEEDED(result)) {
        layoutContext_ = context;
        layoutContext_->AddRef();
        layoutSinkAdvised_ = true;
    }
}

void TextService::unadviseLayoutSink() noexcept {
    if (layoutContext_ != nullptr) {
        if (layoutSinkAdvised_) {
            ITfSource* source = nullptr;
            if (SUCCEEDED(layoutContext_->QueryInterface(
                    IID_ITfSource, reinterpret_cast<void**>(&source))) &&
                source != nullptr) {
                source->UnadviseSink(layoutSinkCookie_);
                source->Release();
            }
        }
        layoutContext_->Release();
        layoutContext_ = nullptr;
    }
    layoutSinkAdvised_ = false;
    layoutSinkCookie_ = TF_INVALID_COOKIE;
}

// Runs the placement retry outside the composing edit session. A synchronous
// read is tried first because the layout notification arrives at a point
// where hosts grant it; the asynchronous read covers the hosts that refuse
// synchronous sessions outside key processing.
void TextService::requestPositioning() {
    if (clientId_ == TF_CLIENTID_NULL ||
        (composition_ == nullptr && !banglaSuggestionActive_)) {
        return;
    }
    ITfContext* context = compositionContext();
    if (context == nullptr) {
        return;
    }
    HRESULT sessionResult = E_FAIL;
    auto* session = new (std::nothrow) EditSession(this, context, EditMode::Position, {}, {}, 0);
    if (session == nullptr) {
        return;
    }
    const HRESULT result =
        context->RequestEditSession(clientId_, session, TF_ES_SYNC | TF_ES_READ, &sessionResult);
    session->Release();
    if (SUCCEEDED(result) && SUCCEEDED(sessionResult)) {
        return;
    }
    auto* retry = new (std::nothrow) EditSession(this, context, EditMode::Position, {}, {}, 0);
    if (retry != nullptr) {
        HRESULT retryResult = E_FAIL;
        (void)context->RequestEditSession(clientId_, retry, TF_ES_ASYNC | TF_ES_READ,
                                          &retryResult);
        retry->Release();
    }
}

HRESULT STDMETHODCALLTYPE TextService::OnLayoutChange(ITfContext* context, TfLayoutCode code,
                                                      ITfContextView* view) {
    (void)view;
    if (code == TF_LC_DESTROY) {
        // The host tore the view down; the anchor would only mislead.
        candidateWindow_.hide();
        hasCandidateAnchor_ = false;
        return S_OK;
    }
    if (code != TF_LC_CHANGE || context == nullptr || context != compositionContext() ||
        (!composing_ && !banglaSuggestionActive_)) {
        return S_OK;
    }
    requestPositioning();
    return S_OK;
}

// Reads a BGDX image from beside the module. Returns an empty string when the
// file is absent, unreadable, oversized, or truncated; a missing dictionary must
// never prevent phonetic typing.
std::string readDictionaryBesideModule(const std::wstring& directory, const wchar_t* name,
                                      std::size_t maximumBytes) {
    const HANDLE file =
        CreateFileW((directory + name).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
        CloseHandle(file);
        return {};
    }

    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t total = 0;
    bool success = true;
    while (total < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk =
            static_cast<DWORD>((std::min)(bytes.size() - total,
                                          static_cast<std::size_t>(
                                              (std::numeric_limits<DWORD>::max)())));
        if (!ReadFile(file, bytes.data() + total, chunk, &read, nullptr) || read == 0) {
            success = false;
            break;
        }
        total += read;
    }
    CloseHandle(file);
    if (!success || total != bytes.size()) {
        return {};
    }
    return bytes;
}

void TextService::loadDictionary(HINSTANCE instance) noexcept {
    try {
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(instance, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0 || length >= static_cast<DWORD>(path.size())) {
            return;
        }

        std::wstring directory(path.data(), length);
        const std::size_t separator = directory.find_last_of(L"\\/");
        if (separator == std::wstring::npos) {
            return;
        }
        directory.resize(separator + 1);

        std::string bytes = readDictionaryBesideModule(directory, L"LekhoniCore.bgk",
                                                       kMaximumDictionaryBytes);
        if (!bytes.empty()) {
            engine_.loadDictionary(std::move(bytes));
        }

        // The phrase layer is optional. It is small, project-authored, and
        // consulted before the core dictionary, so a user can correct a
        // spelling without regenerating the core dictionary.
        bytes = readDictionaryBesideModule(directory, L"LekhoniPhrases.bgk",
                                           kMaximumPhraseBytes);
        if (!bytes.empty()) {
            engine_.loadPhrases(std::move(bytes));
        }

        // The Avro Phonetic layout table is optional; without it the built-in
        // rules drive transliteration.
        bytes = readDictionaryBesideModule(directory, L"AvroLayout.dat",
                                           kMaximumPhraseBytes);
        if (!bytes.empty()) {
            bn::setSharedAvroLayout(std::move(bytes));
        }

        // User selection memory lives under %LOCALAPPDATA%; loading it is
        // best effort and never delays activation.
        loadUserSelections();

        // The trigram next-word table is optional runtime data.
        bytes = readDictionaryBesideModule(directory, L"LekhoniTrigrams.dat",
                                           kMaximumPhraseBytes);
        if (!bytes.empty()) {
            engine_.loadTrigrams(std::move(bytes));
        }
        engine_.setUserDay(currentDayStamp());
    } catch (...) {
        // A missing or invalid dictionary must never prevent phonetic typing.
    }
}


std::uint32_t TextService::currentDayStamp() noexcept {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const ULARGE_INTEGER large{fileTime.dwLowDateTime, fileTime.dwHighDateTime};
    return static_cast<std::uint32_t>(large.QuadPart / 864000000000ULL);
}

void TextService::loadUserSelections() {
    try {
        wchar_t root[1024]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", root, 1024) == 0) {
            return;
        }
        userSelectionsPath_ = std::wstring(root) + L"/Lekhoni/selections.dat";
        const HANDLE file = CreateFileW(userSelectionsPath_.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
            size.QuadPart <= 4 * 1024 * 1024) {
            std::string bytes(static_cast<std::size_t>(size.QuadPart), char(0));
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                         nullptr) && read == bytes.size()) {
                engine_.loadUserSelections(std::move(bytes));
            }
        }
        CloseHandle(file);
        savedSelectionCount_ = engine_.userSelectionCount();
        unsavedSelections_ = 0;
    } catch (...) {
        // A broken memory file must never block typing.
    }
}

bool TextService::saveUserSelections() {
    try {
        if (userSelectionsPath_.empty()) {
            return false;
        }
        const std::string bytes = engine_.userSelectionsBytes();
        if (bytes.empty()) {
            return false;
        }
        std::wstring directory = userSelectionsPath_;
        const std::size_t separator = directory.find_last_of(L"/");
        if (separator == std::wstring::npos) {
            return false;
        }
        directory.resize(separator);
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring temporary = userSelectionsPath_ + L".tmp";
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD written = 0;
        const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                  &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || written != bytes.size()) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), userSelectionsPath_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        savedSelectionCount_ = engine_.userSelectionCount();
        unsavedSelections_ = 0;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace lekho::win
