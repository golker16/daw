#include "PUI.h"

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace
{
    constexpr COLORREF kUiPetrol = RGB(44, 58, 66);
    constexpr COLORREF kUiAnthracite = RGB(48, 54, 61);
    constexpr COLORREF kUiGraphite = RGB(59, 67, 76);
    constexpr COLORREF kUiGraphiteSoft = RGB(53, 61, 69);
    constexpr COLORREF kUiLine = RGB(74, 85, 96);
    constexpr COLORREF kUiLineSoft = RGB(62, 72, 82);
    constexpr COLORREF kUiText = RGB(199, 205, 211);
    constexpr COLORREF kUiTextSoft = RGB(154, 163, 171);
    constexpr COLORREF kUiTextDim = RGB(119, 128, 136);
    constexpr COLORREF kUiLime = RGB(166, 240, 106);
    constexpr COLORREF kUiLimeDim = RGB(119, 164, 84);
    constexpr COLORREF kUiRedAccent = RGB(140, 88, 92);
    constexpr COLORREF kUiShadow = RGB(25, 29, 34);

    constexpr int kOuterPadding = 12;
    constexpr int kGap = 8;
    constexpr int kToolbarHeight = 82;
    constexpr int kTransportHeight = 30;
    constexpr int kInfoStripHeight = 54;
    constexpr int kBrowserWidth = 286;
    constexpr int kBrowserTabCount = 3;
    constexpr int kBrowserTabHeight = 22;
    constexpr int kBrowserRowHeight = 28;
    constexpr int kBrowserIndentWidth = 16;
    constexpr int kPluginHeight = 170;
    constexpr int kMixerHeight = 360;
    constexpr int kSectionHeaderHeight = 24;
    constexpr int kSurfaceHeaderHeight = 22;
    constexpr int kDetachedPaneGap = 10;
    constexpr int kStepCount = 16;
    constexpr int kPlaylistCellCount = 32;
    constexpr int kPianoLaneCount = 24;
    constexpr int kPlaylistLaneHeight = 36;
    constexpr int kPlaylistTimelineHeight = 28;
    constexpr int kPlaylistTrackHeaderWidth = 138;
    constexpr int kToolbarGlyphSize = 13;

    constexpr const char* kBrowserTabs[kBrowserTabCount] = {
        "Project",
        "Samples",
        "Automation"};

    constexpr const char* kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"};

    void fillRectColor(HDC dc, const RECT& rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    COLORREF blendColor(COLORREF from, COLORREF to, int numerator, int denominator)
    {
        if (denominator <= 0)
        {
            return to;
        }

        const int red =
            (static_cast<int>(GetRValue(from)) * (denominator - numerator) + static_cast<int>(GetRValue(to)) * numerator) / denominator;
        const int green =
            (static_cast<int>(GetGValue(from)) * (denominator - numerator) + static_cast<int>(GetGValue(to)) * numerator) / denominator;
        const int blue =
            (static_cast<int>(GetBValue(from)) * (denominator - numerator) + static_cast<int>(GetBValue(to)) * numerator) / denominator;
        return RGB(red, green, blue);
    }

    void drawHorizontalLine(HDC dc, int left, int right, int y, COLORREF color)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, left, y, nullptr);
        LineTo(dc, right, y);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void drawVerticalLine(HDC dc, int x, int top, int bottom, COLORREF color)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, x, top, nullptr);
        LineTo(dc, x, bottom);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void drawFilledCircle(HDC dc, const RECT& rect, COLORREF fillColor, COLORREF outlineColor)
    {
        HBRUSH brush = CreateSolidBrush(fillColor);
        HPEN pen = CreatePen(PS_SOLID, 1, outlineColor);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void drawCollapseTriangle(HDC dc, int x, int y, bool expanded, COLORREF color)
    {
        POINT points[3]{};
        if (expanded)
        {
            points[0] = POINT{x, y};
            points[1] = POINT{x + 8, y};
            points[2] = POINT{x + 4, y + 6};
        }
        else
        {
            points[0] = POINT{x, y};
            points[1] = POINT{x, y + 8};
            points[2] = POINT{x + 6, y + 4};
        }

        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Polygon(dc, points, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void drawToolbarGlyph(HDC dc, WORD controlId, const RECT& rect, COLORREF color, bool active)
    {
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

        switch (controlId)
        {
        case 1003:
            if (active)
            {
                drawVerticalLine(dc, rect.left + 3, rect.top + 2, rect.bottom - 2, color);
                drawVerticalLine(dc, rect.left + 8, rect.top + 2, rect.bottom - 2, color);
            }
            else
            {
                POINT triangle[3]{
                    POINT{rect.left + 2, rect.top + 1},
                    POINT{rect.left + 2, rect.bottom - 1},
                    POINT{rect.right - 1, rect.top + ((rect.bottom - rect.top) / 2)}};
                HBRUSH brush = CreateSolidBrush(color);
                HGDIOBJ oldGlyphBrush = SelectObject(dc, brush);
                Polygon(dc, triangle, 3);
                SelectObject(dc, oldGlyphBrush);
                DeleteObject(brush);
            }
            break;

        case 1004:
            Rectangle(dc, rect.left + 2, rect.top + 2, rect.right - 1, rect.bottom - 1);
            break;

        case 1005:
            drawFilledCircle(
                dc,
                RECT{rect.left + 2, rect.top + 2, rect.right - 1, rect.bottom - 1},
                active ? color : blendColor(kUiAnthracite, color, 2, 7),
                color);
            break;

        case 1013:
            Rectangle(dc, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1);
            drawVerticalLine(dc, rect.left + 4, rect.top + 2, rect.bottom - 2, color);
            break;

        case 1014:
            Rectangle(dc, rect.left + 1, rect.top + 1, rect.left + 5, rect.top + 5);
            Rectangle(dc, rect.left + 7, rect.top + 1, rect.right - 1, rect.top + 5);
            Rectangle(dc, rect.left + 1, rect.top + 7, rect.left + 5, rect.bottom - 1);
            Rectangle(dc, rect.left + 7, rect.top + 7, rect.right - 1, rect.bottom - 1);
            break;

        case 1015:
            Rectangle(dc, rect.left + 1, rect.top + 2, rect.right - 1, rect.bottom - 1);
            drawVerticalLine(dc, rect.left + 4, rect.top + 2, rect.bottom - 1, color);
            drawVerticalLine(dc, rect.left + 8, rect.top + 2, rect.bottom - 1, color);
            break;

        case 1016:
            Rectangle(dc, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1);
            drawVerticalLine(dc, rect.left + 4, rect.top + 1, rect.bottom - 1, color);
            drawVerticalLine(dc, rect.left + 8, rect.top + 1, rect.bottom - 1, color);
            drawHorizontalLine(dc, rect.left + 1, rect.right - 1, rect.top + 4, color);
            drawHorizontalLine(dc, rect.left + 1, rect.right - 1, rect.top + 8, color);
            break;

        case 1017:
            drawVerticalLine(dc, rect.left + 3, rect.top + 1, rect.bottom - 1, color);
            drawVerticalLine(dc, rect.left + 7, rect.top + 3, rect.bottom - 2, color);
            drawVerticalLine(dc, rect.left + 11, rect.top + 2, rect.bottom - 3, color);
            break;

        case 1018:
            Rectangle(dc, rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2);
            drawHorizontalLine(dc, rect.left + 4, rect.right - 4, rect.top + ((rect.bottom - rect.top) / 2), color);
            break;

        default:
            break;
        }

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    std::string boolLabel(bool value, const char* onText, const char* offText)
    {
        return value ? onText : offText;
    }

    const char* paneName(UI::WorkspacePane pane)
    {
        switch (pane)
        {
        case UI::WorkspacePane::Browser: return "Browser";
        case UI::WorkspacePane::ChannelRack: return "Channel Rack";
        case UI::WorkspacePane::PianoRoll: return "Piano Roll";
        case UI::WorkspacePane::Playlist: return "Playlist";
        case UI::WorkspacePane::Mixer: return "Mixer";
        case UI::WorkspacePane::Plugin: return "Plugin";
        default: return "Workspace";
        }
    }

    std::string noteLabelFromLane(int lane)
    {
        const int midiNote = 36 + std::max(0, lane);
        const int octave = (midiNote / 12) - 1;
        return std::string(kNoteNames[midiNote % 12]) + std::to_string(octave);
    }

    int normalizedMeterFill(double valueDb)
    {
        const double normalized = std::clamp((valueDb + 18.0) / 18.0, 0.1, 1.0);
        return static_cast<int>(normalized * 100.0 + 0.5);
    }
}

UI::UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine)
    : hInstance_(hInstance),
      nCmdShow_(nCmdShow),
      engine_(engine)
{
    workspace_.playlistVisible = true;
    workspace_.browserVisible = true;
    workspace_.channelRackVisible = false;
    workspace_.pianoRollVisible = false;
    workspace_.mixerVisible = false;
    workspace_.pluginVisible = false;
    workspace_.focusedPane = WorkspacePane::Playlist;

    dockedPanes_ = {
        {WorkspacePane::Browser, {}, false, true, "Browser"},
        {WorkspacePane::ChannelRack, {}, false, true, "Channel Rack"},
        {WorkspacePane::PianoRoll, {}, false, true, "Piano Roll"},
        {WorkspacePane::Playlist, {}, false, true, "Playlist"},
        {WorkspacePane::Mixer, {}, false, true, "Mixer"},
        {WorkspacePane::Plugin, {}, false, true, "Plugin"}};
    detachPane(WorkspacePane::ChannelRack);
    detachPane(WorkspacePane::Mixer);

    if (DockedPaneState* channelRackPane = findDockedPane(WorkspacePane::ChannelRack))
    {
        channelRackPane->bounds = RECT{120, 120, 700, 670};
    }
    if (DockedPaneState* mixerPane = findDockedPane(WorkspacePane::Mixer))
    {
        mixerPane->bounds = RECT{220, 160, 1320, 590};
    }

    registerWindowClass();
    createMainWindow();
    createMainMenu();
    createControls();
    refreshPluginManagerState();
    refreshFromEngineSnapshot();
    startUiTimer();
}

int UI::run()
{
    ShowWindow(hwnd_, nCmdShow_);
    UpdateWindow(hwnd_);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK UI::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UI* ui = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTA* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        ui = reinterpret_cast<UI*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    else
    {
        ui = reinterpret_cast<UI*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    switch (uMsg)
    {
    case WM_COMMAND:
        if (ui != nullptr)
        {
            ui->handleCommand(static_cast<WORD>(LOWORD(wParam)));
            return 0;
        }
        break;

    case WM_TIMER:
        if (ui != nullptr && wParam == kUiTimerId)
        {
            ui->refreshFromEngineSnapshot();
            return 0;
        }
        break;

    case WM_DRAWITEM:
        if (ui != nullptr && lParam != 0)
        {
            return ui->drawThemedButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)) ? TRUE : FALSE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (ui != nullptr)
        {
            return reinterpret_cast<LRESULT>(ui->resolveLabelBrush(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam)));
        }
        break;

    case WM_ERASEBKGND:
        if (ui != nullptr)
        {
            ui->paintMainBackground(reinterpret_cast<HDC>(wParam));
            return 1;
        }
        break;

    case WM_SIZE:
        if (ui != nullptr)
        {
            ui->layoutControls();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (ui != nullptr && ui->handleKeyDown(wParam, lParam))
        {
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (ui != nullptr && ui->handleKeyDown(wParam, lParam))
        {
            return 0;
        }
        break;

    case WM_DESTROY:
        if (ui != nullptr)
        {
            ui->destroyDetachedWindows();
            ui->stopUiTimer();
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK UI::PluginManagerProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UI* ui = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTA* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        ui = reinterpret_cast<UI*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    else
    {
        ui = reinterpret_cast<UI*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    switch (uMsg)
    {
    case WM_COMMAND:
        if (ui != nullptr)
        {
            ui->handlePluginManagerCommand(static_cast<WORD>(LOWORD(wParam)));
            return 0;
        }
        break;

    case WM_SIZE:
        if (ui != nullptr)
        {
            ui->layoutPluginManagerWindow();
            return 0;
        }
        break;

    case WM_CLOSE:
        if (ui != nullptr)
        {
            ui->closePluginManagerWindow();
            return 0;
        }
        break;

    default:
        break;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK UI::SurfaceProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UI* ui = reinterpret_cast<UI*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTA* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        ui = reinterpret_cast<UI*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }

    if (ui == nullptr)
    {
        return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }

    const SurfaceKind kind = ui->kindFromSurfaceHandle(hwnd);

    switch (uMsg)
    {
    case WM_PAINT:
        ui->paintSurface(hwnd, kind);
        return 0;

    case WM_LBUTTONDOWN:
        ui->handleSurfaceMouseDown(hwnd, kind, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        ui->handleSurfaceMouseMove(hwnd, kind, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
        return 0;

    case WM_LBUTTONUP:
        ui->handleSurfaceMouseUp(hwnd, kind, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    default:
        break;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK UI::DetachedPaneProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UI* ui = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTA* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        ui = reinterpret_cast<UI*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    else
    {
        ui = reinterpret_cast<UI*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    if (ui == nullptr)
    {
        return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }

    DockedPaneState* pane = ui->findDockedPaneWindow(hwnd);

    switch (uMsg)
    {
    case WM_SIZE:
        if (pane != nullptr)
        {
            ui->layoutDetachedPaneWindow(*pane);
            GetWindowRect(hwnd, &pane->bounds);
            return 0;
        }
        break;

    case WM_MOVE:
    case WM_EXITSIZEMOVE:
        if (pane != nullptr)
        {
            GetWindowRect(hwnd, &pane->bounds);
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (pane != nullptr)
        {
            ui->workspace_.focusedPane = pane->pane;
            ui->updateViewButtons();
            return 0;
        }
        break;

    case WM_CLOSE:
        if (pane != nullptr)
        {
            ui->setPaneVisible(pane->pane, false);
            ui->workspace_.focusedPane = WorkspacePane::Playlist;
            ui->updateDockingModel();
            ui->updateViewButtons();
            ShowWindow(hwnd, SW_HIDE);
            ui->layoutControls();
            ui->refreshFromEngineSnapshot();
            return 0;
        }
        break;

    default:
        break;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

void UI::registerWindowClass()
{
    static HBRUSH darkFrameBrush = CreateSolidBrush(kUiAnthracite);
    static HBRUSH darkPanelBrush = CreateSolidBrush(kUiPetrol);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = UI::WindowProc;
    wc.hInstance = hInstance_;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = darkFrameBrush;

    if (!RegisterClassExA(&wc))
    {
        throw UiInitializationException("No se pudo registrar la clase de ventana.");
    }

    WNDCLASSEXA managerClass{};
    managerClass.cbSize = sizeof(WNDCLASSEXA);
    managerClass.lpfnWndProc = UI::PluginManagerProc;
    managerClass.hInstance = hInstance_;
    managerClass.lpszClassName = kPluginManagerClassName;
    managerClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    managerClass.hbrBackground = darkPanelBrush;

    if (!RegisterClassExA(&managerClass))
    {
        throw UiInitializationException("No se pudo registrar la clase del plugin manager.");
    }

    WNDCLASSEXA surfaceClass{};
    surfaceClass.cbSize = sizeof(WNDCLASSEXA);
    surfaceClass.lpfnWndProc = UI::SurfaceProc;
    surfaceClass.hInstance = hInstance_;
    surfaceClass.lpszClassName = kSurfaceClassName;
    surfaceClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    surfaceClass.hbrBackground = darkPanelBrush;

    if (!RegisterClassExA(&surfaceClass))
    {
        throw UiInitializationException("No se pudo registrar la clase de superficies custom.");
    }

    WNDCLASSEXA detachedPaneClass{};
    detachedPaneClass.cbSize = sizeof(WNDCLASSEXA);
    detachedPaneClass.lpfnWndProc = UI::DetachedPaneProc;
    detachedPaneClass.hInstance = hInstance_;
    detachedPaneClass.lpszClassName = kDetachedPaneClassName;
    detachedPaneClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    detachedPaneClass.hbrBackground = darkPanelBrush;

    if (!RegisterClassExA(&detachedPaneClass))
    {
        throw UiInitializationException("No se pudo registrar la clase de paneles desacoplados.");
    }
}

void UI::createMainWindow()
{
    hwnd_ = CreateWindowExA(
        0,
        kWindowClassName,
        kWindowTitleBase,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1520, 930,
        nullptr,
        nullptr,
        hInstance_,
        this);

    if (hwnd_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear la ventana principal.");
    }
}

void UI::createMainMenu()
{
    mainMenu_ = CreateMenu();
    if (mainMenu_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear el menu principal.");
    }

    fileMenu_ = CreatePopupMenu();
    editMenu_ = CreatePopupMenu();
    addMenu_ = CreatePopupMenu();
    patternsMenu_ = CreatePopupMenu();
    viewMenu_ = CreatePopupMenu();
    optionsMenu_ = CreatePopupMenu();
    toolsMenu_ = CreatePopupMenu();
    helpMenu_ = CreatePopupMenu();

    if (fileMenu_ == nullptr || editMenu_ == nullptr || addMenu_ == nullptr || patternsMenu_ == nullptr ||
        viewMenu_ == nullptr || optionsMenu_ == nullptr || toolsMenu_ == nullptr || helpMenu_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno de los submenus.");
    }

    AppendMenuA(fileMenu_, MF_STRING, IdMenuFileNew, "New Project");
    AppendMenuA(fileMenu_, MF_STRING, IdMenuFileOpen, "Open Project");
    AppendMenuA(fileMenu_, MF_STRING, IdMenuFileSave, "Save Project");
    AppendMenuA(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fileMenu_, MF_STRING, IdMenuFileExit, "Exit");

    AppendMenuA(editMenu_, MF_STRING, IdMenuEditUndo, "Undo");
    AppendMenuA(editMenu_, MF_STRING, IdMenuEditRedo, "Redo");

    AppendMenuA(addMenu_, MF_STRING, IdMenuAddTrack, "Add Track");
    AppendMenuA(addMenu_, MF_STRING, IdMenuAddBus, "Add Bus");
    AppendMenuA(addMenu_, MF_STRING, IdMenuAddClip, "Add Clip");

    AppendMenuA(patternsMenu_, MF_STRING, IdMenuPatternsPrev, "Previous Pattern");
    AppendMenuA(patternsMenu_, MF_STRING, IdMenuPatternsNext, "Next Pattern");

    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewBrowser, "Browser\tAlt+F8");
    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewChannelRack, "Channel Rack\tF6");
    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewPianoRoll, "Piano Roll\tF7");
    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewPlaylist, "Playlist\tF5");
    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewMixer, "Mixer\tF9");
    AppendMenuA(viewMenu_, MF_STRING, IdMenuViewPlugin, "Plugin Window");

    AppendMenuA(optionsMenu_, MF_STRING, IdMenuOptionsAutomation, "Toggle Automation");
    AppendMenuA(optionsMenu_, MF_STRING, IdMenuOptionsPdc, "Toggle PDC");
    AppendMenuA(optionsMenu_, MF_STRING, IdMenuOptionsAnticipative, "Toggle Anticipative");
    AppendMenuA(optionsMenu_, MF_STRING, IdMenuOptionsPatSong, "Toggle Pattern / Song");
    AppendMenuA(optionsMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(optionsMenu_, MF_STRING, IdMenuOptionsManagePlugins, "Manage Plugins");

    AppendMenuA(toolsMenu_, MF_STRING, IdMenuToolsStartEngine, "Start Engine");
    AppendMenuA(toolsMenu_, MF_STRING, IdMenuToolsStopEngine, "Stop Engine");
    AppendMenuA(toolsMenu_, MF_STRING, IdMenuToolsRebuildGraph, "Rebuild Graph");
    AppendMenuA(toolsMenu_, MF_STRING, IdMenuToolsRenderOffline, "Offline Render");

    AppendMenuA(helpMenu_, MF_STRING, IdMenuHelpAbout, "About");

    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu_), "File");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu_), "Edit");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(addMenu_), "Add");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(patternsMenu_), "Patterns");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu_), "View");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(optionsMenu_), "Options");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(toolsMenu_), "Tools");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu_), "Help");

    SetMenu(hwnd_, nullptr);
}

