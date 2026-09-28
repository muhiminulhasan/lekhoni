#include "DisplayAttribute.h"
#include "Ids.h"
#include "WinText.h"

#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <oleauto.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using DllGetClassObjectFn = HRESULT(STDMETHODCALLTYPE*)(REFCLSID, REFIID, void**);

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void expectEqual(const std::wstring& actual, const std::wstring& expected,
                 const char* message) {
    if (actual == expected) {
        return;
    }
    const auto hex = [](const std::wstring& value) {
        std::string out;
        char buffer[16];
        for (const wchar_t unit : value) {
            std::snprintf(buffer, sizeof(buffer), "%04lX ", static_cast<unsigned long>(unit));
            out += buffer;
        }
        return out;
    };
    std::cerr << "FAIL: " << message << "\n  expected: [" << hex(expected)
              << "]\n  actual:   [" << hex(actual) << "]\n";
    std::exit(1);
}

// Regression cover for the text helpers. normalizeBangla previously passed its
// own destination buffer as the source, so it normalized NUL characters
// instead of the caller's text and silently returned a string of NULs.
void testTextHelpers() {
    // Non-Bangla text short-circuits and must come back byte for byte.
    expectEqual(lekho::win::normalizeBangla(L"plain ascii"), L"plain ascii",
                "ASCII text is returned unchanged");

    // U+0065 U+0301 composes canonically to U+00E9. The leading Bengali letter
    // keeps the string on the normalizing path.
    expectEqual(lekho::win::normalizeBangla(std::wstring(L"\x0995\x0065\x0301\x0996")),
                std::wstring(L"\x0995\x00E9\x0996"),
                "a decomposed sequence composes to NFC");

    // Bengali nukta letters are Unicode composition exclusions, so NFC must
    // leave them decomposed. This is why the core canonicalizer rewrites
    // U+09DC/U+09DD/U+09DF by hand instead of relying on normalization.
    expectEqual(lekho::win::normalizeBangla(std::wstring(L"\x0995\x09A1\x09BC\x0996")),
                std::wstring(L"\x0995\x09A1\x09BC\x0996"),
                "nukta sequences stay decomposed under NFC");

    // Already-normalized text must be returned unchanged and stay stable.
    const std::wstring composed = L"\x0995\x00E9\x0996";
    expectEqual(lekho::win::normalizeBangla(composed), composed,
                "already composed text is unchanged");
    expectEqual(lekho::win::normalizeBangla(lekho::win::normalizeBangla(composed)), composed,
                "normalization is idempotent");

    // The result must never contain the NUL characters the old bug produced.
    const std::wstring normalized =
        lekho::win::normalizeBangla(std::wstring(L"\x0995\x0065\x0301\x0996"));
    expect(normalized.find(L'\0') == std::wstring::npos,
           "normalized output contains no embedded NUL");
    expect(normalized.size() == 3 && normalized != std::wstring(3, L'\0'),
           "normalized output is the composed text, not a NUL run");

    // normalizeBanglaUtf8 is the path TextService actually calls.
    expect(lekho::win::normalizeBanglaUtf8("\xE0\xA6\x95\x65\xCC\x81\xE0\xA6\x96") ==
               "\xE0\xA6\x95\xC3\xA9\xE0\xA6\x96",
           "normalizeBanglaUtf8 composes a decomposed sequence");

    // Round trip through the wstring converters. The sample is সংলা.
    const char* sanglaUtf8 = "\xE0\xA6\xB8\xE0\xA6\x82\xE0\xA6\xAC\xE0\xA6\xBE";
    const std::wstring sanglaWide = L"\x09B8\x0982\x09AC\x09BE";
    expectEqual(lekho::win::toWide(sanglaUtf8), sanglaWide, "toWide decodes UTF-8");
    expect(lekho::win::toUtf8(sanglaWide) == sanglaUtf8, "toUtf8 encodes UTF-16");
    expect(lekho::win::toUtf8(L"\xD800").empty(), "a lone high surrogate is rejected");
    expect(lekho::win::toWide("").empty(), "empty input converts to an empty string");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    testTextHelpers();

    const auto executable = std::filesystem::absolute(argv[0]).parent_path();
    const auto dllPath = executable / L"LekhoniIME.dll";
    const HMODULE module = LoadLibraryW(dllPath.c_str());
    expect(module != nullptr, "load IME DLL");

    const auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
    expect(getClassObject != nullptr, "resolve DllGetClassObject");

    void* factoryObject = nullptr;
    HRESULT result = getClassObject(lekho::win::kTextServiceClsid, IID_IClassFactory,
                                    &factoryObject);
    expect(SUCCEEDED(result) && factoryObject != nullptr, "create class factory");

    auto* factory = static_cast<IClassFactory*>(factoryObject);
    void* textService = nullptr;
    result = factory->CreateInstance(nullptr, IID_ITfTextInputProcessor, &textService);
    expect(SUCCEEDED(result) && textService != nullptr, "create text input processor");

    void* extendedService = nullptr;
    result = static_cast<ITfTextInputProcessor*>(textService)->QueryInterface(
        IID_ITfTextInputProcessorEx, &extendedService);
    expect(SUCCEEDED(result) && extendedService != nullptr, "query extended text input processor");
    static_cast<IUnknown*>(extendedService)->Release();

    void* keySink = nullptr;
    result = static_cast<ITfTextInputProcessor*>(textService)->QueryInterface(
        IID_ITfKeyEventSink, &keySink);
    expect(SUCCEEDED(result) && keySink != nullptr, "query key event sink");
    static_cast<IUnknown*>(keySink)->Release();

    void* compositionSink = nullptr;
    result = static_cast<ITfTextInputProcessor*>(textService)->QueryInterface(
        IID_ITfCompositionSink, &compositionSink);
    expect(SUCCEEDED(result) && compositionSink != nullptr, "query composition sink");
    static_cast<IUnknown*>(compositionSink)->Release();

    void* threadEventSink = nullptr;
    result = static_cast<ITfTextInputProcessor*>(textService)->QueryInterface(
        IID_ITfThreadMgrEventSink, &threadEventSink);
    expect(SUCCEEDED(result) && threadEventSink != nullptr, "query thread manager event sink");
    static_cast<IUnknown*>(threadEventSink)->Release();

    void* focusSink = nullptr;
    result = static_cast<ITfTextInputProcessor*>(textService)->QueryInterface(
        IID_ITfThreadFocusSink, &focusSink);
    expect(SUCCEEDED(result) && focusSink != nullptr, "query thread focus sink");
    static_cast<IUnknown*>(focusSink)->Release();

    // The composition style is published through the same class factory.
    void* providerObject = nullptr;
    result = factory->CreateInstance(nullptr, IID_ITfDisplayAttributeProvider, &providerObject);
    expect(SUCCEEDED(result) && providerObject != nullptr, "create display attribute provider");

    auto* provider = static_cast<ITfDisplayAttributeProvider*>(providerObject);
    void* providerUnknown = nullptr;
    result = provider->QueryInterface(IID_IUnknown, &providerUnknown);
    expect(SUCCEEDED(result) && providerUnknown != nullptr, "query display attribute provider");
    static_cast<IUnknown*>(providerUnknown)->Release();

    void* unknownAttribute = nullptr;
    result = provider->GetDisplayAttributeInfo(IID_IUnknown,
                                                reinterpret_cast<ITfDisplayAttributeInfo**>(
                                                    &unknownAttribute));
    expect(FAILED(result) && unknownAttribute == nullptr, "reject an unknown display attribute");

    ITfDisplayAttributeInfo* info = nullptr;
    result = provider->GetDisplayAttributeInfo(lekho::win::kCompositionDisplayAttributeGuid, &info);
    expect(SUCCEEDED(result) && info != nullptr, "get composition display attribute info");

    GUID guid{};
    expect(SUCCEEDED(info->GetGUID(&guid)) && guid == lekho::win::kCompositionDisplayAttributeGuid,
           "display attribute identifier round trip");

    TF_DISPLAYATTRIBUTE attribute{};
    expect(SUCCEEDED(info->GetAttributeInfo(&attribute)), "read display attribute");
    expect(attribute.crText.type == TF_CT_NONE, "composition keeps the application text color");
    expect(attribute.crBk.type == TF_CT_NONE, "composition keeps the application background");
    expect(attribute.lsStyle == lekho::win::kCompositionLineStyle,
           "composition uses the subtle underline");
    expect(attribute.fBoldLine == FALSE, "composition underline is not bold");
    expect(attribute.crLine.type == TF_CT_COLORREF &&
               attribute.crLine.cr == lekho::win::kCompositionUnderlineColor,
           "composition underline color is set");
    expect(FAILED(info->SetAttributeInfo(&attribute)),
           "composition display attribute is not editable");

    BSTR description = nullptr;
    result = info->GetDescription(&description);
    expect(SUCCEEDED(result) && description != nullptr, "read display attribute description");
    SysFreeString(description);

    IEnumTfDisplayAttributeInfo* enumerator = nullptr;
    expect(SUCCEEDED(provider->EnumDisplayAttributeInfo(&enumerator)) && enumerator != nullptr,
           "enum display attribute info");

    ITfDisplayAttributeInfo* items[2]{};
    ULONG fetched = 0;
    expect(SUCCEEDED(enumerator->Next(ARRAYSIZE(items), items, &fetched)) && fetched == 1 &&
               items[0] != nullptr,
           "enumerator returns the single composition attribute");
    items[0]->Release();
    items[0] = nullptr;
    expect(enumerator->Next(1, items, &fetched) == S_FALSE && fetched == 0,
           "enumerator reports the end");
    expect(SUCCEEDED(enumerator->Reset()) && SUCCEEDED(enumerator->Next(1, items, &fetched)) &&
               fetched == 1 && items[0] != nullptr,
           "enumerator can be reset");
    if (items[0] != nullptr) {
        items[0]->Release();
    }
    IEnumTfDisplayAttributeInfo* clone = nullptr;
    expect(SUCCEEDED(enumerator->Clone(&clone)) && clone != nullptr, "clone display enumerator");
    clone->Release();
    enumerator->Release();

    static_cast<IUnknown*>(info)->Release();
    static_cast<IUnknown*>(provider)->Release();
    static_cast<IUnknown*>(textService)->Release();
    factory->Release();
    FreeLibrary(module);
    std::cout << "IME COM smoke tests passed.\n";
    return 0;
}