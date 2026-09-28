#pragma once

#include <msctf.h>
#include <windows.h>

namespace lekho::win {

// The chosen composition style is a subtle dotted underline. The text and
// background colors stay with the application (TF_CT_NONE) so the composing
// text never clashes with the host theme, and the underline keeps a mid tone so
// it stays readable on both light and dark backgrounds.
constexpr TF_DA_LINESTYLE kCompositionLineStyle = TF_LS_DOT;
constexpr bool kCompositionBoldLine = false;
constexpr COLORREF kCompositionUnderlineColor = RGB(0x80, 0x80, 0x80);
constexpr wchar_t kCompositionAttributeDescription[] = L"Lekhoni composition";

// Every member of the published attribute is a compile-time constant, so the
// provider itself can stay stateless.
[[nodiscard]] TF_DISPLAYATTRIBUTE compositionDisplayAttribute() noexcept;

// Publishes the single Lekhoni composition attribute. The object doubles as
// its own ITfDisplayAttributeInfo because the description never changes.
class DisplayAttributeProvider final : public ITfDisplayAttributeProvider,
                                       public ITfDisplayAttributeInfo {
public:
    DisplayAttributeProvider();
    ~DisplayAttributeProvider();

    DisplayAttributeProvider(const DisplayAttributeProvider&) = delete;
    DisplayAttributeProvider& operator=(const DisplayAttributeProvider&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(
        IEnumTfDisplayAttributeInfo** enumerator) override;
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid,
                                                      ITfDisplayAttributeInfo** info) override;

    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE Reset() override;

private:
    volatile LONG referenceCount_ = 1;
};

} // namespace lekho::win