void UI::createControls()
{
    const DWORD buttonStyle = WS_VISIBLE | WS_CHILD | BS_OWNERDRAW;
    const DWORD checkboxStyle = WS_VISIBLE | WS_CHILD | BS_OWNERDRAW;
    const DWORD staticStyle = WS_VISIBLE | WS_CHILD | SS_LEFT;
    const WORD menuButtonIds[8] = {
        IdButtonMenuFile,
        IdButtonMenuEdit,
        IdButtonMenuAdd,
        IdButtonMenuPatterns,
        IdButtonMenuView,
        IdButtonMenuOptions,
        IdButtonMenuTools,
        IdButtonMenuHelp};
    const char* menuButtonLabels[8] = {
        "File",
        "Edit",
        "Add",
        "Patterns",
        "View",
        "Options",
        "Tools",
        "Help"};

    for (int index = 0; index < 8; ++index)
    {
        mainMenuButtons_[static_cast<std::size_t>(index)] =
            CreateWindowA(
                "BUTTON",
                menuButtonLabels[index],
                buttonStyle,
                0,
                0,
                0,
                0,
                hwnd_,
                reinterpret_cast<HMENU>(menuButtonIds[index]),
                hInstance_,
                nullptr);
    }

    engineStartButton_ = CreateWindowA("BUTTON", "Engine", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonEngineStart), hInstance_, nullptr);
    engineStopButton_ = CreateWindowA("BUTTON", "Stop Eng", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonEngineStop), hInstance_, nullptr);
    playButton_ = CreateWindowA("BUTTON", "Play", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlay), hInstance_, nullptr);
    stopTransportButton_ = CreateWindowA("BUTTON", "Stop", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonStopTransport), hInstance_, nullptr);
    recordButton_ = CreateWindowA("BUTTON", "Rec", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonRecord), hInstance_, nullptr);
    patSongButton_ = CreateWindowA("BUTTON", "Song", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPatSong), hInstance_, nullptr);
    tempoDownButton_ = CreateWindowA("BUTTON", "-", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonTempoDown), hInstance_, nullptr);
    tempoUpButton_ = CreateWindowA("BUTTON", "+", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonTempoUp), hInstance_, nullptr);
    patternPrevButton_ = CreateWindowA("BUTTON", "<", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPatternPrev), hInstance_, nullptr);
    patternNextButton_ = CreateWindowA("BUTTON", ">", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPatternNext), hInstance_, nullptr);
    snapPrevButton_ = CreateWindowA("BUTTON", "<", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonSnapPrev), hInstance_, nullptr);
    snapNextButton_ = CreateWindowA("BUTTON", ">", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonSnapNext), hInstance_, nullptr);
    browserButton_ = CreateWindowA("BUTTON", "Browser", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonBrowser), hInstance_, nullptr);
    channelRackButton_ = CreateWindowA("BUTTON", "Rack", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonChannelRack), hInstance_, nullptr);
    pianoRollButton_ = CreateWindowA("BUTTON", "Piano", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPianoRoll), hInstance_, nullptr);
    playlistButton_ = CreateWindowA("BUTTON", "Playlist", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlaylist), hInstance_, nullptr);
    mixerButton_ = CreateWindowA("BUTTON", "Mixer", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMixer), hInstance_, nullptr);
    pluginButton_ = CreateWindowA("BUTTON", "Plugin", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlugin), hInstance_, nullptr);
    addTrackButton_ = CreateWindowA("BUTTON", "Add Track", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonAddTrack), hInstance_, nullptr);
    addBusButton_ = CreateWindowA("BUTTON", "Add Bus", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonAddBus), hInstance_, nullptr);
    addClipButton_ = CreateWindowA("BUTTON", "Add Clip", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonAddClip), hInstance_, nullptr);
    undoButton_ = CreateWindowA("BUTTON", "Undo", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonUndo), hInstance_, nullptr);
    redoButton_ = CreateWindowA("BUTTON", "Redo", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonRedo), hInstance_, nullptr);
    saveProjectButton_ = CreateWindowA("BUTTON", "Save", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonSaveProject), hInstance_, nullptr);
    loadProjectButton_ = CreateWindowA("BUTTON", "Load", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonLoadProject), hInstance_, nullptr);
    prevTrackButton_ = CreateWindowA("BUTTON", "Prev Ch", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPrevTrack), hInstance_, nullptr);
    nextTrackButton_ = CreateWindowA("BUTTON", "Next Ch", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonNextTrack), hInstance_, nullptr);
    rebuildGraphButton_ = CreateWindowA("BUTTON", "Graph", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonRebuildGraph), hInstance_, nullptr);
    renderOfflineButton_ = CreateWindowA("BUTTON", "Render", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonRenderOffline), hInstance_, nullptr);
    managePluginsButton_ = CreateWindowA("BUTTON", "Plugins", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonManagePlugins), hInstance_, nullptr);

    automationCheckbox_ = CreateWindowA("BUTTON", "Automation", checkboxStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdCheckboxAutomation), hInstance_, nullptr);
    pdcCheckbox_ = CreateWindowA("BUTTON", "PDC", checkboxStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdCheckboxPdc), hInstance_, nullptr);
    anticipativeCheckbox_ = CreateWindowA("BUTTON", "Anticipative", checkboxStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdCheckboxAnticipative), hInstance_, nullptr);

    statusLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelStatus), hInstance_, nullptr);
    tempoLabel_ = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelTempo), hInstance_, nullptr);
    patternLabel_ = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_CENTER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPattern), hInstance_, nullptr);
    snapLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelSnap), hInstance_, nullptr);
    systemLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelSystem), hInstance_, nullptr);
    hintsLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelHints), hInstance_, nullptr);
    projectSummaryLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelProjectSummary), hInstance_, nullptr);
    documentLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelDocument), hInstance_, nullptr);
    selectionLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelSelection), hInstance_, nullptr);

    browserMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuBrowser), hInstance_, nullptr);
    browserTabPrevButton_ = CreateWindowA("BUTTON", "<", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonBrowserTabPrev), hInstance_, nullptr);
    browserTabNextButton_ = CreateWindowA("BUTTON", ">", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonBrowserTabNext), hInstance_, nullptr);
    browserHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelBrowserHeader), hInstance_, nullptr);
    browserPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelBrowser), hInstance_, this);
    channelRackMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuChannelRack), hInstance_, nullptr);
    channelHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelChannelHeader), hInstance_, nullptr);
    channelRackPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelChannelRack), hInstance_, this);
    stepSequencerPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelStepSequencer), hInstance_, this);
    pianoRollMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPianoRoll), hInstance_, nullptr);
    pianoZoomPrevButton_ = CreateWindowA("BUTTON", "-", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPianoZoomPrev), hInstance_, nullptr);
    pianoZoomNextButton_ = CreateWindowA("BUTTON", "+", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPianoZoomNext), hInstance_, nullptr);
    pianoToolPrevButton_ = CreateWindowA("BUTTON", "<", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPianoToolPrev), hInstance_, nullptr);
    pianoToolNextButton_ = CreateWindowA("BUTTON", ">", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPianoToolNext), hInstance_, nullptr);
    pianoHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPianoHeader), hInstance_, nullptr);
    pianoRollPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPianoRoll), hInstance_, this);
    playlistMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPlaylist), hInstance_, nullptr);
    playlistZoomPrevButton_ = CreateWindowA("BUTTON", "-", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlaylistZoomPrev), hInstance_, nullptr);
    playlistZoomNextButton_ = CreateWindowA("BUTTON", "+", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlaylistZoomNext), hInstance_, nullptr);
    playlistToolPrevButton_ = CreateWindowA("BUTTON", "<", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlaylistToolPrev), hInstance_, nullptr);
    playlistToolNextButton_ = CreateWindowA("BUTTON", ">", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonPlaylistToolNext), hInstance_, nullptr);
    playlistHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPlaylistHeader), hInstance_, nullptr);
    playlistPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPlaylist), hInstance_, this);
    mixerMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuMixer), hInstance_, nullptr);
    mixerHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelMixerHeader), hInstance_, nullptr);
    mixerPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelMixer), hInstance_, this);
    pluginMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPlugin), hInstance_, nullptr);
    pluginHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPluginHeader), hInstance_, nullptr);
    pluginPanel_ = CreateWindowA(kSurfaceClassName, "", WS_VISIBLE | WS_CHILD | WS_BORDER, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPlugin), hInstance_, this);
    contextLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelContext), hInstance_, nullptr);

    const bool missingMenuButton =
        std::any_of(
            mainMenuButtons_.begin(),
            mainMenuButtons_.end(),
            [](HWND button) { return button == nullptr; });

    if (missingMenuButton || engineStartButton_ == nullptr || engineStopButton_ == nullptr || playButton_ == nullptr ||
        stopTransportButton_ == nullptr || recordButton_ == nullptr || patSongButton_ == nullptr ||
        tempoDownButton_ == nullptr || tempoUpButton_ == nullptr || patternPrevButton_ == nullptr ||
        patternNextButton_ == nullptr || snapPrevButton_ == nullptr || snapNextButton_ == nullptr ||
        browserButton_ == nullptr || channelRackButton_ == nullptr || pianoRollButton_ == nullptr ||
        playlistButton_ == nullptr || mixerButton_ == nullptr || pluginButton_ == nullptr ||
        addTrackButton_ == nullptr || addBusButton_ == nullptr || addClipButton_ == nullptr ||
        undoButton_ == nullptr || redoButton_ == nullptr || saveProjectButton_ == nullptr ||
        loadProjectButton_ == nullptr || prevTrackButton_ == nullptr || nextTrackButton_ == nullptr ||
        rebuildGraphButton_ == nullptr || renderOfflineButton_ == nullptr || managePluginsButton_ == nullptr || automationCheckbox_ == nullptr ||
        pdcCheckbox_ == nullptr || anticipativeCheckbox_ == nullptr || statusLabel_ == nullptr ||
        tempoLabel_ == nullptr || patternLabel_ == nullptr || snapLabel_ == nullptr ||
        systemLabel_ == nullptr || hintsLabel_ == nullptr || projectSummaryLabel_ == nullptr ||
        documentLabel_ == nullptr || selectionLabel_ == nullptr || browserMenuButton_ == nullptr ||
        browserTabPrevButton_ == nullptr || browserTabNextButton_ == nullptr || browserHeaderLabel_ == nullptr ||
        browserPanel_ == nullptr || channelRackMenuButton_ == nullptr || channelHeaderLabel_ == nullptr ||
        channelRackPanel_ == nullptr || stepSequencerPanel_ == nullptr || pianoRollMenuButton_ == nullptr ||
        pianoZoomPrevButton_ == nullptr || pianoZoomNextButton_ == nullptr || pianoToolPrevButton_ == nullptr ||
        pianoToolNextButton_ == nullptr || pianoHeaderLabel_ == nullptr || pianoRollPanel_ == nullptr ||
        playlistMenuButton_ == nullptr || playlistZoomPrevButton_ == nullptr || playlistZoomNextButton_ == nullptr ||
        playlistToolPrevButton_ == nullptr || playlistToolNextButton_ == nullptr || playlistHeaderLabel_ == nullptr ||
        playlistPanel_ == nullptr || mixerMenuButton_ == nullptr || mixerHeaderLabel_ == nullptr ||
        mixerPanel_ == nullptr || pluginMenuButton_ == nullptr || pluginHeaderLabel_ == nullptr ||
        pluginPanel_ == nullptr || contextLabel_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno o mas controles de la interfaz.");
    }

    static HFONT numericFont = CreateFontA(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    static HFONT patternFont = CreateFontA(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    SendMessageA(tempoLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(numericFont), TRUE);
    SendMessageA(patternLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(patternFont), TRUE);

    layoutControls();
}

void UI::layoutControls()
{
    if (hwnd_ == nullptr)
    {
        return;
    }

    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);

    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    const int menuStripHeight = 28;
    const int topPanelHeight = 104;
    const int topPanelY = menuStripHeight;
    const int y = topPanelY + topPanelHeight + kGap;

    const int menuButtonWidths[8] = {44, 46, 42, 78, 48, 64, 52, 46};

    int menuX = 8;
    for (int index = 0; index < 8; ++index)
    {
        ShowWindow(mainMenuButtons_[static_cast<std::size_t>(index)], SW_SHOW);
        MoveWindow(mainMenuButtons_[static_cast<std::size_t>(index)], menuX, 1, menuButtonWidths[index], menuStripHeight - 2, TRUE);
        menuX += menuButtonWidths[index] + 2;
    }

    const int timePanelX = 22;
    const int timePanelWidth = 210;
    const int spectrumPanelX = timePanelX + timePanelWidth + 14;
    const int spectrumPanelWidth = 140;
    const int transportX = spectrumPanelX + spectrumPanelWidth + 28;
    const int transportY = topPanelY + 18;
    const int patternClusterWidth = 154;
    const int patternClusterX =
        std::min(
            width - kOuterPadding - patternClusterWidth - 20,
            std::max(transportX + 320, width - 460));

    MoveWindow(patSongButton_, transportX, transportY + 2, 48, 56, TRUE);
    MoveWindow(playButton_, transportX + 58, transportY + 6, 44, 36, TRUE);
    MoveWindow(stopTransportButton_, transportX + 104, transportY + 6, 44, 36, TRUE);
    MoveWindow(recordButton_, transportX + 156, transportY + 7, 32, 32, TRUE);
    MoveWindow(tempoLabel_, transportX + 198, transportY + 4, 90, 38, TRUE);

    MoveWindow(patternPrevButton_, patternClusterX, transportY + 2, 32, 40, TRUE);
    MoveWindow(patternLabel_, patternClusterX + 34, transportY + 2, 56, 40, TRUE);
    MoveWindow(patternNextButton_, patternClusterX + 92, transportY + 2, 32, 40, TRUE);

    MoveWindow(pianoRollButton_, patternClusterX, transportY + 50, 46, 42, TRUE);
    MoveWindow(channelRackButton_, patternClusterX + 48, transportY + 50, 50, 42, TRUE);
    MoveWindow(mixerButton_, patternClusterX + 100, transportY + 50, 44, 42, TRUE);

    ShowWindow(patSongButton_, SW_SHOW);
    ShowWindow(playButton_, SW_SHOW);
    ShowWindow(stopTransportButton_, SW_SHOW);
    ShowWindow(recordButton_, SW_SHOW);
    ShowWindow(tempoLabel_, SW_SHOW);
    ShowWindow(patternPrevButton_, SW_SHOW);
    ShowWindow(patternLabel_, SW_SHOW);
    ShowWindow(patternNextButton_, SW_SHOW);
    ShowWindow(pianoRollButton_, SW_SHOW);
    ShowWindow(channelRackButton_, SW_SHOW);
    ShowWindow(mixerButton_, SW_SHOW);

    ShowWindow(engineStartButton_, SW_HIDE);
    ShowWindow(engineStopButton_, SW_HIDE);
    ShowWindow(tempoDownButton_, SW_HIDE);
    ShowWindow(tempoUpButton_, SW_HIDE);
    ShowWindow(snapPrevButton_, SW_HIDE);
    ShowWindow(snapNextButton_, SW_HIDE);
    ShowWindow(browserButton_, SW_HIDE);
    ShowWindow(playlistButton_, SW_HIDE);
    ShowWindow(pluginButton_, SW_HIDE);
    ShowWindow(addTrackButton_, SW_HIDE);
    ShowWindow(addBusButton_, SW_HIDE);
    ShowWindow(addClipButton_, SW_HIDE);
    ShowWindow(undoButton_, SW_HIDE);
    ShowWindow(redoButton_, SW_HIDE);
    ShowWindow(saveProjectButton_, SW_HIDE);
    ShowWindow(loadProjectButton_, SW_HIDE);
    ShowWindow(prevTrackButton_, SW_HIDE);
    ShowWindow(nextTrackButton_, SW_HIDE);
    ShowWindow(rebuildGraphButton_, SW_HIDE);
    ShowWindow(renderOfflineButton_, SW_HIDE);
    ShowWindow(managePluginsButton_, SW_HIDE);
    ShowWindow(automationCheckbox_, SW_HIDE);
    ShowWindow(pdcCheckbox_, SW_HIDE);
    ShowWindow(anticipativeCheckbox_, SW_HIDE);
    ShowWindow(statusLabel_, SW_HIDE);
    ShowWindow(snapLabel_, SW_HIDE);
    ShowWindow(systemLabel_, SW_HIDE);
    ShowWindow(projectSummaryLabel_, SW_HIDE);
    ShowWindow(documentLabel_, SW_HIDE);
    ShowWindow(selectionLabel_, SW_HIDE);
    ShowWindow(contextLabel_, SW_HIDE);

    const int footerHeight = 18;
    const int browserAreaWidth = workspace_.browserVisible ? kBrowserWidth : 0;
    const int workspaceX = kOuterPadding + browserAreaWidth + (workspace_.browserVisible ? kGap : 0);
    const int workspaceWidth = std::max(180, width - workspaceX - kOuterPadding);
    const int workspaceHeight = std::max(180, height - y - kOuterPadding - footerHeight - 4);

    ShowWindow(browserMenuButton_, SW_HIDE);
    ShowWindow(browserTabPrevButton_, SW_HIDE);
    ShowWindow(browserTabNextButton_, SW_HIDE);
    ShowWindow(browserHeaderLabel_, SW_HIDE);
    ShowWindow(channelRackMenuButton_, SW_HIDE);
    ShowWindow(channelHeaderLabel_, SW_HIDE);
    ShowWindow(playlistMenuButton_, SW_HIDE);
    ShowWindow(playlistZoomPrevButton_, SW_HIDE);
    ShowWindow(playlistZoomNextButton_, SW_HIDE);
    ShowWindow(playlistToolPrevButton_, SW_HIDE);
    ShowWindow(playlistToolNextButton_, SW_HIDE);
    ShowWindow(playlistHeaderLabel_, SW_HIDE);
    ShowWindow(mixerMenuButton_, SW_HIDE);
    ShowWindow(mixerHeaderLabel_, SW_HIDE);
    ShowWindow(pianoRollMenuButton_, SW_HIDE);
    ShowWindow(pianoZoomPrevButton_, SW_HIDE);
    ShowWindow(pianoZoomNextButton_, SW_HIDE);
    ShowWindow(pianoToolPrevButton_, SW_HIDE);
    ShowWindow(pianoToolNextButton_, SW_HIDE);
    ShowWindow(pianoHeaderLabel_, SW_HIDE);
    ShowWindow(pluginMenuButton_, SW_HIDE);
    ShowWindow(pluginHeaderLabel_, SW_HIDE);

    updateDockingModel();

    ShowWindow(browserPanel_, workspace_.browserVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.browserVisible)
    {
        MoveWindow(browserPanel_, kOuterPadding, y, browserAreaWidth, workspaceHeight, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::Browser))
        {
            pane->bounds = RECT{kOuterPadding, y, kOuterPadding + browserAreaWidth, y + workspaceHeight};
        }
    }

    workspace_.playlistVisible = true;
    ShowWindow(playlistPanel_, SW_SHOW);

    const int lowerDockCount = (workspace_.pianoRollVisible ? 1 : 0) + (workspace_.pluginVisible ? 1 : 0);
    const int lowerDockHeight = lowerDockCount == 0 ? 0 : (lowerDockCount == 1 ? 190 : 220);
    const int playlistHeight =
        std::max(160, workspaceHeight - (lowerDockHeight > 0 ? lowerDockHeight + kGap : 0));
    MoveWindow(playlistPanel_, workspaceX, y, workspaceWidth, playlistHeight, TRUE);

    if (DockedPaneState* pane = findDockedPane(WorkspacePane::Playlist))
    {
        pane->bounds = RECT{workspaceX, y, workspaceX + workspaceWidth, y + playlistHeight};
    }

    const int lowerDockY = y + playlistHeight + (lowerDockHeight > 0 ? kGap : 0);
    if (workspace_.pianoRollVisible || workspace_.pluginVisible)
    {
        if (workspace_.pianoRollVisible && workspace_.pluginVisible)
        {
            const int pianoWidth = (workspaceWidth * 58) / 100;
            ShowWindow(pianoRollPanel_, SW_SHOW);
            ShowWindow(pluginPanel_, SW_SHOW);
            MoveWindow(pianoRollPanel_, workspaceX, lowerDockY, pianoWidth, lowerDockHeight, TRUE);
            MoveWindow(pluginPanel_, workspaceX + pianoWidth + kGap, lowerDockY, workspaceWidth - pianoWidth - kGap, lowerDockHeight, TRUE);
        }
        else if (workspace_.pianoRollVisible)
        {
            ShowWindow(pianoRollPanel_, SW_SHOW);
            ShowWindow(pluginPanel_, SW_HIDE);
            MoveWindow(pianoRollPanel_, workspaceX, lowerDockY, workspaceWidth, lowerDockHeight, TRUE);
        }
        else
        {
            ShowWindow(pianoRollPanel_, SW_HIDE);
            ShowWindow(pluginPanel_, SW_SHOW);
            MoveWindow(pluginPanel_, workspaceX, lowerDockY, workspaceWidth, lowerDockHeight, TRUE);
        }
    }
    else
    {
        ShowWindow(pianoRollPanel_, SW_HIDE);
        ShowWindow(pluginPanel_, SW_HIDE);
    }

    if (DockedPaneState* pane = findDockedPane(WorkspacePane::PianoRoll))
    {
        pane->bounds = RECT{
            workspaceX,
            lowerDockY,
            workspaceX + (workspace_.pluginVisible && workspace_.pianoRollVisible ? (workspaceWidth * 58) / 100 : workspaceWidth),
            lowerDockY + lowerDockHeight};
    }
    if (DockedPaneState* pane = findDockedPane(WorkspacePane::Plugin))
    {
        pane->bounds = RECT{
            workspace_.pluginVisible && workspace_.pianoRollVisible ? workspaceX + ((workspaceWidth * 58) / 100) + kGap : workspaceX,
            lowerDockY,
            workspaceX + workspaceWidth,
            lowerDockY + lowerDockHeight};
    }

    ShowWindow(channelRackPanel_, SW_HIDE);
    ShowWindow(stepSequencerPanel_, SW_HIDE);
    ShowWindow(mixerPanel_, SW_HIDE);
    ShowWindow(contextLabel_, SW_HIDE);
    ShowWindow(hintsLabel_, SW_HIDE);

    ensureDetachedWindows();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void UI::ensureDetachedWindows()
{
    const auto ensureDetachedPane =
        [this](WorkspacePane paneId, int defaultWidth, int defaultHeight)
    {
        DockedPaneState* pane = findDockedPane(paneId);
        if (pane == nullptr || !pane->detached)
        {
            return;
        }

        if (pane->windowHandle == nullptr)
        {
            const int x = pane->bounds.right > pane->bounds.left ? pane->bounds.left : CW_USEDEFAULT;
            const int y = pane->bounds.bottom > pane->bounds.top ? pane->bounds.top : CW_USEDEFAULT;
            const int width = pane->bounds.right > pane->bounds.left ? (pane->bounds.right - pane->bounds.left) : defaultWidth;
            const int height = pane->bounds.bottom > pane->bounds.top ? (pane->bounds.bottom - pane->bounds.top) : defaultHeight;

            pane->windowHandle = CreateWindowExA(
                WS_EX_TOOLWINDOW,
                kDetachedPaneClassName,
                pane->title.c_str(),
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                x,
                y,
                width,
                height,
                hwnd_,
                nullptr,
                hInstance_,
                this);

            if (pane->windowHandle == nullptr)
            {
                throw UiInitializationException("No se pudo crear una ventana desacoplada.");
            }
        }

        if (paneId == WorkspacePane::ChannelRack)
        {
            SetParent(channelRackPanel_, pane->windowHandle);
            SetParent(stepSequencerPanel_, pane->windowHandle);
            ShowWindow(channelRackPanel_, pane->visible ? SW_SHOW : SW_HIDE);
            ShowWindow(stepSequencerPanel_, pane->visible ? SW_SHOW : SW_HIDE);
        }
        else if (paneId == WorkspacePane::Mixer)
        {
            SetParent(mixerPanel_, pane->windowHandle);
            ShowWindow(mixerPanel_, pane->visible ? SW_SHOW : SW_HIDE);
        }

        SetWindowTextA(pane->windowHandle, pane->title.c_str());
        ShowWindow(pane->windowHandle, pane->visible ? SW_SHOW : SW_HIDE);
        if (pane->visible)
        {
            layoutDetachedPaneWindow(*pane);
        }
    };

    ensureDetachedPane(WorkspacePane::ChannelRack, 560, 540);
    ensureDetachedPane(WorkspacePane::Mixer, 1080, 420);
}

