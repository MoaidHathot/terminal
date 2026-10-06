// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "HwndTerminal.hpp"

#include <DefaultSettings.h>
#include <windowsx.h>
#include <dcomp.h>

#include "HwndTerminalAutomationPeer.hpp"
#include "../../cascadia/TerminalCore/Terminal.hpp"
#include "../../renderer/atlas/AtlasEngine.h"
#include "../../renderer/base/renderer.hpp"
#include "../../renderer/uia/UiaRenderer.hpp"
#include "../../types/viewport.cpp"

using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;

static LPCWSTR term_window_class = L"HwndTerminalClass";

// The render thread tells the window thread that the engine has a new composition surface.
static constexpr UINT WM_HWNDTERMINAL_SWAPCHAIN_CHANGED = WM_USER + 0x4F;

// Composition is invisible when it goes wrong - nothing draws - so its steps say what they did
// to the debugger output, where a host's trace tooling can pick them up.
template<typename... Args>
static void _CompositionTrace(const wchar_t* format, Args... args) noexcept
{
    wchar_t buffer[512];
    if (swprintf_s(buffer, format, args...) > 0)
    {
        OutputDebugStringW(buffer);
    }
}

// One DirectComposition target per top-level window, shared by the composed terminals in it: a
// window can have only one target, and each terminal is a child visual of its root visual.
struct HwndTerminal::CompositionTarget
{
    HWND root = nullptr;
    wil::com_ptr<IDCompositionTarget> target;
    wil::com_ptr<IDCompositionVisual2> rootVisual;
};

// Lives on the UI thread, like everything else here; dcomp.dll is loaded on first use so that a
// host that never asks for a composed terminal never pays for it.
static wil::com_ptr<IDCompositionDesktopDevice> s_compositionDevice;
static std::unordered_map<HWND, std::weak_ptr<HwndTerminal::CompositionTarget>> s_compositionTargets;

static IDCompositionDesktopDevice* _GetCompositionDevice() noexcept
try
{
    if (!s_compositionDevice)
    {
        static const auto module = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        THROW_LAST_ERROR_IF_NULL(module);
        using PFN_DCompositionCreateDevice2 = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
        const auto createDevice = reinterpret_cast<PFN_DCompositionCreateDevice2>(GetProcAddress(module, "DCompositionCreateDevice2"));
        THROW_LAST_ERROR_IF_NULL(createDevice);
        wil::com_ptr<IDCompositionDesktopDevice> device;
        const auto hr = createDevice(nullptr, IID_PPV_ARGS(device.addressof()));
        _CompositionTrace(L"HwndTerminal composition: DCompositionCreateDevice2 -> 0x%08X\n", static_cast<unsigned>(hr));
        THROW_IF_FAILED(hr);
        s_compositionDevice = std::move(device);
    }
    return s_compositionDevice.get();
}
catch (...)
{
    _CompositionTrace(L"HwndTerminal composition: device creation failed 0x%08X\n", static_cast<unsigned>(wil::ResultFromCaughtException()));
    LOG_CAUGHT_EXCEPTION();
    return nullptr;
}

static std::shared_ptr<HwndTerminal::CompositionTarget> _GetCompositionTarget(HWND root) noexcept
try
{
    if (const auto it = s_compositionTargets.find(root); it != s_compositionTargets.end())
    {
        if (auto existing = it->second.lock())
        {
            return existing;
        }
        s_compositionTargets.erase(it);
    }

    const auto device = _GetCompositionDevice();
    if (!device)
    {
        return nullptr;
    }

    auto target = std::make_shared<HwndTerminal::CompositionTarget>();
    target->root = root;
    // topmost: the visual tree sits above the window's child windows, our input-only child included.
    const auto hr = device->CreateTargetForHwnd(root, TRUE, target->target.addressof());
    _CompositionTrace(L"HwndTerminal composition: CreateTargetForHwnd(0x%p) -> 0x%08X\n", root, static_cast<unsigned>(hr));
    THROW_IF_FAILED(hr);
    THROW_IF_FAILED(device->CreateVisual(target->rootVisual.addressof()));
    THROW_IF_FAILED(target->target->SetRoot(target->rootVisual.get()));
    THROW_IF_FAILED(device->Commit());
    s_compositionTargets[root] = target;
    return target;
}
catch (...)
{
    _CompositionTrace(L"HwndTerminal composition: target for 0x%p failed 0x%08X\n", root, static_cast<unsigned>(wil::ResultFromCaughtException()));
    LOG_CAUGHT_EXCEPTION();
    return nullptr;
}

STDMETHODIMP HwndTerminal::TsfDataProvider::QueryInterface(REFIID, void**) noexcept
{
    return E_NOTIMPL;
}

ULONG STDMETHODCALLTYPE HwndTerminal::TsfDataProvider::AddRef() noexcept
{
    return 1;
}

ULONG STDMETHODCALLTYPE HwndTerminal::TsfDataProvider::Release() noexcept
{
    return 1;
}

HWND HwndTerminal::TsfDataProvider::GetHwnd()
{
    return _terminal->GetHwnd();
}

RECT HwndTerminal::TsfDataProvider::GetViewport()
{
    const auto hwnd = GetHwnd();

    RECT rc;
    GetClientRect(hwnd, &rc);

    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclientrect
    // > The left and top members are zero. The right and bottom members contain the width and height of the window.
    // --> We can turn the client rect into a screen-relative rect by adding the left/top position.
    ClientToScreen(hwnd, reinterpret_cast<POINT*>(&rc));
    rc.right += rc.left;
    rc.bottom += rc.top;

    return rc;
}

RECT HwndTerminal::TsfDataProvider::GetCursorPosition()
{
    // Convert from columns/rows to pixels.
    til::point cursorPos;
    til::size fontSize;
    {
        const auto lock = _terminal->_terminal->LockForReading();
        cursorPos = _terminal->_terminal->GetTextBuffer().GetCursor().GetPosition(); // measured in terminal cells
        fontSize = _terminal->_actualFont.GetSize(); // measured in pixels, not DIP
    }
    POINT ptSuggestion = {
        .x = cursorPos.x * fontSize.width,
        .y = cursorPos.y * fontSize.height,
    };

    ClientToScreen(GetHwnd(), &ptSuggestion);

    // Final measurement should be in pixels
    return {
        .left = ptSuggestion.x,
        .top = ptSuggestion.y,
        .right = ptSuggestion.x + fontSize.width,
        .bottom = ptSuggestion.y + fontSize.height,
    };
}

