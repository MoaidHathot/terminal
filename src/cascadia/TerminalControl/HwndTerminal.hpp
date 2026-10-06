// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../../buffer/out/textBuffer.hpp"
#include "../../renderer/inc/FontInfoDesired.hpp"
#include "../../types/IControlAccessibilityInfo.h"
#include "../../tsf/Handle.h"

namespace Microsoft::Console::Render::Atlas
{
    class AtlasEngine;
}

namespace Microsoft::Console::Render
{
    using AtlasEngine = Atlas::AtlasEngine;
    class IRenderData;
    class Renderer;
    class RenderSettings;
    class UiaEngine;
}

namespace Microsoft::Terminal::Core
{
    class Terminal;
}

class FontInfo;
class FontInfoDesired;
class HwndTerminalAutomationPeer;
struct IDCompositionDesktopDevice;
struct IDCompositionTarget;
struct IDCompositionVisual2;

// Flags for CreateTerminalEx. Keep in sync with NativeMethods.cs.
// TERMINAL_CREATE_COMPOSED: the terminal renders into a DirectComposition visual that this library
// places on the host's top-level window over the child HWND, instead of into the child HWND itself.
// A swap chain presented that way keeps its alpha channel, so TerminalSetBackgroundOpacity can let
// whatever is behind the window - a system backdrop, say - show through the terminal's background.
// The child HWND still exists for input, focus, text services and UI Automation; it just has no
// pixels of its own (WS_EX_NOREDIRECTIONBITMAP).
constexpr uint32_t TERMINAL_CREATE_COMPOSED = 0x1;

// Keep in sync with TerminalTheme.cs
typedef struct _TerminalTheme
{
    COLORREF DefaultBackground;
    COLORREF DefaultForeground;
    COLORREF DefaultSelectionBackground;
    uint32_t CursorStyle; // This will be converted to DispatchTypes::CursorStyle (size_t), but C# cannot marshal an enum type and have it fit in a size_t.
    COLORREF ColorTable[16];
} TerminalTheme, *LPTerminalTheme;

extern "C" {
__declspec(dllexport) void _stdcall AvoidBuggyTSFConsoleFlags();
__declspec(dllexport) HRESULT _stdcall CreateTerminal(HWND parentHwnd, _Out_ void** hwnd, _Out_ void** terminal);
__declspec(dllexport) HRESULT _stdcall CreateTerminalEx(HWND parentHwnd, uint32_t flags, _Out_ void** hwnd, _Out_ void** terminal);
__declspec(dllexport) void _stdcall TerminalSendOutput(void* terminal, LPCWSTR data);
__declspec(dllexport) void _stdcall TerminalRegisterScrollCallback(void* terminal, void __stdcall callback(int, int, int));
__declspec(dllexport) HRESULT _stdcall TerminalTriggerResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions);
__declspec(dllexport) HRESULT _stdcall TerminalTriggerResizeWithDimension(_In_ void* terminal, _In_ til::size dimensions, _Out_ til::size* dimensionsInPixels);
__declspec(dllexport) HRESULT _stdcall TerminalCalculateResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions);
__declspec(dllexport) void _stdcall TerminalDpiChanged(void* terminal, int newDpi);
__declspec(dllexport) void _stdcall TerminalUserScroll(void* terminal, int viewTop);
__declspec(dllexport) const wchar_t* _stdcall TerminalGetSelection(void* terminal);
__declspec(dllexport) bool _stdcall TerminalIsSelectionActive(void* terminal);
__declspec(dllexport) void _stdcall DestroyTerminal(void* terminal);
__declspec(dllexport) void _stdcall TerminalSetTheme(void* terminal, TerminalTheme theme, LPCWSTR fontFamily, til::CoordType fontSize, int newDpi);
__declspec(dllexport) void _stdcall TerminalRegisterWriteCallback(void* terminal, const void __stdcall callback(wchar_t*));
__declspec(dllexport) void _stdcall TerminalSendKeyEvent(void* terminal, WORD vkey, WORD scanCode, WORD flags, bool keyDown);
__declspec(dllexport) void _stdcall TerminalSendCharEvent(void* terminal, wchar_t ch, WORD flags, WORD scanCode);
__declspec(dllexport) void _stdcall TerminalSetFocused(void* terminal, bool focused);
// Composed terminals only (TERMINAL_CREATE_COMPOSED); no-ops otherwise.
// The opacity (0..1) of cells that have the default background colour; text and coloured cells stay opaque.
__declspec(dllexport) void _stdcall TerminalSetBackgroundOpacity(void* terminal, float opacity);
// Re-reads the child HWND's root window, position and visibility and moves the visual accordingly. The
// library does this itself on WM_WINDOWPOSCHANGED; a host that re-parents the child to another top-level
// window (SetParent) calls this afterwards.
__declspec(dllexport) void _stdcall TerminalUpdateComposition(void* terminal);
};