void UI::updateDockingModel()
{
    for (auto& pane : dockedPanes_)
    {
        pane.visible = isPaneVisible(pane.pane);
        pane.title = paneTitle(pane.pane);
        if (pane.windowHandle != nullptr)
        {
            SetWindowTextA(pane.windowHandle, pane.title.c_str());
        }
    }
}

void UI::ensurePluginManagerWindow()
{
    if (pluginManagerHwnd_ != nullptr)
    {
        return;
    }

    pluginManagerHwnd_ = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        kPluginManagerClassName,
        "Manage Plugins",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        980, 700,
        hwnd_,
        nullptr,
        hInstance_,
        this);

    if (pluginManagerHwnd_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear la ventana Manage Plugins.");
    }

    const DWORD buttonStyle = WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON;
    const DWORD staticStyle = WS_VISIBLE | WS_CHILD | SS_LEFT;

    pluginManagerHeaderLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdLabelPluginManagerHeader), hInstance_, nullptr);
    pluginManagerSearchEdit_ = CreateWindowA("EDIT", "", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(4001), hInstance_, nullptr);
    pluginManagerListBox_ = CreateWindowA("LISTBOX", "", WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(4002), hInstance_, nullptr);
    pluginManagerDetailLabel_ = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | WS_BORDER | SS_LEFT, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdLabelPluginManagerDetail), hInstance_, nullptr);
    pluginManagerPathsLabel_ = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | WS_BORDER | SS_LEFT, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdLabelPluginManagerPaths), hInstance_, nullptr);
    pluginManagerRescanButton_ = CreateWindowA("BUTTON", "Rescan", buttonStyle, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdButtonPluginRescan), hInstance_, nullptr);
    pluginManagerAddStubButton_ = CreateWindowA("BUTTON", "Add Stub", buttonStyle, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdButtonPluginAddStub), hInstance_, nullptr);
    pluginManagerToggleSandboxButton_ = CreateWindowA("BUTTON", "Toggle Sandbox", buttonStyle, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdButtonPluginToggleSandbox), hInstance_, nullptr);
    pluginManagerCloseButton_ = CreateWindowA("BUTTON", "Close", buttonStyle, 0, 0, 0, 0, pluginManagerHwnd_, reinterpret_cast<HMENU>(IdButtonPluginClose), hInstance_, nullptr);

    layoutPluginManagerWindow();
}