void HwndTerminal::TsfDataProvider::HandleOutput(std::wstring_view text)
{
    _terminal->_WriteTextToConnection(text);
}

Microsoft::Console::Render::Renderer* HwndTerminal::TsfDataProvider::GetRenderer()
{
    return _terminal->_renderer.get();
}

// This magic flag is "documented" at https://msdn.microsoft.com/en-us/library/windows/desktop/ms646301(v=vs.85).aspx
// "If the high-order bit is 1, the key is down; otherwise, it is up."
static constexpr short KeyPressed{ gsl::narrow_cast<short>(0x8000) };

static constexpr bool _IsMouseMessage(UINT uMsg)
{
    return uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK ||
           uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP || uMsg == WM_MBUTTONDBLCLK ||
           uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_RBUTTONDBLCLK ||
           uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL;
}

LRESULT CALLBACK HwndTerminal::HwndTerminalWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam) noexcept
try
{
    if (WM_NCCREATE == uMsg)
    {
#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        HwndTerminal* that = static_cast<HwndTerminal*>(cs->lpCreateParams);
        that->_hwnd = wil::unique_hwnd(hwnd);

#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
        return DefWindowProc(hwnd, WM_NCCREATE, wParam, lParam);
    }
#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
    const auto publicTerminal = reinterpret_cast<HwndTerminal*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (publicTerminal)
    {
        if (_IsMouseMessage(uMsg))
        {
            if (publicTerminal->_CanSendVTMouseInput() && publicTerminal->_SendMouseEvent(uMsg, wParam, lParam))
            {
                // GH#6401: Capturing the mouse ensures that we get drag/release events
                // even if the user moves outside the window.
                // _SendMouseEvent returns false if the terminal's not in VT mode, so we'll
                // fall through to release the capture.
                switch (uMsg)
                {
                case WM_LBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_RBUTTONDOWN:
                    SetCapture(hwnd);
                    break;
                case WM_LBUTTONUP:
                case WM_MBUTTONUP:
                case WM_RBUTTONUP:
                    ReleaseCapture();
                    break;
                default:
                    break;
                }

                // Suppress all mouse events that made it into the terminal.
                return 0;
            }
        }

        switch (uMsg)
        {
        case WM_HWNDTERMINAL_SWAPCHAIN_CHANGED:
            _CompositionTrace(L"HwndTerminal composition: 0x%p received the swap chain message\n", hwnd);
            publicTerminal->_ApplyPendingSwapChain();
            return 0;
        case WM_WINDOWPOSCHANGED:
            // Moved, sized, shown or hidden (HwndHost hides the child when its element is collapsed):
            // the visual follows. DefWindowProc still runs, so WM_SIZE/WM_MOVE are generated as before.
            if (publicTerminal->_composed)
            {
                publicTerminal->_UpdateComposition();
            }
            break;
        case WM_GETOBJECT:
            if (lParam == UiaRootObjectId)
            {
                return UiaReturnRawElementProvider(hwnd, wParam, lParam, publicTerminal->_GetUiaProvider());
            }
            break;
        case WM_LBUTTONDOWN:
            LOG_IF_FAILED(publicTerminal->_StartSelection(lParam));
            return 0;
        case WM_LBUTTONUP:
            publicTerminal->_singleClickTouchdownPos = std::nullopt;
            [[fallthrough]];
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            ReleaseCapture();
            break;
        case WM_MOUSEMOVE:
            if (WI_IsFlagSet(wParam, MK_LBUTTON))
            {
                LOG_IF_FAILED(publicTerminal->_MoveSelection(lParam));
                return 0;
            }
            break;
        case WM_RBUTTONDOWN:
            try
            {
                if (publicTerminal->_terminal)
                {
                    const auto lock = publicTerminal->_terminal->LockForWriting();
                    if (publicTerminal->_terminal->IsSelectionActive())
                    {
                        const auto bufferData = publicTerminal->_terminal->RetrieveSelectedTextFromBuffer(false, false, true, true);
                        LOG_IF_FAILED(publicTerminal->_CopyTextToSystemClipboard(bufferData.plainText, bufferData.html, bufferData.rtf));
                        publicTerminal->_ClearSelection();
                        return 0;
                    }
                }
                publicTerminal->_PasteTextFromClipboard();
                return 0;
            }
            CATCH_LOG();
        case WM_DESTROY:
            publicTerminal->_DetachComposition();
            // Release Terminal's hwnd so Teardown doesn't try to destroy it again
            publicTerminal->_hwnd.release();
            publicTerminal->Teardown();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 0;
}

static bool RegisterTermClass(HINSTANCE hInstance) noexcept
{
    WNDCLASSW wc;
    if (GetClassInfoW(hInstance, term_window_class, &wc))
    {
        return true;
    }

    wc.style = 0;
    wc.lpfnWndProc = HwndTerminal::HwndTerminalWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = term_window_class;

    return RegisterClassW(&wc) != 0;
}

HwndTerminal::HwndTerminal(HWND parentHwnd) noexcept :
    HwndTerminal(parentHwnd, 0)
{
}

HwndTerminal::HwndTerminal(HWND parentHwnd, uint32_t flags) noexcept :
    _desiredFont{ L"Consolas", 0, DEFAULT_FONT_WEIGHT, 14, CP_UTF8 },
    _actualFont{ L"Consolas", 0, DEFAULT_FONT_WEIGHT, { 0, 14 }, CP_UTF8, false },
    _uiaProvider{ nullptr },
    _currentDpi{ USER_DEFAULT_SCREEN_DPI },
    _pfnWriteCallback{ nullptr },
    _multiClickTime{ 500 }, // this will be overwritten by the windows system double-click time
    _composed{ WI_IsFlagSet(flags, TERMINAL_CREATE_COMPOSED) }
{
    auto hInstance = wil::GetModuleInstanceHandle();

    if (RegisterTermClass(hInstance))
    {
        // The WS_EX_NOREDIRECTIONBITMAP flag is used to disable the GDI redirection surface
        // for reduced memory usage, because the window is fully rendered in DX. A composed
        // terminal (TERMINAL_CREATE_COMPOSED) relies on it too: its child window has no pixels of
        // its own, the visual on the top-level window covers it.
        CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP,
            term_window_class,
            nullptr,
            WS_CHILD |
                WS_CLIPCHILDREN |
                WS_CLIPSIBLINGS |
                WS_VISIBLE,
            0,
            0,
            0,
            0,
            parentHwnd,
            nullptr,
            hInstance,
            this);
    }
}

HwndTerminal::~HwndTerminal()
{
    Teardown();
}

HRESULT HwndTerminal::Initialize()
{
    _terminal = std::make_unique<::Microsoft::Terminal::Core::Terminal>();
    const auto lock = _terminal->LockForWriting();

    auto& renderSettings = _terminal->GetRenderSettings();
    renderSettings.SetColorTableEntry(TextColor::DEFAULT_BACKGROUND, RGB(12, 12, 12));
    renderSettings.SetColorTableEntry(TextColor::DEFAULT_FOREGROUND, RGB(204, 204, 204));
    _renderer = std::make_unique<::Microsoft::Console::Render::Renderer>(renderSettings, _terminal.get());

    auto engine = std::make_unique<::Microsoft::Console::Render::AtlasEngine>();
    if (_composed)
    {
        // No HWND: the engine renders into a composition surface and hands us its handle from the
        // render thread whenever it (re)creates the swap chain; the window thread wraps it in the
        // visual. The engine's XAML-scale compensation is for SwapChainPanel hosts, not for us.
        _CompositionTrace(L"HwndTerminal composition: terminal 0x%p composed (child 0x%p)\n", this, _hwnd.get());
        engine->SetUndoXamlScale(false);
        engine->SetCallback([this](HANDLE handle) noexcept { _OnSwapChainChanged(handle); });
        _ApplyBackgroundOpacity(renderSettings);
        engine->EnableTransparentBackground(_backgroundOpacity < 1.0f);
    }
    else
    {
        RETURN_IF_FAILED(engine->SetHwnd(_hwnd.get()));
    }
    _renderer->AddRenderEngine(engine.get());

    _UpdateFont(USER_DEFAULT_SCREEN_DPI);
    RECT windowRect;
    GetWindowRect(_hwnd.get(), &windowRect);

    const til::size windowSize{ windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };

    // Fist set up the dx engine with the window size in pixels.
    // Then, using the font, get the number of characters that can fit.
    const auto viewInPixels = Viewport::FromDimensions({ 0, 0 }, windowSize);
    RETURN_IF_FAILED(engine->SetWindowSize({ viewInPixels.Width(), viewInPixels.Height() }));

    _renderEngine = std::move(engine);

    _terminal->Create({ 80, 25 }, 9001, *_renderer);
    _terminal->SetWriteInputCallback([=](std::wstring_view input) noexcept { _WriteTextToConnection(input); });
    _terminal->SetCopyToClipboardCallback([=](wil::zwstring_view text) noexcept { _CopyTextToSystemClipboard(text, {}, {}); });
    _renderer->EnablePainting();

    _multiClickTime = std::chrono::milliseconds{ GetDoubleClickTime() };

    return S_OK;
}

void HwndTerminal::Teardown() noexcept
try
{
    // As a rule, detach resources from the Terminal before shutting them down.
    // This ensures that teardown is reentrant.
    _tsfHandle = {};

    // Shut down the renderer (and therefore the thread) before we implode
    _renderer.reset();
    _renderEngine.reset();
    _DetachComposition();

    // These two callbacks have a dangling reference to `this`; let's just clear them
    _terminal->SetWriteInputCallback(nullptr);
    _terminal->SetCopyToClipboardCallback(nullptr);

    if (auto localHwnd{ _hwnd.release() })
    {
        // If we're being called through WM_DESTROY, we won't get here (hwnd is already released)
        // If we're not, we may end up in Teardown _again_... but by the time we do, all other
        // resources have been released and will not be released again.
        DestroyWindow(localHwnd);
    }
}
CATCH_LOG();

void HwndTerminal::RegisterScrollCallback(std::function<void(int, int, int)> callback)
{
    if (!_terminal)
    {
        return;
    }
    _terminal->SetScrollPositionChangedCallback(callback);
}

void HwndTerminal::_WriteTextToConnection(const std::wstring_view input) noexcept
{
    if (input.empty() || !_pfnWriteCallback)
    {
        return;
    }

    try
    {
        auto callingText{ wil::make_cotaskmem_string(input.data(), input.size()) };
        _pfnWriteCallback(callingText.release());
    }
    CATCH_LOG();
}

void HwndTerminal::RegisterWriteCallback(const void _stdcall callback(wchar_t*))
{
    _pfnWriteCallback = callback;
}

::Microsoft::Console::Render::IRenderData* HwndTerminal::GetRenderData() const noexcept
{
    return _terminal.get();
}

HWND HwndTerminal::GetHwnd() const noexcept
{
    return _hwnd.get();
}

void HwndTerminal::_UpdateFont(int newDpi)
{
    if (!_terminal)
    {
        return;
    }
    _currentDpi = newDpi;

    // TODO: MSFT:20895307 If the font doesn't exist, this doesn't
    //      actually fail. We need a way to gracefully fallback.
    _renderer->TriggerFontChange(newDpi, _desiredFont, _actualFont);
}

IRawElementProviderSimple* HwndTerminal::_GetUiaProvider() noexcept
{
    // If TermControlUiaProvider throws during construction,
    // we don't want to try constructing an instance again and again.
    if (!_uiaProvider)
    {
        try
        {
            if (!_terminal)
            {
                return nullptr;
            }
            LOG_IF_FAILED(::Microsoft::WRL::MakeAndInitialize<HwndTerminalAutomationPeer>(&_uiaProvider, this->GetRenderData(), this));
            _uiaEngine = std::make_unique<::Microsoft::Console::Render::UiaEngine>(_uiaProvider.Get());
            LOG_IF_FAILED(_uiaEngine->Enable());
            const auto lock = _terminal->LockForWriting();
            _renderer->AddRenderEngine(_uiaEngine.get());
        }
        catch (...)
        {
            LOG_HR(wil::ResultFromCaughtException());
            _uiaProvider = nullptr;
        }
    }

    return _uiaProvider.Get();
}

HRESULT HwndTerminal::Refresh(const til::size windowSize, _Out_ til::size* dimensions)
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    RETURN_HR_IF_NULL(E_INVALIDARG, dimensions);

    const auto lock = _terminal->LockForWriting();

    _terminal->ClearSelection();

    RETURN_IF_FAILED(_renderEngine->SetWindowSize(windowSize));

    // Invalidate everything
    _renderer->TriggerRedrawAll();

    // Convert our new dimensions to characters
    const auto viewInPixels = Viewport::FromDimensions({}, windowSize);
    const auto vp = _renderEngine->GetViewportInCharacters(viewInPixels);

    // Guard against resizing below the visible minimum (GH#19996).
    auto size = vp.Dimensions();
    size.width = std::max(size.width, MINIMUM_VISIBLE_CELLS);
    size.height = std::max(size.height, MINIMUM_VISIBLE_CELLS);

    // If this function succeeds with S_FALSE, then the terminal didn't
    //      actually change size. No need to notify the connection of this
    //      no-op.
    // TODO: MSFT:20642295 Resizing the buffer will corrupt it
    // I believe we'll need support for CSI 2J, and additionally I think
    //      we're resetting the viewport to the top
    RETURN_IF_FAILED(_terminal->UserResize(size));
    dimensions->width = size.width;
    dimensions->height = size.height;

    return S_OK;
}