struct HwndTerminal : ::Microsoft::Console::Types::IControlAccessibilityInfo
{
public:
    HwndTerminal(HWND hwnd) noexcept;
    HwndTerminal(HWND hwnd, uint32_t flags) noexcept;

    // One DirectComposition target per top-level window, shared by the composed terminals in it (see
    // TERMINAL_CREATE_COMPOSED). Public only so the file-local helpers in HwndTerminal.cpp can name it.
    struct CompositionTarget;

    HwndTerminal(const HwndTerminal&) = default;
    HwndTerminal(HwndTerminal&&) = default;
    HwndTerminal& operator=(const HwndTerminal&) = default;
    HwndTerminal& operator=(HwndTerminal&&) = default;
    ~HwndTerminal();

    HRESULT Initialize();
    void Teardown() noexcept;
    void SendOutput(std::wstring_view data);
    HRESULT Refresh(const til::size windowSize, _Out_ til::size* dimensions);
    void RegisterScrollCallback(std::function<void(int, int, int)> callback);
    void RegisterWriteCallback(const void _stdcall callback(wchar_t*));
    ::Microsoft::Console::Render::IRenderData* GetRenderData() const noexcept;
    HWND GetHwnd() const noexcept;

    static LRESULT CALLBACK HwndTerminalWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept;

private:
    struct TsfDataProvider : public Microsoft::Console::TSF::IDataProvider
    {
        TsfDataProvider(HwndTerminal* t) :
            _terminal(t) {}
        virtual ~TsfDataProvider() = default;
        STDMETHODIMP QueryInterface(REFIID, void**) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;
        HWND GetHwnd() override;
        RECT GetViewport() override;
        RECT GetCursorPosition() override;
        void HandleOutput(std::wstring_view text) override;
        Microsoft::Console::Render::Renderer* GetRenderer() override;
        HwndTerminal* _terminal;
    };
    wil::unique_hwnd _hwnd;
    FontInfoDesired _desiredFont;
    FontInfo _actualFont;
    int _currentDpi;
    std::function<void(wchar_t*)> _pfnWriteCallback;
    ::Microsoft::WRL::ComPtr<HwndTerminalAutomationPeer> _uiaProvider;

    std::unique_ptr<::Microsoft::Terminal::Core::Terminal> _terminal;

    std::unique_ptr<::Microsoft::Console::Render::Renderer> _renderer;
    std::unique_ptr<::Microsoft::Console::Render::AtlasEngine> _renderEngine;
    std::unique_ptr<::Microsoft::Console::Render::UiaEngine> _uiaEngine;

    bool _focused{ false };

    std::chrono::milliseconds _multiClickTime;
    unsigned int _multiClickCounter{};
    std::chrono::steady_clock::time_point _lastMouseClickTimestamp{};
    std::optional<til::point> _lastMouseClickPos;
    std::optional<til::point> _singleClickTouchdownPos;

    // _tsfHandle uses _tsfDataProvider. Destructors run from bottom to top; this maintains correct destruction order.
    TsfDataProvider _tsfDataProvider{ this };
    Microsoft::Console::TSF::Handle _tsfHandle;

