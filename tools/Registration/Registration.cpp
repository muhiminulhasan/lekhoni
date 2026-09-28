#include "Ids.h"

#include <windows.h>
#include <msctf.h>
#include <objbase.h>

#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

// Bengali (Bangladesh), not Xitsonga (0x0431).
constexpr LANGID kBanglaLanguageId = 0x0845;
constexpr wchar_t kDescription[] = L"Lekhoni Bangla Phonetic";
constexpr DWORD kInstallLayoutOrTipDisabled = 0x00000080;

using InstallLayoutOrTipFn = BOOL(CALLBACK*)(LPCWSTR, DWORD);

void printResult(const wchar_t* operation, HRESULT result) {
    if (SUCCEEDED(result)) {
        std::wcout << operation << L": success\n";
        return;
    }
    std::wcerr << operation << L": failure 0x" << std::hex << std::uppercase
               << static_cast<unsigned long>(static_cast<ULONG>(result)) << L'\n';
}

std::wstring guidString(REFGUID guid) {
    wchar_t buffer[64]{};
    if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0) {
        return {};
    }
    return buffer;
}

std::wstring profileList() {
    wchar_t language[16]{};
    if (swprintf_s(language, ARRAYSIZE(language), L"0x%04X",
                   static_cast<unsigned int>(kBanglaLanguageId)) < 0) {
        return {};
    }
    std::wstring result = language;
    result += L":";
    result += guidString(lekho::win::kTextServiceClsid);
    result += guidString(lekho::win::kLanguageProfileGuid);
    result += L";";
    return result;
}

bool writeComServer(const std::filesystem::path& dllPath) {
    const std::wstring keyPath = L"SOFTWARE\\Classes\\CLSID\\" +
                                 guidString(lekho::win::kTextServiceClsid) +
                                 L"\\InprocServer32";
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
        nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        std::wcerr << L"RegCreateKeyExW failed: " << status << L'\n';
        return false;
    }

    const std::wstring pathText = dllPath.c_str();
    LSTATUS writeStatus = RegSetValueExW(
        key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(pathText.c_str()),
        static_cast<DWORD>((pathText.size() + 1) * sizeof(wchar_t)));
    if (writeStatus == ERROR_SUCCESS) {
        const wchar_t threadingModel[] = L"Apartment";
        writeStatus = RegSetValueExW(
            key, L"ThreadingModel", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(threadingModel),
            static_cast<DWORD>(ARRAYSIZE(threadingModel) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    return writeStatus == ERROR_SUCCESS;
}

void deleteComServer() {
    const std::wstring keyPath = L"SOFTWARE\\Classes\\CLSID\\" +
                                 guidString(lekho::win::kTextServiceClsid);
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
        status != ERROR_PATH_NOT_FOUND) {
        std::wcerr << L"RegDeleteTreeW failed: " << status << L'\n';
    }
}

bool setCurrentUserLayout(DWORD flags) {
    const std::wstring list = profileList();
    // This helper runs elevated from a user-writable checkout, so an unqualified
    // LoadLibraryW would search the application directory first and could load
    // a planted input.dll with administrator rights. input.dll is not a
    // KnownDLL, so restrict the search to the system directory.
    HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (input == nullptr) {
        std::wcerr << L"input.dll could not be loaded; sign out and back in after install.\n";
        return false;
    }
    const auto install = reinterpret_cast<InstallLayoutOrTipFn>(
        reinterpret_cast<void*>(GetProcAddress(input, "InstallLayoutOrTip")));
    if (install == nullptr) {
        std::wcerr << L"InstallLayoutOrTip is unavailable; sign out and back in after install.\n";
        FreeLibrary(input);
        return false;
    }
    const BOOL result = install(list.c_str(), flags);
    FreeLibrary(input);
    if (result == FALSE) {
        std::wcerr << L"InstallLayoutOrTip failed; the profile can still be selected manually.\n";
        return false;
    }
    return true;
}

bool registerCategories() {
    ITfCategoryMgr* categories = nullptr;
    HRESULT result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITfCategoryMgr,
                                      reinterpret_cast<void**>(&categories));
    if (FAILED(result) || categories == nullptr) {
        printResult(L"Create category manager", result);
        return false;
    }

    result = categories->RegisterCategory(lekho::win::kTextServiceClsid,
                                          GUID_TFCAT_CATEGORY_OF_TIP,
                                          lekho::win::kTextServiceClsid);
    printResult(L"Register TIP category", result);
    const HRESULT keyboardResult = categories->RegisterCategory(
        lekho::win::kTextServiceClsid, GUID_TFCAT_TIP_KEYBOARD, lekho::win::kTextServiceClsid);
    printResult(L"Register keyboard category", keyboardResult);
    // Hosts only ask this text service for a composition underline when it is
    // registered as a display attribute provider.
    const HRESULT displayResult = categories->RegisterCategory(
        lekho::win::kTextServiceClsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
        lekho::win::kTextServiceClsid);
    printResult(L"Register display attribute provider category", displayResult);
    categories->Release();
    return SUCCEEDED(result) && SUCCEEDED(keyboardResult) && SUCCEEDED(displayResult);
}