void HwndTerminal::SendOutput(std::wstring_view data)
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    _terminal->Write(data);
}

void _stdcall AvoidBuggyTSFConsoleFlags()
{
    Microsoft::Console::TSF::Handle::AvoidBuggyTSFConsoleFlags();
}

HRESULT _stdcall CreateTerminal(HWND parentHwnd, _Out_ void** hwnd, _Out_ void** terminal)
{
    return CreateTerminalEx(parentHwnd, 0, hwnd, terminal);
}

HRESULT _stdcall CreateTerminalEx(HWND parentHwnd, uint32_t flags, _Out_ void** hwnd, _Out_ void** terminal)
{
    RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(flags, ~TERMINAL_CREATE_COMPOSED));

    auto publicTerminal = std::make_unique<HwndTerminal>(parentHwnd, flags);
    RETURN_HR_IF_NULL(E_FAIL, publicTerminal->GetHwnd());

    RETURN_IF_FAILED(publicTerminal->Initialize());

    *hwnd = publicTerminal->GetHwnd();
    *terminal = publicTerminal.release();

    return S_OK;
}

void _stdcall TerminalRegisterScrollCallback(void* terminal, void __stdcall callback(int, int, int))
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->RegisterScrollCallback(callback);
}
CATCH_LOG()

void _stdcall TerminalRegisterWriteCallback(void* terminal, const void __stdcall callback(wchar_t*))
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->RegisterWriteCallback(callback);
}
CATCH_LOG()