    // Composed rendering (TERMINAL_CREATE_COMPOSED). The engine renders into a composition surface
    // (no HWND swap chain); this library wraps it in a DirectComposition visual on the child's
    // top-level window, kept at the child's position and size. One composition device per process,
    // one target per top-level window, shared by every composed terminal in it.
    bool _composed{ false };
    float _backgroundOpacity{ 1.0f };
    std::shared_ptr<CompositionTarget> _compositionTarget;
    wil::com_ptr<IDCompositionVisual2> _compositionVisual;
    bool _compositionVisualAttached{ false };
    wil::unique_handle _swapChainHandle;
    std::mutex _pendingSwapChainLock;
    wil::unique_handle _pendingSwapChainHandle;

    friend HRESULT _stdcall CreateTerminal(HWND parentHwnd, _Out_ void** hwnd, _Out_ void** terminal);
    friend HRESULT _stdcall CreateTerminalEx(HWND parentHwnd, uint32_t flags, _Out_ void** hwnd, _Out_ void** terminal);
    friend void _stdcall TerminalSetBackgroundOpacity(void* terminal, float opacity);
    friend void _stdcall TerminalUpdateComposition(void* terminal);
    friend HRESULT _stdcall TerminalTriggerResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions);
    friend HRESULT _stdcall TerminalTriggerResizeWithDimension(_In_ void* terminal, _In_ til::size dimensions, _Out_ til::size* dimensionsInPixels);
    friend HRESULT _stdcall TerminalCalculateResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions);
    friend void _stdcall TerminalDpiChanged(void* terminal, int newDpi);
    friend void _stdcall TerminalUserScroll(void* terminal, int viewTop);
    friend const wchar_t* _stdcall TerminalGetSelection(void* terminal);
    friend bool _stdcall TerminalIsSelectionActive(void* terminal);
    friend void _stdcall TerminalSendKeyEvent(void* terminal, WORD vkey, WORD scanCode, WORD flags, bool keyDown);
    friend void _stdcall TerminalSendCharEvent(void* terminal, wchar_t ch, WORD scanCode, WORD flags);
    friend void _stdcall TerminalSetTheme(void* terminal, TerminalTheme theme, LPCWSTR fontFamily, til::CoordType fontSize, int newDpi);
    friend void _stdcall TerminalSetFocused(void* terminal, bool focused);

    void _UpdateFont(int newDpi);
    void _WriteTextToConnection(const std::wstring_view text) noexcept;
    HRESULT _CopyTextToSystemClipboard(wil::zwstring_view text, wil::zstring_view htmlData, wil::zstring_view rtfData) const;
    HRESULT _CopyToSystemClipboard(wil::zstring_view stringToCopy, LPCWSTR lpszFormat) const;
    void _PasteTextFromClipboard() noexcept;

    void _setFocused(bool focused) noexcept;

    void _ApplyBackgroundOpacity(::Microsoft::Console::Render::RenderSettings& renderSettings) noexcept;
    void _OnSwapChainChanged(HANDLE handle) noexcept;
    void _ApplyPendingSwapChain() noexcept;
    void _UpdateComposition() noexcept;
    void _DetachComposition() noexcept;

    const unsigned int _NumberOfClicks(til::point clickPos, std::chrono::steady_clock::time_point clickTime) noexcept;
    HRESULT _StartSelection(LPARAM lParam) noexcept;
    HRESULT _MoveSelection(LPARAM lParam) noexcept;
    IRawElementProviderSimple* _GetUiaProvider() noexcept;

    void _ClearSelection();

    bool _CanSendVTMouseInput() const noexcept;
    bool _SendMouseEvent(UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept;

    void _SendKeyEvent(WORD vkey, WORD scanCode, WORD flags, bool keyDown) noexcept;
    void _SendCharEvent(wchar_t ch, WORD scanCode, WORD flags) noexcept;

    // Inherited via IControlAccessibilityInfo
    til::size GetFontSize() const noexcept override;
    til::rect GetBounds() const noexcept override;
    void ChangeViewport(const til::inclusive_rect& NewWindow) override;
    HRESULT GetHostUiaProvider(IRawElementProviderSimple** provider) noexcept override;
    til::rect GetPadding() const noexcept override;
};
