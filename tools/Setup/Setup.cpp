// LekhoniSetup — native installer for the Lekhoni Bangla IME.
//
// It performs exactly what install.ps1 and uninstall.ps1 perform — same
// destination layout, same registration helpers, same rollback behavior — so
// the two installation paths stay interchangeable: install with either, then
// uninstall with either. The PowerShell scripts remain the scriptable
// interface; this executable is the double-clickable one for release
// packages.
//
// Usage:
//   LekhoniSetup.exe              install (elevates itself through UAC)
//   LekhoniSetup.exe --uninstall  remove the installation (elevates itself)
//   LekhoniSetup.exe --dry-run    print the planned actions, change nothing
//   LekhoniSetup.exe --help       usage
//
// The payload is resolved relative to this executable, in either layout:
//   package:  <exeDir>\bin\Win32, <exeDir>\bin\x64
//   source:   <ancestor>\Win32\Release, <ancestor>\x64\Release
// (the source form also covers the repository's bin\<arch> ZIP layout).

#include "Ids.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kInstallDirName[] = L"Lekhoni";
constexpr wchar_t kDisplayName[] = L"Lekhoni Bangla Phonetic IME";
constexpr wchar_t kPublisher[] = L"Lekhoni project";
// Bump together with the release version recorded in RELEASE_CHECKLIST.md.
constexpr wchar_t kVersion[] = L"0.1.0";
constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Lekhoni";

// --- small utilities --------------------------------------------------------

bool is64BitWindows() {
#ifdef _WIN64
    return true;
#else
    BOOL wow64 = FALSE;
    return IsWow64Process(GetCurrentProcess(), &wow64) && wow64;
#endif
}

bool isElevated() {
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&ntAuthority, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &administrators)) {
        return false;
    }
    BOOL member = FALSE;
    const BOOL ok = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    return ok && member;
}

fs::path ownPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    if (length == 0 || length == ARRAYSIZE(buffer)) {
        return {};
    }
    return fs::path(buffer);
}

fs::path programFiles() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path)) || !path) {
        return {};
    }
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::vector<fs::path> ancestors(const fs::path& start, int depth) {
    std::vector<fs::path> result;
    fs::path current = start;
    for (int i = 0; i < depth && current.has_parent_path(); ++i) {
        result.push_back(current);
        current = current.parent_path();
    }
    return result;
}

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// The console created for a double-clicked console application is owned by
// the single attached process, so a process list of one means Explorer (or a
// shortcut) launched us: keep the window open so the summary is readable.
void pauseIfDoubleClicked() {
    DWORD processes[2]{};
    const DWORD count = GetConsoleProcessList(processes, 2);
    if (count != 1) {
        return;
    }
    std::wcout << L"\nPress Enter to close this window... ";
    std::wstring ignored;
    std::getline(std::wcin, ignored);
}

// --- payload resolution -----------------------------------------------------

// Directory holding LekhoniIME.dll and LekhoniRegistration.exe for one
// architecture, in either the package or the source-tree layout.
std::optional<fs::path> findArchPayload(const fs::path& exeDir, const wchar_t* arch) {
    std::vector<fs::path> candidates;
    candidates.push_back(exeDir / L"bin" / arch);
    for (const fs::path& ancestor : ancestors(exeDir, 4)) {
        candidates.push_back(ancestor / L"bin" / arch);
        candidates.push_back(ancestor / arch / L"Release");
    }
    for (const fs::path& candidate : candidates) {
        if (fileExists(candidate / L"LekhoniIME.dll") &&
            fileExists(candidate / L"LekhoniRegistration.exe")) {
            return candidate;
        }
    }
    return std::nullopt;
}

// Data images live beside the build outputs: in the artifacts root for a
// source layout, in bin\<arch> for a package. This mirrors the candidate
// order install.ps1 uses.
std::optional<fs::path> findDataFile(const fs::path& exeDir,
                                     const std::vector<fs::path>& payloadDirs,
                                     const wchar_t* name) {
    for (const fs::path& ancestor : ancestors(exeDir, 4)) {
        if (fileExists(ancestor / name)) {
            return ancestor / name;
        }
    }
    for (const fs::path& dir : payloadDirs) {
        if (fileExists(dir / name)) {
            return dir / name;
        }
    }
    return std::nullopt;
}

// --- install ----------------------------------------------------------------

bool copyFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    const bool ok = fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ok || ec) {
        std::wcerr << L"copy failed: " << from.wstring() << L" -> " << to.wstring()
                   << L" (" << ec.message().c_str() << L")\n";
        return false;
    }
    return true;
}