void _stdcall TerminalSendOutput(void* terminal, LPCWSTR data)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->SendOutput(data);
}
CATCH_LOG()

/// <summary>
/// Triggers a terminal resize using the new width and height in pixel.
/// </summary>
/// <param name="terminal">Terminal pointer.</param>
/// <param name="width">New width of the terminal in pixels.</param>
/// <param name="height">New height of the terminal in pixels</param>
/// <param name="dimensions">Out parameter containing the columns and rows that fit the new size.</param>
/// <returns>HRESULT of the attempted resize.</returns>
HRESULT _stdcall TerminalTriggerResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);

    LOG_IF_WIN32_BOOL_FALSE(SetWindowPos(
        publicTerminal->GetHwnd(),
        nullptr,
        0,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        0));

    const til::size windowSize{ width, height };
    return publicTerminal->Refresh(windowSize, dimensions);
}

/// <summary>
/// Helper method for resizing the terminal using character column and row counts
/// </summary>
/// <param name="terminal">Pointer to the terminal object.</param>
/// <param name="dimensionsInCharacters">New terminal size in row and column count.</param>
/// <param name="dimensionsInPixels">Out parameter with the new size of the renderer.</param>
/// <returns>HRESULT of the attempted resize.</returns>
HRESULT _stdcall TerminalTriggerResizeWithDimension(_In_ void* terminal, _In_ til::size dimensionsInCharacters, _Out_ til::size* dimensionsInPixels)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, dimensionsInPixels);

    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);

    Viewport viewInPixels;
    {
        const auto viewInCharacters = Viewport::FromDimensions({}, dimensionsInCharacters);
        const auto lock = publicTerminal->_terminal->LockForReading();
        viewInPixels = publicTerminal->_renderEngine->GetViewportInPixels(viewInCharacters);
    }

    dimensionsInPixels->width = viewInPixels.Width();
    dimensionsInPixels->height = viewInPixels.Height();

    til::size unused;

    return TerminalTriggerResize(terminal, viewInPixels.Width(), viewInPixels.Height(), &unused);
}
CATCH_RETURN()

/// <summary>
/// Calculates the amount of rows and columns that fit in the provided width and height.
/// </summary>
/// <param name="terminal">Terminal pointer</param>
/// <param name="width">Width of the terminal area to calculate.</param>
/// <param name="height">Height of the terminal area to calculate.</param>
/// <param name="dimensions">Out parameter containing the columns and rows that fit the new size.</param>
/// <returns>HRESULT of the calculation.</returns>
HRESULT _stdcall TerminalCalculateResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions)
try
{
    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);

    const auto viewInPixels = Viewport::FromDimensions({}, { width, height });
    const auto lock = publicTerminal->_terminal->LockForReading();
    const auto viewInCharacters = publicTerminal->_renderEngine->GetViewportInCharacters(viewInPixels);

    dimensions->width = std::max(viewInCharacters.Width(), MINIMUM_VISIBLE_CELLS);
    dimensions->height = std::max(viewInCharacters.Height(), MINIMUM_VISIBLE_CELLS);

    return S_OK;
}
CATCH_RETURN()

