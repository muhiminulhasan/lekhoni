#include "CandidateWindow.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace lekho::win {
namespace {

constexpr int kMaximumCandidates = 9;
constexpr int kItemHeight = 29;
constexpr int kHorizontalPadding = 22;

} // namespace

CandidateWindow::~CandidateWindow() {
    destroy();
}

bool CandidateWindow::create(HINSTANCE instance, SelectionCallback callback, HWND owner) {
    if (window_ != nullptr) {
        return true;
    }

    callback_ = std::move(callback);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    windowClass.lpfnWndProc = &CandidateWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kClassName;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    if (owner == nullptr) {
        owner = GetFocus();
    }
    if (owner == nullptr) {
        owner = GetActiveWindow();
    }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClassName, L"Lekhoni", WS_POPUP, 0, 0, width_, height_,
                              owner, nullptr, instance, this);
    if (window_ == nullptr) {
        return false;
    }

    font_ = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, nullptr);
    if (font_ == nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }
    return true;
}

void CandidateWindow::destroy() noexcept {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    candidates_.clear();
    selectedIndex_ = 0;
    visible_ = false;
}

void CandidateWindow::setCandidates(std::vector<std::wstring> candidates) {
    if (candidates.size() > kMaximumCandidates) {
        candidates.resize(kMaximumCandidates);
    }
    candidates_ = std::move(candidates);
    selectedIndex_ = 0;
    if (candidates_.empty()) {
        hide();
    } else {
        updateSize();
        invalidate();
    }
}

void CandidateWindow::clear() noexcept {
    candidates_.clear();
    selectedIndex_ = 0;
    hide();
}

void CandidateWindow::show(int x, int y) {
    if (window_ == nullptr || candidates_.size() < 2) {
        hide();
        return;
    }

    int monitorLeft = 0;
    int monitorTop = 0;
    int monitorRight = GetSystemMetrics(SM_CXSCREEN);
    int monitorBottom = GetSystemMetrics(SM_CYSCREEN);
    HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorLeft = monitorInfo.rcMonitor.left;
        monitorTop = monitorInfo.rcMonitor.top;
        monitorRight = monitorInfo.rcMonitor.right;
        monitorBottom = monitorInfo.rcMonitor.bottom;
    }

    int left = x;
    int top = y;
    if (left + width_ > monitorRight - 8) {
        left = (std::max)(monitorLeft, x - width_);
    }
    if (top + height_ > monitorBottom - 8) {
        top = (std::max)(monitorTop, y - height_ - kItemHeight);
    }
    left = (std::max)(monitorLeft, left);
    top = (std::max)(monitorTop, top);

    SetWindowPos(window_, HWND_TOP, left, top, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
}

void CandidateWindow::hide() noexcept {
    if (window_ != nullptr && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
}

void CandidateWindow::selectNext() noexcept {
    if (!candidates_.empty()) {
        selectedIndex_ = (selectedIndex_ + 1) % candidates_.size();
        invalidate();
    }
}

void CandidateWindow::selectPrevious() noexcept {
    if (!candidates_.empty()) {
        selectedIndex_ = (selectedIndex_ + candidates_.size() - 1) % candidates_.size();
        invalidate();
    }
}

void CandidateWindow::select(std::size_t index) noexcept {
    if (index < candidates_.size()) {
        selectedIndex_ = index;
        invalidate();
    }
}

const std::wstring& CandidateWindow::selected() const noexcept {
    static const std::wstring empty;
    return selectedIndex_ < candidates_.size() ? candidates_[selectedIndex_] : empty;
}

void CandidateWindow::invalidate() noexcept {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void CandidateWindow::updateSize() {
    int maximumWidth = 110;
    HDC deviceContext = GetDC(window_);
    HGDIOBJ oldFont = SelectObject(deviceContext, font_);
    for (const auto& candidate : candidates_) {
        SIZE extent{};
        GetTextExtentPoint32W(deviceContext, candidate.c_str(), static_cast<int>(candidate.size()),
                              &extent);
        maximumWidth = (std::max)(maximumWidth, static_cast<int>(extent.cx));
    }
    SelectObject(deviceContext, oldFont);
    ReleaseDC(window_, deviceContext);

    width_ = maximumWidth + kHorizontalPadding;
    height_ = static_cast<int>(candidates_.size()) * kItemHeight + 8;
}

void CandidateWindow::paint(HWND window) {
    PAINTSTRUCT paint{};
    HDC deviceContext = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(deviceContext, &client, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(205, 210, 218));
    HGDIOBJ oldPen = SelectObject(deviceContext, border);
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(NULL_BRUSH));
    Rectangle(deviceContext, client.left, client.top, client.right, client.bottom);
    SelectObject(deviceContext, oldPen);
    SelectObject(deviceContext, oldBrush);
    DeleteObject(border);

    HGDIOBJ oldFont = SelectObject(deviceContext, font_);
    SetBkMode(deviceContext, TRANSPARENT);
    int top = 4;
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        RECT item{4, top, client.right - 4, top + kItemHeight};
        const bool active = index == selectedIndex_;
        if (active) {
            HBRUSH selection = CreateSolidBrush(RGB(33, 93, 153));
            FillRect(deviceContext, &item, selection);
            DeleteObject(selection);
        }
        SetTextColor(deviceContext, active ? RGB(255, 255, 255) : RGB(28, 31, 36));
        RECT text{item.left + 7, item.top, item.right - 5, item.bottom};
        DrawTextW(deviceContext, candidates_[index].c_str(), -1, &text,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        top += kItemHeight;
    }
    SelectObject(deviceContext, oldFont);
    EndPaint(window, &paint);
}

LRESULT CALLBACK CandidateWindow::windowProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    CandidateWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CandidateWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->handleMessage(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CandidateWindow::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP: {
        const auto y = GET_Y_LPARAM(lParam);
        if (y >= 4 && y < height_) {
            const std::size_t index = static_cast<std::size_t>((y - 4) / kItemHeight);
            if (index < candidates_.size()) {
                select(index);
                if (callback_) {
                    try {
                        callback_(index);
                    } catch (...) {
                        // Candidate activation must not unwind through a window procedure.
                    }
                }
            }
        }
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        window_ = nullptr;
        visible_ = false;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace lekho::win