bool copyFileOrPlan(const fs::path& from, const fs::path& to, bool dryRun) {
    if (dryRun) {
        std::wcout << L"  copy " << from.wstring() << L"\n    -> " << to.wstring() << L'\n';
        return true;
    }
    return copyFile(from, to);
}

bool runProcess(const fs::path& exe, const std::wstring& arguments) {
    std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) {
        std::wcerr << L"could not start: " << exe.wstring() << L'\n';
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        std::wcerr << L"failed (exit code " << exitCode << L"): " << exe.wstring() << L'\n';
        return false;
    }
    return true;
}

// The optional images must not linger from an earlier install: a stale
// layout table or phrase list would keep steering the IME after the payload
// stopped shipping it.
void syncOptionalFile(const std::optional<fs::path>& source, const fs::path& destination,
                      const wchar_t* label, bool dryRun) {
    if (source) {
        std::wcout << L"  " << label << L": " << source->wstring() << L'\n';
        if (!dryRun && !copyFile(*source, destination)) {
            throw std::runtime_error("optional file copy failed");
        }
    } else {
        std::wcout << L"  " << label << L": not present; removing any stale copy\n";
        if (!dryRun) {
            DeleteFileW(destination.c_str());
        }
    }
}

// Installs one architecture: files first, registration last, exactly like
// install.ps1, so a file failure leaves nothing registered.
void installArch(const fs::path& payload, const fs::path& bin,
                 const fs::path& dictionary, const std::optional<fs::path>& phrases,
                 const std::optional<fs::path>& layout,
                 const std::optional<fs::path>& trigrams,
                 const wchar_t* label, bool dryRun) {
    std::wcout << L"Installing " << label << L" from " << payload.wstring() << L'\n';
    if (!copyFileOrPlan(payload / L"LekhoniIME.dll", bin / L"LekhoniIME.dll", dryRun) ||
        !copyFileOrPlan(dictionary, bin / L"LekhoniCore.bgk", dryRun) ||
        !copyFileOrPlan(payload / L"LekhoniRegistration.exe",
                        bin / L"LekhoniRegistration.exe", dryRun)) {
        throw std::runtime_error("required file copy failed");
    }
    // The registration helper is installed beside the DLL so a later
    // uninstall needs nothing from the original package.
    syncOptionalFile(phrases, bin / L"LekhoniPhrases.bgk", L"phrase layer", dryRun);
    syncOptionalFile(layout, bin / L"AvroLayout.dat", L"layout table", dryRun);
    syncOptionalFile(trigrams, bin / L"LekhoniTrigrams.dat", L"trigram table", dryRun);
    std::wcout << L"  registering (TSF)\n";
    if (!dryRun) {
        const std::wstring arguments =
            L"register \"" + (bin / L"LekhoniIME.dll").wstring() + L"\"";
        if (!runProcess(payload / L"LekhoniRegistration.exe", arguments)) {
            throw std::runtime_error("TSF registration failed");
        }
    }
}

void writeUninstallEntry(const fs::path& installRoot, const fs::path& installedSetup) {
    const std::wstring uninstallCommand =
        L"\"" + installedSetup.wstring() + L"\" --uninstall";
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr,
                                           REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                           &key, nullptr);
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("could not create the uninstall registry entry");
    }
    const DWORD trueValue = 1;
    struct Value { const wchar_t* name; const std::wstring& data; };
    const Value values[] = {
        {L"DisplayName", std::wstring(kDisplayName)},
        {L"Publisher", std::wstring(kPublisher)},
        {L"DisplayVersion", std::wstring(kVersion)},
        {L"InstallLocation", installRoot.wstring()},
        {L"UninstallString", uninstallCommand},
    };
    for (const Value& value : values) {
        RegSetValueExW(key, value.name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.data.c_str()),
                       static_cast<DWORD>((value.data.size() + 1) * sizeof(wchar_t)));
    }
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&trueValue), sizeof(DWORD));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&trueValue), sizeof(DWORD));
    RegCloseKey(key);
}