void _stdcall TerminalDpiChanged(void* terminal, int newDpi)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_UpdateFont(newDpi);
}
CATCH_LOG()

void _stdcall TerminalUserScroll(void* terminal, int viewTop)
try
{
    if (const auto publicTerminal = static_cast<const HwndTerminal*>(terminal); publicTerminal && publicTerminal->_terminal)
    {
        const auto lock = publicTerminal->_terminal->LockForWriting();
        publicTerminal->_terminal->UserScrollViewport(viewTop);
    }
}
CATCH_LOG()

const unsigned int HwndTerminal::_NumberOfClicks(til::point point, std::chrono::steady_clock::time_point timestamp) noexcept
{
    // if click occurred at a different location or past the multiClickTimer...
    const auto delta{ timestamp - _lastMouseClickTimestamp };
    if (point != _lastMouseClickPos || delta > _multiClickTime)
    {
        // exit early. This is a single click.
        _multiClickCounter = 1;
    }
    else
    {
        _multiClickCounter++;
    }
    return _multiClickCounter;
}

HRESULT HwndTerminal::_StartSelection(LPARAM lParam) noexcept
try
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    const til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const auto lock = _terminal->LockForWriting();
    const auto altPressed = GetKeyState(VK_MENU) < 0;
    const til::size fontSize{ this->_actualFont.GetSize() };

    this->_terminal->SetBlockSelection(altPressed);

    const auto clickCount{ _NumberOfClicks(cursorPosition, std::chrono::steady_clock::now()) };

    // This formula enables the number of clicks to cycle properly between single-, double-, and triple-click.
    // To increase the number of acceptable click states, simply increment MAX_CLICK_COUNT and add another if-statement
    const unsigned int MAX_CLICK_COUNT = 3;
    const auto multiClickMapper = clickCount > MAX_CLICK_COUNT ? ((clickCount + MAX_CLICK_COUNT - 1) % MAX_CLICK_COUNT) + 1 : clickCount;

    if (multiClickMapper == 3)
    {
        _terminal->MultiClickSelection(cursorPosition / fontSize, ::Terminal::SelectionExpansion::Line);
    }
    else if (multiClickMapper == 2)
    {
        _terminal->MultiClickSelection(cursorPosition / fontSize, ::Terminal::SelectionExpansion::Word);
    }
    else
    {
        this->_terminal->ClearSelection();
        _singleClickTouchdownPos = cursorPosition;

        _lastMouseClickTimestamp = std::chrono::steady_clock::now();
        _lastMouseClickPos = cursorPosition;
    }
    this->_renderer->TriggerSelection();

    return S_OK;
}
CATCH_RETURN();

HRESULT HwndTerminal::_MoveSelection(LPARAM lParam) noexcept
try
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    const til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const auto lock = _terminal->LockForWriting();
    const til::size fontSize{ this->_actualFont.GetSize() };

    RETURN_HR_IF(E_NOT_VALID_STATE, fontSize.area() == 0); // either dimension = 0, area == 0

    // This is a copy of ControlInteractivity::PointerMoved
    if (_singleClickTouchdownPos)
    {
        const auto touchdownPoint = *_singleClickTouchdownPos;
        const auto dx = cursorPosition.x - touchdownPoint.x;
        const auto dy = cursorPosition.y - touchdownPoint.y;
        const auto w = fontSize.width;
        const auto distanceSquared = dx * dx + dy * dy;
        const auto maxDistanceSquared = w * w / 16; // (w / 4)^2

        if (distanceSquared >= maxDistanceSquared)
        {
            _terminal->SetSelectionAnchor(touchdownPoint / fontSize);
            // stop tracking the touchdown point
            _singleClickTouchdownPos = std::nullopt;
        }
    }

    this->_terminal->SetSelectionEnd(cursorPosition / fontSize);
    this->_renderer->TriggerSelection();

    return S_OK;
}
CATCH_RETURN();

void HwndTerminal::_ClearSelection()
{
    if (!_terminal)
    {
        return;
    }
    _terminal->ClearSelection();
    _renderer->TriggerSelection();
}

bool _stdcall TerminalIsSelectionActive(void* terminal)
try
{
    if (const auto publicTerminal = static_cast<const HwndTerminal*>(terminal); publicTerminal && publicTerminal->_terminal)
    {
        const auto lock = publicTerminal->_terminal->LockForReading();
        return publicTerminal->_terminal->IsSelectionActive();
    }
    return false;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

// Returns the selected text in the terminal.
const wchar_t* _stdcall TerminalGetSelection(void* terminal)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return nullptr;
    }

    std::wstring selectedText;
    {
        const auto lock = publicTerminal->_terminal->LockForWriting();
        auto bufferData = publicTerminal->_terminal->RetrieveSelectedTextFromBuffer(false);
        selectedText = std::move(bufferData.plainText);
        publicTerminal->_ClearSelection();
    }

    auto returnText = wil::make_cotaskmem_string_nothrow(selectedText.c_str());
    return returnText.release();
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return nullptr;
}

static ControlKeyStates getControlKeyState() noexcept
{
    struct KeyModifier
    {
        int vkey;
        ControlKeyStates flags;
    };

    constexpr std::array<KeyModifier, 5> modifiers{ {
        { VK_RMENU, ControlKeyStates::RightAltPressed },
        { VK_LMENU, ControlKeyStates::LeftAltPressed },
        { VK_RCONTROL, ControlKeyStates::RightCtrlPressed },
        { VK_LCONTROL, ControlKeyStates::LeftCtrlPressed },
        { VK_SHIFT, ControlKeyStates::ShiftPressed },
    } };

    ControlKeyStates flags;

    for (const auto& mod : modifiers)
    {
        const auto state = GetKeyState(mod.vkey);
        const auto isDown = state < 0;

        if (isDown)
        {
            flags |= mod.flags;
        }
    }

    return flags;
}

bool HwndTerminal::_CanSendVTMouseInput() const noexcept
{
    // Only allow the transit of mouse events if shift isn't pressed.
    const auto shiftPressed = GetKeyState(VK_SHIFT) < 0;
    const auto lock = _terminal->LockForReading();
    return !shiftPressed && _focused && _terminal && _terminal->IsTrackingMouseInput();
}