void UI::layoutPluginManagerWindow()
{
    if (pluginManagerHwnd_ == nullptr)
    {
        return;
    }

    RECT rect{};
    GetClientRect(pluginManagerHwnd_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int leftWidth = 320;

    MoveWindow(pluginManagerHeaderLabel_, 12, 12, width - 24, 22, TRUE);
    MoveWindow(pluginManagerSearchEdit_, 12, 42, leftWidth, 24, TRUE);
    MoveWindow(pluginManagerListBox_, 12, 74, leftWidth, height - 150, TRUE);
    MoveWindow(pluginManagerDetailLabel_, 344, 42, width - 356, 280, TRUE);
    MoveWindow(pluginManagerPathsLabel_, 344, 330, width - 356, height - 418, TRUE);
    MoveWindow(pluginManagerRescanButton_, 344, height - 74, 90, 28, TRUE);
    MoveWindow(pluginManagerAddStubButton_, 440, height - 74, 90, 28, TRUE);
    MoveWindow(pluginManagerToggleSandboxButton_, 536, height - 74, 130, 28, TRUE);
    MoveWindow(pluginManagerCloseButton_, width - 102, height - 74, 90, 28, TRUE);
}

void UI::refreshPluginManagerState()
{
    pluginManagerState_.loadedPlugins.clear();
    const auto descriptors = engine_.getLoadedPluginDescriptors();
    for (const auto& descriptor : descriptors)
    {
        VisiblePluginDescriptor visible{};
        visible.name = descriptor.name;
        visible.vendor = descriptor.vendor;
        visible.format = descriptor.format;
        visible.latencySamples = descriptor.reportedLatencySamples;
        visible.supportsDoublePrecision = descriptor.supportsDoublePrecision;
        visible.supportsSampleAccurateAutomation = descriptor.supportsSampleAccurateAutomation;

        switch (descriptor.processMode)
        {
        case AudioEngine::PluginProcessMode::InProcess:
            visible.processModeLabel = "Inline";
            break;
        case AudioEngine::PluginProcessMode::GroupSandbox:
            visible.processModeLabel = "Group Sandbox";
            break;
        case AudioEngine::PluginProcessMode::PerPlugin:
            visible.processModeLabel = "Per-Plugin";
            break;
        }

        pluginManagerState_.loadedPlugins.push_back(std::move(visible));
    }

    if (pluginManagerState_.searchPaths.empty())
    {
        pluginManagerState_.searchPaths = {
            "VST3: C:\\Program Files\\Common Files\\VST3",
            "CLAP: C:\\Program Files\\Common Files\\CLAP",
            "User: C:\\Users\\USUARIO\\Documents\\AudioPlugins",
            "Project: .\\Presets"};
    }

    if (pluginManagerState_.favorites.empty())
    {
        pluginManagerState_.favorites = {"Sampler", "EQ", "Compressor", "Reverb"};
    }

    if (pluginManagerState_.blacklist.empty())
    {
        pluginManagerState_.blacklist = {"BrokenLegacyFx.dll", "CrashySynth.vst3"};
    }

    applyPluginSearchFilter();
}

void UI::updatePluginManagerControls()
{
    if (pluginManagerHwnd_ == nullptr)
    {
        return;
    }

    std::ostringstream header;
    header
        << "Manage Plugins | Loaded " << pluginManagerState_.loadedPlugins.size()
        << " | Visible " << pluginManagerState_.filteredPlugins.size()
        << " | Sandbox " << boolLabel(visibleState_.pluginSandboxEnabled, "On", "Off")
        << " | Host " << boolLabel(visibleState_.pluginHostEnabled, "On", "Off");
    setStaticText(pluginManagerHeaderLabel_, header.str());

    SendMessageA(pluginManagerListBox_, LB_RESETCONTENT, 0, 0);
    for (const auto& plugin : pluginManagerState_.filteredPlugins)
    {
        const std::string line = plugin.name + " [" + plugin.format + "] - " + plugin.vendor;
        SendMessageA(pluginManagerListBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }

    if (!pluginManagerState_.filteredPlugins.empty())
    {
        pluginManagerState_.selectedIndex = std::min(pluginManagerState_.selectedIndex, pluginManagerState_.filteredPlugins.size() - 1);
        SendMessageA(pluginManagerListBox_, LB_SETCURSEL, pluginManagerState_.selectedIndex, 0);
    }

    setStaticText(pluginManagerDetailLabel_, buildPluginManagerDetailText());
    setStaticText(pluginManagerPathsLabel_, buildPluginManagerTableText() + "\r\n\r\n" + buildPluginManagerPathText());
    SetWindowTextA(pluginManagerToggleSandboxButton_, visibleState_.pluginSandboxEnabled ? "Sandbox On" : "Sandbox Off");
}

void UI::showPluginManagerWindow()
{
    ensurePluginManagerWindow();
    pluginManagerState_.visible = true;
    refreshPluginManagerState();
    updatePluginManagerControls();
    ShowWindow(pluginManagerHwnd_, SW_SHOW);
    SetForegroundWindow(pluginManagerHwnd_);
}

void UI::closePluginManagerWindow()
{
    pluginManagerState_.visible = false;
    if (pluginManagerHwnd_ != nullptr)
    {
        ShowWindow(pluginManagerHwnd_, SW_HIDE);
    }
}

void UI::invalidateSurface(HWND surface)
{
    if (surface != nullptr)
    {
        InvalidateRect(surface, nullptr, TRUE);
    }
}

void UI::invalidateAllSurfaces()
{
    invalidateSurface(browserPanel_);
    invalidateSurface(channelRackPanel_);
    invalidateSurface(stepSequencerPanel_);
    invalidateSurface(pianoRollPanel_);
    invalidateSurface(playlistPanel_);
    invalidateSurface(mixerPanel_);
    invalidateSurface(pluginPanel_);
}

void UI::destroyDetachedWindows()
{
    for (auto& pane : dockedPanes_)
    {
        if (pane.windowHandle != nullptr)
        {
            DestroyWindow(pane.windowHandle);
            pane.windowHandle = nullptr;
        }
    }
}

void UI::layoutDetachedPaneWindow(DockedPaneState& pane)
{
    if (pane.windowHandle == nullptr)
    {
        return;
    }

    RECT clientRect{};
    GetClientRect(pane.windowHandle, &clientRect);
    const int width = std::max(200, static_cast<int>(clientRect.right - clientRect.left));
    const int height = std::max(160, static_cast<int>(clientRect.bottom - clientRect.top));

    if (pane.pane == WorkspacePane::ChannelRack)
    {
        const int topHeight = std::max(180, (height * 54) / 100);
        MoveWindow(channelRackPanel_, 0, 0, width, topHeight, TRUE);
        MoveWindow(stepSequencerPanel_, 0, topHeight + kDetachedPaneGap, width, std::max(120, height - topHeight - kDetachedPaneGap), TRUE);
    }
    else if (pane.pane == WorkspacePane::Mixer)
    {
        MoveWindow(mixerPanel_, 0, 0, width, height, TRUE);
    }

    GetWindowRect(pane.windowHandle, &pane.bounds);
}

void UI::startUiTimer()
{
    if (SetTimer(hwnd_, kUiTimerId, kUiTimerIntervalMs, nullptr) == 0)
    {
        throw UiInitializationException("No se pudo iniciar el timer de UI.");
    }
}

void UI::stopUiTimer()
{
    if (hwnd_ != nullptr)
    {
        KillTimer(hwnd_, kUiTimerId);
    }
}

void UI::refreshFromEngineSnapshot()
{
    visibleState_ = buildVisibleEngineState();
    refreshPluginManagerState();

    if (!visibleState_.project.tracks.empty() && selectedTrackIndex_ >= visibleState_.project.tracks.size())
    {
        selectedTrackIndex_ = visibleState_.project.tracks.size() - 1;
        visibleState_ = buildVisibleEngineState();
    }

    workspace_.tempoBpm = std::max(10.0, std::min(522.0, engine_.getTransportInfo().tempoBpm));
    syncWorkspaceModel();

    updateTransportControls();
    updateStatusLabel();
    updateMetricLabels();
    updateProjectLabels();
    updateToggleStates();
    updateWorkspacePanels();
    updateViewButtons();
    updateWindowTitle();
    updatePluginManagerControls();
    invalidateAllSurfaces();
}

UI::VisibleEngineState UI::buildVisibleEngineState() const
{
    VisibleEngineState snapshot{};
    const AudioEngine::EngineSnapshot engineSnapshot = engine_.getUiSnapshot();

    snapshot.engineState = engineSnapshot.engineState;
    snapshot.transportState = engineSnapshot.transport.state;
    snapshot.sampleRate = static_cast<int>(engineSnapshot.device.sampleRate);
    snapshot.blockSize = static_cast<int>(engineSnapshot.device.blockSize);
    snapshot.timelineSeconds = engineSnapshot.transport.timelineSeconds;
    snapshot.transportSamplePosition = engineSnapshot.transport.samplePosition;
    snapshot.xruns = engineSnapshot.metrics.xruns;
    snapshot.deadlineMisses = engineSnapshot.metrics.deadlineMisses;
    snapshot.callbackCount = engineSnapshot.metrics.callbackCount;
    snapshot.recoveryCount = engineSnapshot.metrics.recoveryCount;
    snapshot.cpuLoadApprox = engineSnapshot.metrics.cpuLoadApprox;
    snapshot.averageBlockTimeUs = engineSnapshot.metrics.averageBlockTimeUs;
    snapshot.peakBlockTimeUs = engineSnapshot.metrics.peakBlockTimeUs;
    snapshot.currentLatencySamples = engineSnapshot.metrics.currentLatencySamples;
    snapshot.activeGraphVersion = engineSnapshot.metrics.activeGraphVersion;

    snapshot.anticipativeProcessingEnabled = engineSnapshot.config.enableAnticipativeProcessing;
    snapshot.automationEnabled = engineSnapshot.config.enableSampleAccurateAutomation;
    snapshot.pdcEnabled = engineSnapshot.config.enablePdc;
    snapshot.offlineRenderEnabled = engineSnapshot.config.enableOfflineRender;
    snapshot.pluginHostEnabled = engineSnapshot.config.enablePluginHost;
    snapshot.pluginSandboxEnabled = engineSnapshot.config.enablePluginSandbox;
    snapshot.prefer64BitMix = engineSnapshot.config.prefer64BitInternalMix;
    snapshot.monitoringEnabled = engineSnapshot.transport.monitoringEnabled;
    snapshot.safeMode = engineSnapshot.config.safeMode;

    snapshot.backendName = engineSnapshot.device.backendName;
    snapshot.deviceName = engineSnapshot.device.deviceName;
    snapshot.statusText = engineSnapshot.statusText;
    snapshot.lastErrorMessage = engineSnapshot.lastErrorMessage;

    snapshot.project.projectName = engineSnapshot.project.state.projectName;
    snapshot.project.sessionPath = engineSnapshot.project.state.sessionPath;
    snapshot.project.revision = engineSnapshot.project.state.revision;
    snapshot.project.dirty = engineSnapshot.project.state.dirty;
    snapshot.project.undoDepth = engineSnapshot.project.undoDepth;
    snapshot.project.redoDepth = engineSnapshot.project.redoDepth;

    for (const auto& bus : engineSnapshot.project.state.buses)
    {
        snapshot.project.buses.push_back(VisibleBus{bus.busId, bus.name, bus.inputTrackIds});
    }

    for (const auto& track : engineSnapshot.project.state.tracks)
    {
        VisibleTrack visibleTrack{};
        visibleTrack.trackId = track.trackId;
        visibleTrack.busId = track.busId;
        visibleTrack.name = track.name;
        visibleTrack.armed = track.armed;
        visibleTrack.muted = track.muted;
        visibleTrack.solo = track.solo;

        for (const auto& clip : engineSnapshot.project.state.clips)
        {
            if (clip.trackId != track.trackId)
            {
                continue;
            }

            visibleTrack.clips.push_back(VisibleClip{
                clip.clipId,
                clip.name,
                clip.sourceType == AudioEngine::ClipSourceType::GeneratedTone ? "Pattern" : "Audio",
                clip.startTimeSeconds,
                clip.durationSeconds,
                clip.muted});
        }

        snapshot.project.tracks.push_back(std::move(visibleTrack));
    }

    if (!snapshot.project.tracks.empty())
    {
        const std::size_t clampedIndex = std::min(selectedTrackIndex_, snapshot.project.tracks.size() - 1);
        const VisibleTrack& selectedTrack = snapshot.project.tracks[clampedIndex];
        snapshot.selection.selectedTrackId = selectedTrack.trackId;
        snapshot.selection.selectedTrackName = selectedTrack.name;
        snapshot.selection.selectedClipCount = static_cast<std::uint32_t>(selectedTrack.clips.size());
    }

    snapshot.document.hasProject = !snapshot.project.projectName.empty();
    snapshot.document.dirty = snapshot.project.dirty;
    snapshot.document.sessionPath = snapshot.project.sessionPath;

    std::ostringstream summary;
    summary
        << "Callbacks " << snapshot.callbackCount
        << " | Recoveries " << snapshot.recoveryCount
        << " | Graph " << snapshot.activeGraphVersion
        << " | Safe " << boolLabel(snapshot.safeMode, "On", "Off");
    snapshot.document.statusSummary = summary.str();

    return snapshot;
}

void UI::updateTransportControls()
{
    SetWindowTextA(playButton_, visibleState_.transportState == AudioEngine::TransportState::Playing ? "Pause" : "Play");
    SetWindowTextA(recordButton_, "Rec");
    SetWindowTextA(patSongButton_, workspace_.songMode ? "SONG" : "PAT");

    char tempoBuffer[32]{};
    std::snprintf(tempoBuffer, sizeof(tempoBuffer), "%.3f", workspace_.tempoBpm);
    setStaticText(tempoLabel_, tempoBuffer);
    setStaticText(patternLabel_, std::to_string(workspace_.activePattern));
    setStaticText(snapLabel_, "Snap " + currentSnapLabel());

    InvalidateRect(engineStartButton_, nullptr, TRUE);
    InvalidateRect(playButton_, nullptr, TRUE);
    InvalidateRect(recordButton_, nullptr, TRUE);
    InvalidateRect(patSongButton_, nullptr, TRUE);
    InvalidateRect(patternPrevButton_, nullptr, TRUE);
    InvalidateRect(patternNextButton_, nullptr, TRUE);
    InvalidateRect(channelRackButton_, nullptr, TRUE);
    InvalidateRect(pianoRollButton_, nullptr, TRUE);
    InvalidateRect(mixerButton_, nullptr, TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UI::updateStatusLabel()
{
    std::ostringstream status;
    status
        << "Transport "
        << (visibleState_.transportState == AudioEngine::TransportState::Playing ? "Playing" :
            visibleState_.transportState == AudioEngine::TransportState::Paused ? "Paused" : "Stopped")
        << " | Mode " << (workspace_.songMode ? "Song" : "Pattern")
        << " | Backend " << visibleState_.backendName
        << " | Device " << visibleState_.deviceName
        << " | Status " << visibleState_.statusText
        << " | Focus " << paneName(workspace_.focusedPane);
    setStaticText(statusLabel_, status.str());
}

void UI::updateMetricLabels()
{
    char buffer[256]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "CPU %.2f%% | Disk cache %u | XRuns %llu | Misses %llu | Latency %u smp | SR %d | BS %d",
        visibleState_.cpuLoadApprox * 100.0,
        static_cast<unsigned int>(engine_.getMetrics().cachedClipCount),
        static_cast<unsigned long long>(visibleState_.xruns),
        static_cast<unsigned long long>(visibleState_.deadlineMisses),
        static_cast<unsigned int>(visibleState_.currentLatencySamples),
        visibleState_.sampleRate,
        visibleState_.blockSize);
    setStaticText(systemLabel_, buffer);
}

void UI::updateProjectLabels()
{
    std::ostringstream project;
    project
        << "Project " << visibleState_.project.projectName
        << " | Tracks " << visibleState_.project.tracks.size()
        << " | Buses " << visibleState_.project.buses.size()
        << " | Undo " << visibleState_.project.undoDepth
        << " | Redo " << visibleState_.project.redoDepth
        << " | Dirty " << boolLabel(visibleState_.project.dirty, "Yes", "No");
    setStaticText(projectSummaryLabel_, project.str());

    setStaticText(
        documentLabel_,
        std::string("Session ") +
        (visibleState_.document.sessionPath.empty() ? "<unsaved>" : visibleState_.document.sessionPath));

    std::ostringstream selection;
    selection
        << "Target Channel "
        << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName)
        << " | Clips " << visibleState_.selection.selectedClipCount;
    setStaticText(selectionLabel_, selection.str());
}

void UI::updateToggleStates()
{
    SendMessageA(automationCheckbox_, BM_SETCHECK, visibleState_.automationEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(pdcCheckbox_, BM_SETCHECK, visibleState_.pdcEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(anticipativeCheckbox_, BM_SETCHECK, visibleState_.anticipativeProcessingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    InvalidateRect(automationCheckbox_, nullptr, TRUE);
    InvalidateRect(pdcCheckbox_, nullptr, TRUE);
    InvalidateRect(anticipativeCheckbox_, nullptr, TRUE);
}

void UI::updateWindowTitle()
{
    std::ostringstream oss;
    oss
        << kWindowTitleBase
        << " | " << visibleState_.project.projectName
        << " | " << (workspace_.songMode ? "Song" : "Pattern")
        << " | BPM " << static_cast<int>(workspace_.tempoBpm + 0.5)
        << " | Pattern " << workspace_.activePattern;
    SetWindowTextA(hwnd_, oss.str().c_str());
}

void UI::updateWorkspacePanels()
{
    setStaticText(browserPanel_, buildBrowserPanelText());
    setStaticText(channelRackPanel_, buildChannelRackPanelText());
    setStaticText(stepSequencerPanel_, buildStepSequencerPanelText());
    setStaticText(pianoRollPanel_, buildPianoRollPanelText());
    setStaticText(playlistPanel_, buildPlaylistPanelText());
    setStaticText(mixerPanel_, buildMixerPanelText());
    setStaticText(pluginPanel_, buildPluginPanelText());

    setStaticText(
        hintsLabel_,
        "Shortcuts: Alt+F8 Browser | F5 Arrange | F6 Channel Rack window | F7 Piano Roll | F9 Mixer window | Space Play | +/- Zoom");

    setStaticText(browserHeaderLabel_, "Browser | " + currentBrowserTabLabel());
    setStaticText(channelHeaderLabel_, "Channel Rack | Target " + (visibleState_.selection.selectedTrackName.empty() ? std::string("<none>") : visibleState_.selection.selectedTrackName));
    setStaticText(pianoHeaderLabel_, "Piano Roll | Zoom " + currentZoomLabel(true) + " | Tool " + currentToolLabel(true));
    setStaticText(playlistHeaderLabel_, "Playlist | Zoom " + currentZoomLabel(false) + " | Tool " + currentToolLabel(false));
    setStaticText(mixerHeaderLabel_, "Mixer | Inserts " + std::to_string(std::max<std::size_t>(2, visibleState_.project.buses.size())));
    setStaticText(pluginHeaderLabel_, "Plugin / Channel Settings");
    setStaticText(contextLabel_, "Focused pane: " + std::string(paneName(workspace_.focusedPane)));
}

void UI::updateViewButtons()
{
    SetWindowTextA(browserButton_, "Browser");
    SetWindowTextA(channelRackButton_, "chan.\nrack");
    SetWindowTextA(pianoRollButton_, "piano\nroll");
    SetWindowTextA(playlistButton_, "Playlist");
    SetWindowTextA(mixerButton_, "mixer");
    SetWindowTextA(pluginButton_, "Plugin");

    InvalidateRect(browserButton_, nullptr, TRUE);
    InvalidateRect(channelRackButton_, nullptr, TRUE);
    InvalidateRect(pianoRollButton_, nullptr, TRUE);
    InvalidateRect(playlistButton_, nullptr, TRUE);
    InvalidateRect(mixerButton_, nullptr, TRUE);
    InvalidateRect(pluginButton_, nullptr, TRUE);
}

void UI::requestEngineStart()
{
    engine_.postCommand({AudioEngine::CommandType::StartEngine});
}

void UI::requestEngineStop()
{
    engine_.postCommand({AudioEngine::CommandType::StopEngine});
}

void UI::requestTransportPlay()
{
    if (visibleState_.transportState == AudioEngine::TransportState::Playing)
    {
        requestTransportPause();
        return;
    }

    engine_.postCommand({AudioEngine::CommandType::PlayTransport});
}

void UI::requestTransportPause()
{
    engine_.postCommand({AudioEngine::CommandType::PauseTransport});
}

void UI::requestTransportStop()
{
    engine_.postCommand({AudioEngine::CommandType::StopTransport});
}

void UI::requestGraphRebuild()
{
    engine_.postCommand({AudioEngine::CommandType::RebuildGraph});
}

void UI::requestOfflineRender()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::RenderOffline;
    command.textValue = "ui_offline_render.wav";
    engine_.postCommand(command);
}

void UI::requestToggleAutomation()
{
    engine_.postCommand({AudioEngine::CommandType::ToggleAutomation});
}

void UI::requestTogglePdc()
{
    engine_.postCommand({AudioEngine::CommandType::TogglePdc});
}

void UI::requestToggleAnticipativeProcessing()
{
    engine_.postCommand({AudioEngine::CommandType::ToggleAnticipativeProcessing});
}

void UI::requestTempoChange(double bpm)
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::SetTempo;
    command.doubleValue = std::max(10.0, std::min(522.0, bpm));
    engine_.postCommand(command);
}

void UI::requestAddTrack()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::AddTrack;
    command.textValue = "Channel " + std::to_string(visibleState_.project.tracks.size() + 1);
    engine_.postCommand(command);
}

void UI::requestAddBus()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::AddBus;
    command.textValue = "Insert " + std::to_string(visibleState_.project.buses.size() + 1);
    engine_.postCommand(command);
}

void UI::requestAddClip()
{
    if (visibleState_.selection.selectedTrackId == 0)
    {
        return;
    }

    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::AddClipToTrack;
    command.uintValue = visibleState_.selection.selectedTrackId;
    command.textValue = "Pattern " + std::to_string(workspace_.activePattern);
    engine_.postCommand(command);
}

void UI::requestNewProject()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::NewProject;
    command.textValue = "Untitled Project";
    engine_.postCommand(command);
    workspace_.activePattern = 1;
    selectedTrackIndex_ = 0;
}

