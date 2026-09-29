#pragma once

#include "CandidateWindow.h"

#include "lekhoni/InputEngine.h"

#include <msctf.h>
#include <windows.h>

#include <cstddef>
#include <string>

namespace lekho::win {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfThreadMgrEventSink,
                          public ITfThreadFocusSink {
public:
    TextService();
    ~TextService();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* threadManager, TfClientId clientId) override;
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* threadManager, TfClientId clientId,
                                         DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Deactivate() override;

    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM key, LPARAM parameter,
                                              BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM key, LPARAM parameter,
                                            BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM key, LPARAM parameter,
                                        BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM key, LPARAM parameter,
                                      BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID command,
                                              BOOL* eaten) override;

    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie,
                                                       ITfComposition* composition) override;

    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr* documentManager) override;
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* documentManager) override;
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focused,
                                          ITfDocumentMgr* previous) override;
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override;
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override;

    HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override;
    HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override;

private:
    friend class EditSession;

    enum class KeyAction {
        None,
        Character,
        Dari,
        Backspace,
        Space,
        Enter,
        Escape,
        DeleteComposition,
        CandidatePrevious,
        CandidateNext,
        CandidatePagePrevious,
        CandidatePageNext,
        CandidateSelect,
        CommitAndForward,
        BanglaSuggest,
    };

    enum class EditMode {
        Start,
        Update,
        Finish,
        Cancel,
        Insert,
        ReadBanglaPrefix,
        ReplaceBanglaPrefix,
        ClearCompositionAttribute,
    };

    struct KeyDecision {
        KeyAction action = KeyAction::None;
        WCHAR character = 0;
        // The virtual key survives alongside the character because the
        // character alone cannot tell the numpad decimal from the main
        // keyboard period; the two carry different Avro meanings.
        UINT virtualKey = 0;
        std::size_t candidateIndex = 0;
    };

    HRESULT activateInternal(ITfThreadMgr* threadManager, TfClientId clientId, DWORD flags);
    [[nodiscard]] KeyDecision classify(WPARAM key, LPARAM parameter) const noexcept;
    [[nodiscard]] static WCHAR characterFromKey(WPARAM key, LPARAM parameter) noexcept;
    [[nodiscard]] static bool isInputCharacter(WCHAR character) noexcept;

    HRESULT execute(const KeyDecision& decision, ITfContext* context, BOOL* eaten);
    HRESULT requestEdit(ITfContext* context, EditMode mode, const std::wstring& text,
                        const std::wstring& trailing = {}, std::size_t prefixLength = 0);
    HRESULT resolveContext(ITfContext* supplied, ITfContext** context) const noexcept;
    HRESULT startOrUpdate(ITfContext* context);
    HRESULT finishComposition(const std::wstring& trailing, bool forwardKey);
    HRESULT cancelComposition();
    HRESULT beginBanglaSuggestions(ITfContext* supplied);
    HRESULT finishBanglaSuggestion(const std::wstring& trailing);
    void clearBanglaSuggestions() noexcept;

    void setBanglaSuggestions(std::string prefix, ITfContext* context);
    void updateCandidates();
    void positionCandidates(TfEditCookie cookie, ITfContext* context);
    void clearCompositionState() noexcept;
    void abandonComposition() noexcept;
    void registerCompositionAttribute() noexcept;
    void clearCompositionAttributeRegistration() noexcept;
    void requestCompositionAttributeClear() noexcept;
    void onCandidateClicked(std::size_t index);
    void loadDictionary(HINSTANCE instance) noexcept;
    void loadUserSelections();
    bool saveUserSelections();
    static std::uint32_t currentDayStamp() noexcept;

    volatile LONG referenceCount_ = 1;
    ITfThreadMgr* threadManager_ = nullptr;
    ITfKeystrokeMgr* keystrokeManager_ = nullptr;
    ITfSource* source_ = nullptr;
    ITfContext* context_ = nullptr;
    ITfComposition* composition_ = nullptr;
    ITfContext* focusedContext_ = nullptr;
    ITfContext* suggestionContext_ = nullptr;
    ITfCategoryMgr* categoryManager_ = nullptr;
    TfGuidAtom compositionAttributeAtom_ = TF_INVALID_GUIDATOM;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD focusCookie_ = TF_INVALID_COOKIE;
    DWORD threadMgrCookie_ = TF_INVALID_COOKIE;
    bool keySinkAdvised_ = false;
    bool focusSinkAdvised_ = false;
    bool threadMgrSinkAdvised_ = false;
    bool active_ = false;
    bool composing_ = false;
    bool threadFocused_ = true;
    bool pendingDecisionValid_ = false;
    WPARAM pendingKey_ = 0;
    LPARAM pendingParameter_ = 0;
    KeyDecision pendingDecision_;
    bool banglaSuggestionActive_ = false;
    std::size_t banglaPrefixLength_ = 0;
    std::string banglaPrefix_;
    std::vector<bn::InputCandidate> banglaCandidates_;
    bn::InputEngine engine_;
    CandidateWindow candidateWindow_;
    std::wstring userSelectionsPath_;
    std::size_t unsavedSelections_ = 0;
    std::size_t savedSelectionCount_ = 0;
    // Last placement the host could describe for the composition. It is the
    // final fallback for hosts whose views cannot answer GetTextExt at all.
    int lastCandidateX_ = 0;
    int lastCandidateY_ = 0;
    bool hasCandidateAnchor_ = false;
};

} // namespace lekho::win