bool HwndTerminal::_SendMouseEvent(UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept
try
{
    if (!_terminal)
    {
        return false;
    }

    til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const til::size fontSize{ this->_actualFont.GetSize() };
    short wheelDelta{ 0 };
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL)
    {
        wheelDelta = HIWORD(wParam);

        // If it's a *WHEEL event, it's in screen coordinates, not window (?!)
        ScreenToClient(_hwnd.get(), cursorPosition.as_win32_point());
    }

    const Microsoft::Console::VirtualTerminal::TerminalInput::MouseButtonState state{
        WI_IsFlagSet(GetKeyState(VK_LBUTTON), KeyPressed),
        WI_IsFlagSet(GetKeyState(VK_MBUTTON), KeyPressed),
        WI_IsFlagSet(GetKeyState(VK_RBUTTON), KeyPressed)
    };

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForReading();
        out = _terminal->SendMouseEvent(cursorPosition / fontSize, uMsg, getControlKeyState(), wheelDelta, state);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
        return true;
    }
    return false;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

void HwndTerminal::_SendKeyEvent(WORD vkey, WORD scanCode, WORD flags, bool keyDown) noexcept
try
{
    if (!_terminal)
    {
        return;
    }

    auto modifiers = getControlKeyState();
    if (WI_IsFlagSet(flags, ENHANCED_KEY))
    {
        modifiers |= ControlKeyStates::EnhancedKey;
    }
    if (vkey && keyDown && _uiaProvider)
    {
        _uiaProvider->RecordKeyEvent(vkey);
    }

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForReading();
        out = _terminal->SendKeyEvent(vkey, scanCode, modifiers, keyDown);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
    }
}
CATCH_LOG();

void HwndTerminal::_SendCharEvent(wchar_t ch, WORD scanCode, WORD flags) noexcept
try
{
    if (!_terminal)
    {
        return;
    }

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForWriting();

        if (_terminal->IsSelectionActive())
        {
            _ClearSelection();
            if (ch == UNICODE_ESC)
            {
                // ESC should clear any selection before it triggers input.
                // Other characters pass through.
                return;
            }
        }

        if (ch == UNICODE_TAB)
        {
            // TAB was handled as a keydown event (cf. Terminal::SendKeyEvent)
            return;
        }

        auto modifiers = getControlKeyState();
        if (WI_IsFlagSet(flags, ENHANCED_KEY))
        {
            modifiers |= ControlKeyStates::EnhancedKey;
        }

        out = _terminal->SendCharEvent(ch, scanCode, modifiers);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
    }
}
CATCH_LOG();

void _stdcall TerminalSendKeyEvent(void* terminal, WORD vkey, WORD scanCode, WORD flags, bool keyDown)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_SendKeyEvent(vkey, scanCode, flags, keyDown);
}
CATCH_LOG()

void _stdcall TerminalSendCharEvent(void* terminal, wchar_t ch, WORD scanCode, WORD flags)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_SendCharEvent(ch, scanCode, flags);
}
CATCH_LOG()

void _stdcall DestroyTerminal(void* terminal)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    delete publicTerminal;
}

// Updates the terminal font type, size, color, as well as the background/foreground colors to a specified theme.
void _stdcall TerminalSetTheme(void* terminal, TerminalTheme theme, LPCWSTR fontFamily, til::CoordType fontSize, int newDpi)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return;
    }

    {
        const auto lock = publicTerminal->_terminal->LockForWriting();

        auto& renderSettings = publicTerminal->_terminal->GetRenderSettings();
        renderSettings.SetColorTableEntry(TextColor::DEFAULT_FOREGROUND, theme.DefaultForeground);
        renderSettings.SetColorTableEntry(TextColor::DEFAULT_BACKGROUND, theme.DefaultBackground);
        renderSettings.SetColorTableEntry(TextColor::SELECTION_BACKGROUND, theme.DefaultSelectionBackground);
        publicTerminal->_ApplyBackgroundOpacity(renderSettings);

        // Set the font colors
        for (size_t tableIndex = 0; tableIndex < 16; tableIndex++)
        {
            // It's using gsl::at to check the index is in bounds, but the analyzer still calls this array-to-pointer-decay
            GSL_SUPPRESS(bounds.3)
            renderSettings.SetColorTableEntry(tableIndex, gsl::at(theme.ColorTable, tableIndex));
        }

        // Save these values as the new default render settings.
        renderSettings.SaveDefaultSettings();

        publicTerminal->_terminal->SetCursorStyle(static_cast<Microsoft::Console::VirtualTerminal::DispatchTypes::CursorStyle>(theme.CursorStyle));

        publicTerminal->_desiredFont = { fontFamily, 0, DEFAULT_FONT_WEIGHT, static_cast<float>(fontSize), CP_UTF8 };
        publicTerminal->_desiredFont.SetEnableBuiltinGlyphs(true);
        publicTerminal->_UpdateFont(newDpi);
    }

    // When the font changes the terminal dimensions need to be recalculated since the available row and column
    // space will have changed.
    RECT windowRect;
    GetWindowRect(publicTerminal->_hwnd.get(), &windowRect);

    til::size dimensions;
    const til::size windowSize{ windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };
    publicTerminal->Refresh(windowSize, &dimensions);
}

void __stdcall TerminalSetFocused(void* terminal, bool focused)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_setFocused(focused);
}

void _stdcall TerminalSetBackgroundOpacity(void* terminal, float opacity)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_composed || !publicTerminal->_terminal)
    {
        return;
    }

    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (publicTerminal->_backgroundOpacity == opacity)
    {
        return;
    }

    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_backgroundOpacity = opacity;
    publicTerminal->_ApplyBackgroundOpacity(publicTerminal->_terminal->GetRenderSettings());
    if (publicTerminal->_renderEngine)
    {
        publicTerminal->_renderEngine->EnableTransparentBackground(opacity < 1.0f);
    }
    if (publicTerminal->_renderer)
    {
        publicTerminal->_renderer->TriggerRedrawAll();
    }
}
CATCH_LOG()