void UI::requestUndo()
{
    engine_.postCommand({AudioEngine::CommandType::UndoEdit});
}

void UI::requestRedo()
{
    engine_.postCommand({AudioEngine::CommandType::RedoEdit});
}

void UI::requestSaveProject()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::SaveProject;
    command.textValue = visibleState_.document.sessionPath.empty() ? "session.dawproject" : visibleState_.document.sessionPath;
    engine_.postCommand(command);
}

void UI::requestLoadProject()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::LoadProject;
    command.textValue = visibleState_.document.sessionPath.empty() ? "session.dawproject" : visibleState_.document.sessionPath;
    engine_.postCommand(command);
}

void UI::requestCreatePluginStub()
{
    AudioEngine::PluginDescriptor descriptor{};
    descriptor.name = "Managed Plugin " + std::to_string(pluginManagerState_.loadedPlugins.size() + 1);
    descriptor.vendor = "DAW Cloud";
    descriptor.format = "VST3";
    descriptor.processMode =
        visibleState_.pluginSandboxEnabled ? AudioEngine::PluginProcessMode::GroupSandbox : AudioEngine::PluginProcessMode::InProcess;
    descriptor.supportsDoublePrecision = true;
    descriptor.supportsSampleAccurateAutomation = true;
    descriptor.reportedLatencySamples = 32;
    engine_.addPluginStub(descriptor, 0);
    pluginManagerState_.statusText = "New managed plugin stub added.";
}