int doInstall(bool dryRun) {
    const fs::path exeDir = ownPath().parent_path();
    const auto payload32 = findArchPayload(exeDir, L"Win32");
    const auto payload64 = findArchPayload(exeDir, L"x64");
    if (!payload32) {
        std::wcerr << L"No 32-bit payload (bin\\Win32 or Win32\\Release with "
                      L"LekhoniIME.dll) found beside LekhoniSetup.exe.\n";
        return 1;
    }
    if (is64BitWindows() && !payload64) {
        std::wcerr << L"No 64-bit payload (bin\\x64 or x64\\Release with "
                      L"LekhoniIME.dll) found beside LekhoniSetup.exe.\n";
        return 1;
    }

    std::vector<fs::path> payloadDirs{*payload32};
    if (payload64) {
        payloadDirs.push_back(*payload64);
    }
    const auto dictionary = findDataFile(exeDir, payloadDirs, L"LekhoniCore.bgk");
    if (!dictionary) {
        std::wcerr << L"No core dictionary (LekhoniCore.bgk) found in the payload.\n";
        return 1;
    }
    const auto phrases = findDataFile(exeDir, payloadDirs, L"LekhoniPhrases.bgk");
    const auto layout = findDataFile(exeDir, payloadDirs, L"AvroLayout.dat");
    const auto trigrams = findDataFile(exeDir, payloadDirs, L"LekhoniTrigrams.dat");

    const fs::path installRoot = programFiles() / kInstallDirName;
    if (installRoot.empty()) {
        std::wcerr << L"Could not determine the Program Files directory.\n";
        return 1;
    }
    std::wcout << L"Destination: " << installRoot.wstring() << L'\n';
    std::wcout << L"Dictionary: " << dictionary->wstring() << L'\n';

    try {
        installArch(*payload32, installRoot / L"bin32", *dictionary,
                    phrases, layout, trigrams, L"32-bit IME", dryRun);
        if (payload64 && is64BitWindows()) {
            installArch(*payload64, installRoot / L"bin64", *dictionary,
                        phrases, layout, trigrams, L"64-bit IME", dryRun);
        }
    } catch (const std::exception& error) {
        // install.ps1 rolls the 64-bit registration back but leaves the
        // 32-bit one in place when the second registration fails; the file
        // copies above are already harmless without a registration.
        std::wcerr << L"Installation failed: " << error.what() << L'\n';
        if (!dryRun && payload64 && is64BitWindows()) {
            const fs::path helper = *payload64 / L"LekhoniRegistration.exe";
            const std::wstring arguments =
                L"unregister \"" +
                (installRoot / L"bin64" / L"LekhoniIME.dll").wstring() + L"\"";
            std::wcout << L"Rolling back the 64-bit registration.\n";
            runProcess(helper, arguments);
        }
        return 1;
    }

    if (dryRun) {
        std::wcout << L"Dry run complete; nothing was installed.\n";
        return 0;
    }

    // Copy itself into the installation so Add/Remove Programs can invoke
    // the uninstaller without the original package.
    const fs::path installedSetup = installRoot / L"LekhoniSetup.exe";
    if (!copyFile(ownPath(), installedSetup)) {
        return 1;
    }
    writeUninstallEntry(installRoot, installedSetup);

    std::wcout << L"Lekhoni installed. Use Win+Space to select Lekhoni Bangla "
                  L"Phonetic.\nIf it is not listed immediately, sign out and back in "
                  L"once.\n";
    std::wcout << L"Uninstall from Settings > Apps, or run: " << installedSetup.wstring()
               << L" --uninstall\n";
    return 0;
}

// --- uninstall --------------------------------------------------------------

std::wstring guidString(REFGUID guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, ARRAYSIZE(buffer));
    return buffer;
}

// The 32-bit COM view lives under Wow6432Node; a 64-bit process reaches it
// with KEY_WOW64_32KEY, which is what uninstall.ps1 spells out as the
// Wow6432Node path.
bool comKeyRemains(REGSAM access) {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
         guidString(lekho::win::kTextServiceClsid)).c_str(),
        0, KEY_QUERY_VALUE | access, &key);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