void _stdcall TerminalUpdateComposition(void* terminal)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (publicTerminal && publicTerminal->_composed)
    {
        publicTerminal->_UpdateComposition();
    }
}
CATCH_LOG()

// The caller holds the terminal lock. Cells with the default background get the opacity as their
// alpha (the renderer keeps the default background's alpha when transparency is enabled and forces
// every other background opaque); a theme change resets the entry, so TerminalSetTheme calls this too.
void HwndTerminal::_ApplyBackgroundOpacity(::Microsoft::Console::Render::RenderSettings& renderSettings) noexcept
{
    if (!_composed)
    {
        return;
    }

    const auto alpha = gsl::narrow_cast<uint8_t>(std::lround(_backgroundOpacity * 255.0f));
    const til::color background{ renderSettings.GetColorTableEntry(TextColor::DEFAULT_BACKGROUND) };
    renderSettings.SetColorTableEntry(TextColor::DEFAULT_BACKGROUND, background.with_alpha(alpha).abgr);
}

// Render thread: the engine made a new swap chain and this is its composition surface handle. The
// engine owns that handle; a duplicate goes to the window thread, which owns the visual.
void HwndTerminal::_OnSwapChainChanged(HANDLE handle) noexcept
try
{
    wil::unique_handle duplicate;
    if (handle)
    {
        THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), duplicate.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    }

    {
        const std::lock_guard lock{ _pendingSwapChainLock };
        _pendingSwapChainHandle = std::move(duplicate);
    }

    const auto hwnd = _hwnd.get();
    _CompositionTrace(L"HwndTerminal composition: swap chain changed, handle 0x%p, posting to 0x%p\n", handle, hwnd);
    if (hwnd)
    {
        PostMessageW(hwnd, WM_HWNDTERMINAL_SWAPCHAIN_CHANGED, 0, 0);
    }
}
CATCH_LOG()

// Window thread: wrap the pending surface handle in the visual's content.
void HwndTerminal::_ApplyPendingSwapChain() noexcept
try
{
    wil::unique_handle handle;
    {
        const std::lock_guard lock{ _pendingSwapChainLock };
        handle = std::move(_pendingSwapChainHandle);
    }
    if (!handle)
    {
        return;
    }

    const auto device = _GetCompositionDevice();
    if (!device)
    {
        return;
    }

    if (!_compositionVisual)
    {
        THROW_IF_FAILED(device->CreateVisual(_compositionVisual.addressof()));
    }

    wil::com_ptr<IUnknown> surface;
    const auto hr = device->CreateSurfaceFromHandle(handle.get(), surface.addressof());
    _CompositionTrace(L"HwndTerminal composition: CreateSurfaceFromHandle(0x%p) -> 0x%08X\n", handle.get(), static_cast<unsigned>(hr));
    THROW_IF_FAILED(hr);
    THROW_IF_FAILED(_compositionVisual->SetContent(surface.get()));
    _swapChainHandle = std::move(handle);

    _UpdateComposition();
}
catch (...)
{
    _CompositionTrace(L"HwndTerminal composition: applying the swap chain failed 0x%08X\n", static_cast<unsigned>(wil::ResultFromCaughtException()));
    LOG_CAUGHT_EXCEPTION();
}

