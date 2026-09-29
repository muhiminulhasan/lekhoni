#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace lekho::win {

class CandidateWindow final {
public:
    using SelectionCallback = std::function<void(std::size_t)>;

    CandidateWindow() = default;
    ~CandidateWindow();

    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    bool create(HINSTANCE instance, SelectionCallback callback, HWND owner = nullptr);
    void destroy() noexcept;

    void setCandidates(std::vector<std::wstring> candidates);
    void clear() noexcept;
    void show(int x, int y);
    void hide() noexcept;

    void selectNext() noexcept;
    void selectPrevious() noexcept;
    void select(std::size_t index) noexcept;

    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    [[nodiscard]] std::size_t count() const noexcept { return candidates_.size(); }
    [[nodiscard]] std::size_t selectedIndex() const noexcept { return selectedIndex_; }
    [[nodiscard]] const std::wstring& selected() const noexcept;
    [[nodiscard]] const std::wstring& at(std::size_t index) const noexcept;

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void paint(HWND window);
    void updateSize();
    void invalidate() noexcept;

    static constexpr wchar_t kClassName[] = L"LekhoniCandidateWindow";

    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::wstring> candidates_;
    SelectionCallback callback_;
    std::size_t selectedIndex_ = 0;
    int width_ = 180;
    int height_ = 32;
    bool visible_ = false;
};

} // namespace lekho::win