void UI::requestTogglePluginSandboxMode()
{
    pluginManagerState_.statusText =
        std::string("Sandbox preference requested. Current engine sandbox state is ") +
        (visibleState_.pluginSandboxEnabled ? "enabled." : "disabled.");
}

void UI::showMainMenuPopup(WORD commandId)
{
    HMENU popupMenu = nullptr;
    HWND anchor = nullptr;

    switch (commandId)
    {
    case IdButtonMenuFile:
        popupMenu = fileMenu_;
        anchor = mainMenuButtons_[0];
        break;

    case IdButtonMenuEdit:
        popupMenu = editMenu_;
        anchor = mainMenuButtons_[1];
        break;

    case IdButtonMenuAdd:
        popupMenu = addMenu_;
        anchor = mainMenuButtons_[2];
        break;

    case IdButtonMenuPatterns:
        popupMenu = patternsMenu_;
        anchor = mainMenuButtons_[3];
        break;

    case IdButtonMenuView:
        popupMenu = viewMenu_;
        anchor = mainMenuButtons_[4];
        break;

    case IdButtonMenuOptions:
        popupMenu = optionsMenu_;
        anchor = mainMenuButtons_[5];
        break;

    case IdButtonMenuTools:
        popupMenu = toolsMenu_;
        anchor = mainMenuButtons_[6];
        break;

    case IdButtonMenuHelp:
        popupMenu = helpMenu_;
        anchor = mainMenuButtons_[7];
        break;

    default:
        break;
    }

    if (popupMenu == nullptr || anchor == nullptr)
    {
        return;
    }

    RECT anchorRect{};
    GetWindowRect(anchor, &anchorRect);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(
        popupMenu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        anchorRect.left,
        anchorRect.bottom,
        0,
        hwnd_,
        nullptr);
}