// Window thread: the visual sits on the child's current top-level window, at the child's position
// in that window's client area, and only while the child is shown. Cheap enough to call on every
// WM_WINDOWPOSCHANGED.
void HwndTerminal::_UpdateComposition() noexcept
try
{
    if (!_composed || !_compositionVisual)
    {
        return;
    }

    const auto hwnd = _hwnd.get();
    const auto device = _GetCompositionDevice();
    if (!hwnd || !device)
    {
        return;
    }

    const auto root = GetAncestor(hwnd, GA_ROOT);
    const auto shown = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0 && root && root != hwnd;

    if (!shown || (_compositionTarget && _compositionTarget->root != root))
    {
        if (_compositionVisualAttached && _compositionTarget)
        {
            LOG_IF_FAILED(_compositionTarget->rootVisual->RemoveVisual(_compositionVisual.get()));
            _compositionVisualAttached = false;
        }
        if (_compositionTarget && _compositionTarget->root != root)
        {
            _compositionTarget.reset();
        }
    }

    if (shown)
    {
        if (!_compositionTarget)
        {
            _compositionTarget = _GetCompositionTarget(root);
            if (!_compositionTarget)
            {
                return;
            }
        }

        RECT rect{};
        GetWindowRect(hwnd, &rect);
        MapWindowPoints(HWND_DESKTOP, root, reinterpret_cast<POINT*>(&rect), 2);
        THROW_IF_FAILED(_compositionVisual->SetOffsetX(static_cast<float>(rect.left)));
        THROW_IF_FAILED(_compositionVisual->SetOffsetY(static_cast<float>(rect.top)));

        if (!_compositionVisualAttached)
        {
            THROW_IF_FAILED(_compositionTarget->rootVisual->AddVisual(_compositionVisual.get(), TRUE, nullptr));
            _compositionVisualAttached = true;
        }

        _CompositionTrace(L"HwndTerminal composition: visual of 0x%p on root 0x%p at (%d,%d) size %dx%d\n", hwnd, root, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    else
    {
        _CompositionTrace(L"HwndTerminal composition: visual of 0x%p hidden (root 0x%p)\n", hwnd, root);
    }

    THROW_IF_FAILED(device->Commit());
}
catch (...)
{
    _CompositionTrace(L"HwndTerminal composition: update failed 0x%08X\n", static_cast<unsigned>(wil::ResultFromCaughtException()));
    LOG_CAUGHT_EXCEPTION();
}

void HwndTerminal::_DetachComposition() noexcept
try
{
    if (_compositionVisualAttached && _compositionTarget)
    {
        LOG_IF_FAILED(_compositionTarget->rootVisual->RemoveVisual(_compositionVisual.get()));
        if (const auto device = _GetCompositionDevice())
        {
            LOG_IF_FAILED(device->Commit());
        }
    }
    _compositionVisualAttached = false;
    _compositionTarget.reset();
    _compositionVisual.reset();
    _swapChainHandle.reset();
    const std::lock_guard lock{ _pendingSwapChainLock };
    _pendingSwapChainHandle.reset();
}
CATCH_LOG()

void HwndTerminal::_setFocused(bool focused) noexcept
{
    if (_focused == focused)
    {
        return;
    }

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForWriting();

        _focused = focused;

        if (focused)
        {
            if (!_tsfHandle)
            {
                _tsfHandle = Microsoft::Console::TSF::Handle::Create();
                _tsfHandle.AssociateFocus(&_tsfDataProvider);
            }

            if (const auto uiaEngine = _uiaEngine.get())
            {
                LOG_IF_FAILED(uiaEngine->Enable());
            }
        }
        else
        {
            if (const auto uiaEngine = _uiaEngine.get())
            {
                LOG_IF_FAILED(uiaEngine->Disable());
            }
        }

        _renderer->AllowCursorVisibility(Microsoft::Console::Render::InhibitionSource::Host, focused);
        out = _terminal->FocusChanged(focused);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
    }
}

// Routine Description:
// - Copies the text given onto the global system clipboard.
// Arguments:
// - text - selected text in plain-text format
// - htmlData - selected text in HTML format
// - rtfData - selected text in RTF format
HRESULT HwndTerminal::_CopyTextToSystemClipboard(wil::zwstring_view text, wil::zstring_view htmlData, wil::zstring_view rtfData) const
try
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);

    // allocate the final clipboard data
    const auto cchNeeded = text.size() + 1;
    const auto cbNeeded = sizeof(wchar_t) * cchNeeded;
    wil::unique_hglobal globalHandle(GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, cbNeeded));
    RETURN_LAST_ERROR_IF_NULL(globalHandle.get());

    auto pwszClipboard = static_cast<PWSTR>(GlobalLock(globalHandle.get()));
    RETURN_LAST_ERROR_IF_NULL(pwszClipboard);

    // The pattern gets a bit strange here because there's no good wil built-in for global lock of this type.
    // Try to copy then immediately unlock. Don't throw until after (so the hglobal won't be freed until we unlock).
    const auto hr = StringCchCopyW(pwszClipboard, cchNeeded, text.data());
    GlobalUnlock(globalHandle.get());
    RETURN_IF_FAILED(hr);

    // Set global data to clipboard
    RETURN_LAST_ERROR_IF(!OpenClipboard(_hwnd.get()));

    { // Clipboard Scope
        auto clipboardCloser = wil::scope_exit([]() {
            LOG_LAST_ERROR_IF(!CloseClipboard());
        });

        RETURN_LAST_ERROR_IF(!EmptyClipboard());
        RETURN_LAST_ERROR_IF_NULL(SetClipboardData(CF_UNICODETEXT, globalHandle.get()));

        if (!htmlData.empty())
        {
            RETURN_IF_FAILED(_CopyToSystemClipboard(htmlData, L"HTML Format"));
        }

        if (!rtfData.empty())
        {
            RETURN_IF_FAILED(_CopyToSystemClipboard(rtfData, L"Rich Text Format"));
        }
    }

    // only free if we failed.
    // the memory has to remain allocated if we successfully placed it on the clipboard.
    // Releasing the smart pointer will leave it allocated as we exit scope.
    globalHandle.release();

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Copies the given string onto the global system clipboard in the specified format
// Arguments:
// - stringToCopy - The string to copy
// - lpszFormat - the name of the format
HRESULT HwndTerminal::_CopyToSystemClipboard(wil::zstring_view stringToCopy, LPCWSTR lpszFormat) const
{
    const auto cbData = stringToCopy.size() + 1; // +1 for '\0'
    if (cbData)
    {
        wil::unique_hglobal globalHandleData(GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, cbData));
        RETURN_LAST_ERROR_IF_NULL(globalHandleData.get());

        auto pszClipboardHTML = static_cast<PSTR>(GlobalLock(globalHandleData.get()));
        RETURN_LAST_ERROR_IF_NULL(pszClipboardHTML);

        // The pattern gets a bit strange here because there's no good wil built-in for global lock of this type.
        // Try to copy then immediately unlock. Don't throw until after (so the hglobal won't be freed until we unlock).
        const auto hr2 = StringCchCopyA(pszClipboardHTML, cbData, stringToCopy.data());
        GlobalUnlock(globalHandleData.get());
        RETURN_IF_FAILED(hr2);

        const auto CF_FORMAT = RegisterClipboardFormatW(lpszFormat);
        RETURN_LAST_ERROR_IF(0 == CF_FORMAT);

        RETURN_LAST_ERROR_IF_NULL(SetClipboardData(CF_FORMAT, globalHandleData.get()));

        // only free if we failed.
        // the memory has to remain allocated if we successfully placed it on the clipboard.
        // Releasing the smart pointer will leave it allocated as we exit scope.
        globalHandleData.release();
    }

    return S_OK;
}

void HwndTerminal::_PasteTextFromClipboard() noexcept
{
    // Get paste data from clipboard
    if (!OpenClipboard(_hwnd.get()))
    {
        return;
    }

    auto ClipboardDataHandle = GetClipboardData(CF_UNICODETEXT);
    if (ClipboardDataHandle == nullptr)
    {
        CloseClipboard();
        return;
    }

    if (const auto pwstr = static_cast<PCWCH>(GlobalLock(ClipboardDataHandle)))
    {
        _WriteTextToConnection(pwstr);
    }

    GlobalUnlock(ClipboardDataHandle);

    CloseClipboard();
}

til::size HwndTerminal::GetFontSize() const noexcept
{
    return _actualFont.GetSize();
}

til::rect HwndTerminal::GetBounds() const noexcept
{
    til::rect windowRect;
    GetWindowRect(_hwnd.get(), windowRect.as_win32_rect());
    return windowRect;
}

til::rect HwndTerminal::GetPadding() const noexcept
{
    return {};
}

void HwndTerminal::ChangeViewport(const til::inclusive_rect& NewWindow)
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    _terminal->UserScrollViewport(NewWindow.top);
}

HRESULT HwndTerminal::GetHostUiaProvider(IRawElementProviderSimple** provider) noexcept
{
    return UiaHostProviderFromHwnd(_hwnd.get(), provider);
}