int doUninstall(bool dryRun) {
    const fs::path exeDir = ownPath().parent_path();
    const fs::path installRoot = programFiles() / kInstallDirName;
    std::wcout << L"Removing " << installRoot.wstring() << L'\n';

    // Prefer the helper installed beside the IME; fall back to a payload
    // beside this executable, in either layout (covers installations made
    // by install.ps1, which does not copy the helper into the install
    // directory).
    const auto helperFor = [&](const wchar_t* subdir,
                               const wchar_t* arch) -> std::optional<fs::path> {
        const fs::path installed = installRoot / subdir / L"LekhoniRegistration.exe";
        if (fileExists(installed)) {
            return installed;
        }
        const auto payload = findArchPayload(exeDir, arch);
        if (payload) {
            return *payload / L"LekhoniRegistration.exe";
        }
        return std::nullopt;
    };

    struct Arch { const wchar_t* subdir; const wchar_t* arch; const wchar_t* label; };
    const Arch architectures[] = {
        {L"bin32", L"Win32", L"32-bit"}, {L"bin64", L"x64", L"64-bit"}};
    for (const Arch& arch : architectures) {
        const fs::path dll = installRoot / arch.subdir / L"LekhoniIME.dll";
        if (!fileExists(dll)) {
            continue;
        }
        const auto helper = helperFor(arch.subdir, arch.arch);
        if (!helper) {
            std::wcerr << L"No registration helper for the " << arch.label
                       << L" IME; refusing to delete an installed IME blindly.\n";
            return 1;
        }
        std::wcout << L"Unregistering the " << arch.label << L" IME (TSF).\n";
        if (!dryRun) {
            const std::wstring arguments = L"unregister \"" + dll.wstring() + L"\"";
            if (!runProcess(*helper, arguments)) {
                return 1;
            }
        }
    }

    // This guard belongs after a real unregistration: in a dry run the keys
    // are of course still present, so the check is skipped and the deletion
    // is only planned.
    if (!dryRun && (comKeyRemains(0) ||
                    (is64BitWindows() && comKeyRemains(KEY_WOW64_32KEY)))) {
        std::wcerr << L"A COM registration key remains; files were not removed.\n";
        return 1;
    }

    if (dryRun) {
        std::wcout << L"  would delete " << installRoot.wstring() << L'\n';
        std::wcout << L"Dry run complete; nothing was removed.\n";
        return 0;
    }

    // Refuse to delete anything that is not under Program Files, mirroring
    // the guard in uninstall.ps1.
    const std::wstring rootText = installRoot.wstring();
    const std::wstring parentText = programFiles().wstring();
    const bool underProgramFiles =
        rootText.size() > parentText.size() &&
        _wcsnicmp(rootText.c_str(), parentText.c_str(), parentText.size()) == 0 &&
        (rootText[parentText.size()] == L'\\' || rootText[parentText.size()] == L'/');
    std::error_code ec;
    if (!underProgramFiles || !fs::is_directory(installRoot, ec)) {
        std::wcerr << L"Installation directory is missing or unexpected; nothing "
                      L"was deleted.\n";
        return 1;
    }

    // Everything except a running copy of this executable deletes normally;
    // the running copy (Add/Remove Programs launches it from the install
    // directory) is scheduled for deletion on the next reboot, and an empty
    // parent directory may remain until then.
    for (const auto& entry : fs::recursive_directory_iterator(installRoot, ec)) {
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc) &&
            _wcsicmp(entry.path().c_str(), ownPath().c_str()) != 0) {
            DeleteFileW(entry.path().c_str());
        }
    }
    MoveFileExW(ownPath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    for (const auto& entry : fs::directory_iterator(installRoot, ec)) {
        std::error_code removeEc;
        fs::remove_all(entry.path(), removeEc);
    }
    fs::remove(installRoot, ec);

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    std::wcout << L"Lekhoni unregistered and removed. Sign out and back in to "
                  L"refresh the language list.\n";
    return 0;
}

// --- entry point ------------------------------------------------------------

int relaunchElevated(const std::wstring& arguments) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = ownPath().c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        std::wcerr << L"Elevation was declined or failed.\n";
        return 1;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return static_cast<int>(exitCode);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool uninstall = false;
    bool dryRun = false;
    std::wstring arguments;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument = argv[i];
        if (argument == L"--uninstall") {
            uninstall = true;
            arguments += L"--uninstall";
        } else if (argument == L"--dry-run") {
            dryRun = true;
            arguments += L"--dry-run";
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            std::wcout << L"Usage: LekhoniSetup [ --uninstall ] [ --dry-run ]\n"
                          L"  (no arguments)  install Lekhoni (elevates itself)\n"
                          L"  --uninstall    remove the installation\n"
                          L"  --dry-run      print the planned actions only (combine with\n"
                          L"                 --uninstall to preview removal)\n";
            pauseIfDoubleClicked();
            return 0;
        } else {
            std::wcerr << L"Unknown argument: " << argument << L"\n"
                       << L"Usage: LekhoniSetup [ --uninstall ] [ --dry-run ]\n";
            pauseIfDoubleClicked();
            return 1;
        }
    }

    // --dry-run only reads, so it runs at the current elevation; the modes
    // that change the machine elevate themselves, which also works when
    // Add/Remove Programs launches the uninstaller.
    int exitCode = 0;
    if (!dryRun && !isElevated()) {
        exitCode = relaunchElevated(arguments);
    } else if (uninstall) {
        exitCode = doUninstall(dryRun);
    } else {
        exitCode = doInstall(dryRun);
    }
    pauseIfDoubleClicked();
    return exitCode;
}