bool unregisterCategories() {
    ITfCategoryMgr* categories = nullptr;
    HRESULT result = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITfCategoryMgr,
                                      reinterpret_cast<void**>(&categories));
    if (FAILED(result) || categories == nullptr) {
        printResult(L"Create category manager", result);
        return false;
    }
    const HRESULT category = categories->UnregisterCategory(
        lekho::win::kTextServiceClsid, GUID_TFCAT_CATEGORY_OF_TIP, lekho::win::kTextServiceClsid);
    const HRESULT keyboard = categories->UnregisterCategory(
        lekho::win::kTextServiceClsid, GUID_TFCAT_TIP_KEYBOARD, lekho::win::kTextServiceClsid);
    const HRESULT display = categories->UnregisterCategory(
        lekho::win::kTextServiceClsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
        lekho::win::kTextServiceClsid);
    categories->Release();
    const bool categoryOkay = SUCCEEDED(category) || category == REGDB_E_CLASSNOTREG;
    const bool keyboardOkay = SUCCEEDED(keyboard) || keyboard == REGDB_E_CLASSNOTREG;
    const bool displayOkay = SUCCEEDED(display) || display == REGDB_E_CLASSNOTREG;
    return categoryOkay && keyboardOkay && displayOkay;
}

bool registerProfile(const std::filesystem::path& dllPath) {
    const std::filesystem::path absolutePath = std::filesystem::absolute(dllPath);
    if (!std::filesystem::is_regular_file(absolutePath)) {
        std::wcerr << L"DLL does not exist: " << absolutePath << L'\n';
        return false;
    }
    if (!writeComServer(absolutePath)) {
        return false;
    }
    if (!registerCategories()) {
        unregisterCategories();
        deleteComServer();
        return false;
    }

    ITfInputProcessorProfileMgr* profiles = nullptr;
    HRESULT result = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                                      reinterpret_cast<void**>(&profiles));
    if (FAILED(result) || profiles == nullptr) {
        printResult(L"Create input processor profile manager", result);
        unregisterCategories();
        deleteComServer();
        return false;
    }

    const std::wstring pathText = absolutePath.wstring();
    result = profiles->RegisterProfile(
        lekho::win::kTextServiceClsid, kBanglaLanguageId, lekho::win::kLanguageProfileGuid,
        kDescription, static_cast<ULONG>(ARRAYSIZE(kDescription) - 1), pathText.c_str(),
        static_cast<ULONG>(pathText.size()), 1, nullptr, 0, TRUE, 0);
    printResult(L"Register TIP language profile", result);
    profiles->Release();
    if (FAILED(result)) {
        unregisterCategories();
        deleteComServer();
        return false;
    }

    // This is best effort. A profile can be selected manually if a host does
    // not expose the legacy input.dll entry point.
    setCurrentUserLayout(0);
    return true;
}

bool unregisterProfile() {
    setCurrentUserLayout(kInstallLayoutOrTipDisabled);

    ITfInputProcessorProfileMgr* profiles = nullptr;
    HRESULT result = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                                      reinterpret_cast<void**>(&profiles));
    bool success = true;
    if (SUCCEEDED(result) && profiles != nullptr) {
        const HRESULT removed = profiles->UnregisterProfile(
            lekho::win::kTextServiceClsid, kBanglaLanguageId, lekho::win::kLanguageProfileGuid, 0);
        printResult(L"Unregister TIP language profile", removed);
        success = SUCCEEDED(removed) || removed == REGDB_E_CLASSNOTREG;
        profiles->Release();
    } else {
        printResult(L"Create input processor profile manager", result);
        success = false;
    }

    if (!unregisterCategories()) {
        success = false;
    }
    deleteComServer();
    return success;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || (std::wstring_view(argv[1]) != L"register" &&
                      std::wstring_view(argv[1]) != L"unregister")) {
        std::wcerr << L"Usage: LekhoniRegistration <register|unregister> <LekhoniIME.dll>\n";
        return 2;
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        printResult(L"CoInitializeEx", initialized);
        return 3;
    }

    bool success = false;
    try {
        if (std::wstring_view(argv[1]) == L"register") {
            success = registerProfile(argv[2]);
        } else {
            success = unregisterProfile();
        }
    } catch (...) {
        std::wcerr << L"Registration failed because of an unexpected local error.\n";
        success = false;
    }
    CoUninitialize();
    return success ? 0 : 4;
}