void UI::handleCommand(WORD commandId)
{
    switch (commandId)
    {
    case IdButtonMenuFile:
    case IdButtonMenuEdit:
    case IdButtonMenuAdd:
    case IdButtonMenuPatterns:
    case IdButtonMenuView:
    case IdButtonMenuOptions:
    case IdButtonMenuTools:
    case IdButtonMenuHelp:
        showMainMenuPopup(commandId);
        break;

    case IdButtonEngineStart:
    case IdMenuToolsStartEngine:
        requestEngineStart();
        break;

    case IdButtonEngineStop:
    case IdMenuToolsStopEngine:
        requestEngineStop();
        break;

    case IdButtonPlay:
        requestTransportPlay();
        break;

    case IdButtonStopTransport:
        requestTransportStop();
        break;

    case IdButtonRecord:
        workspace_.recordArmed = !workspace_.recordArmed;
        break;

    case IdButtonPatSong:
    case IdMenuOptionsPatSong:
        workspace_.songMode = !workspace_.songMode;
        break;

    case IdButtonTempoDown:
        requestTempoChange(workspace_.tempoBpm - 1.0);
        break;

    case IdButtonTempoUp:
        requestTempoChange(workspace_.tempoBpm + 1.0);
        break;

    case IdButtonPatternPrev:
    case IdMenuPatternsPrev:
        selectPreviousPattern();
        break;

    case IdButtonPatternNext:
    case IdMenuPatternsNext:
        selectNextPattern();
        break;

    case IdButtonSnapPrev:
        cycleSnap(-1);
        break;

    case IdButtonSnapNext:
        cycleSnap(1);
        break;

    case IdButtonBrowser:
    case IdMenuViewBrowser:
    case IdButtonMenuBrowser:
        togglePane(WorkspacePane::Browser);
        break;

    case IdButtonBrowserTabPrev:
        cycleBrowserTab(-1);
        break;

    case IdButtonBrowserTabNext:
        cycleBrowserTab(1);
        break;

    case IdButtonChannelRack:
    case IdMenuViewChannelRack:
    case IdButtonMenuChannelRack:
        togglePane(WorkspacePane::ChannelRack);
        break;

    case IdButtonPianoRoll:
    case IdMenuViewPianoRoll:
    case IdButtonMenuPianoRoll:
        togglePane(WorkspacePane::PianoRoll);
        break;

    case IdButtonPianoZoomPrev:
        cycleZoom(true, -1);
        break;

    case IdButtonPianoZoomNext:
        cycleZoom(true, 1);
        break;

    case IdButtonPianoToolPrev:
        cycleEditorTool(true, -1);
        break;

    case IdButtonPianoToolNext:
        cycleEditorTool(true, 1);
        break;

    case IdButtonPlaylist:
    case IdMenuViewPlaylist:
    case IdButtonMenuPlaylist:
        togglePane(WorkspacePane::Playlist);
        break;

    case IdButtonPlaylistZoomPrev:
        cycleZoom(false, -1);
        break;

    case IdButtonPlaylistZoomNext:
        cycleZoom(false, 1);
        break;

    case IdButtonPlaylistToolPrev:
        cycleEditorTool(false, -1);
        break;

    case IdButtonPlaylistToolNext:
        cycleEditorTool(false, 1);
        break;

    case IdButtonMixer:
    case IdMenuViewMixer:
    case IdButtonMenuMixer:
        togglePane(WorkspacePane::Mixer);
        break;

    case IdButtonPlugin:
    case IdMenuViewPlugin:
    case IdButtonMenuPlugin:
        togglePane(WorkspacePane::Plugin);
        break;

    case IdButtonManagePlugins:
        showPluginManagerWindow();
        break;

    case IdMenuFileNew:
        requestNewProject();
        break;

    case IdButtonAddTrack:
    case IdMenuAddTrack:
        requestAddTrack();
        break;

    case IdButtonAddBus:
    case IdMenuAddBus:
        requestAddBus();
        break;

    case IdButtonAddClip:
    case IdMenuAddClip:
        requestAddClip();
        break;

    case IdButtonUndo:
    case IdMenuEditUndo:
        requestUndo();
        break;

    case IdButtonRedo:
    case IdMenuEditRedo:
        requestRedo();
        break;

    case IdButtonSaveProject:
    case IdMenuFileSave:
        requestSaveProject();
        break;

    case IdButtonLoadProject:
    case IdMenuFileOpen:
        requestLoadProject();
        break;

    case IdButtonPrevTrack:
        selectPreviousTrack();
        break;

    case IdButtonNextTrack:
        selectNextTrack();
        break;

    case IdButtonRebuildGraph:
    case IdMenuToolsRebuildGraph:
        requestGraphRebuild();
        break;

    case IdButtonRenderOffline:
    case IdMenuToolsRenderOffline:
        requestOfflineRender();
        break;

    case IdCheckboxAutomation:
    case IdMenuOptionsAutomation:
        requestToggleAutomation();
        break;

    case IdCheckboxPdc:
    case IdMenuOptionsPdc:
        requestTogglePdc();
        break;

    case IdCheckboxAnticipative:
    case IdMenuOptionsAnticipative:
        requestToggleAnticipativeProcessing();
        break;

    case IdMenuOptionsManagePlugins:
        showPluginManagerWindow();
        break;

    case IdMenuHelpAbout:
        showAboutDialog();
        break;

    case IdMenuFileExit:
        DestroyWindow(hwnd_);
        break;

    default:
        break;
    }

    layoutControls();
    refreshFromEngineSnapshot();
}


