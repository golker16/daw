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
    constexpr int kOuterPadding = 12;
    constexpr int kGap = 8;
    constexpr int kToolbarHeight = 82;
    constexpr int kTransportHeight = 30;
    constexpr int kInfoStripHeight = 54;
    constexpr int kBrowserWidth = 286;
    constexpr int kBrowserTabCount = 3;
    constexpr int kBrowserTabHeight = 28;
    constexpr int kBrowserRowHeight = 44;
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

    constexpr const char* kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"};

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
    static HBRUSH darkFrameBrush = CreateSolidBrush(RGB(27, 31, 37));
    static HBRUSH darkPanelBrush = CreateSolidBrush(RGB(24, 28, 34));

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

    HMENU fileMenu = CreatePopupMenu();
    HMENU editMenu = CreatePopupMenu();
    HMENU addMenu = CreatePopupMenu();
    HMENU patternsMenu = CreatePopupMenu();
    HMENU viewMenu = CreatePopupMenu();
    HMENU optionsMenu = CreatePopupMenu();
    HMENU toolsMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    if (fileMenu == nullptr || editMenu == nullptr || addMenu == nullptr || patternsMenu == nullptr ||
        viewMenu == nullptr || optionsMenu == nullptr || toolsMenu == nullptr || helpMenu == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno de los submenus.");
    }

    AppendMenuA(fileMenu, MF_STRING, IdMenuFileNew, "New Project");
    AppendMenuA(fileMenu, MF_STRING, IdMenuFileOpen, "Open Project");
    AppendMenuA(fileMenu, MF_STRING, IdMenuFileSave, "Save Project");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fileMenu, MF_STRING, IdMenuFileExit, "Exit");

    AppendMenuA(editMenu, MF_STRING, IdMenuEditUndo, "Undo");
    AppendMenuA(editMenu, MF_STRING, IdMenuEditRedo, "Redo");

    AppendMenuA(addMenu, MF_STRING, IdMenuAddTrack, "Add Track");
    AppendMenuA(addMenu, MF_STRING, IdMenuAddBus, "Add Bus");
    AppendMenuA(addMenu, MF_STRING, IdMenuAddClip, "Add Clip");

    AppendMenuA(patternsMenu, MF_STRING, IdMenuPatternsPrev, "Previous Pattern");
    AppendMenuA(patternsMenu, MF_STRING, IdMenuPatternsNext, "Next Pattern");

    AppendMenuA(viewMenu, MF_STRING, IdMenuViewBrowser, "Browser\tAlt+F8");
    AppendMenuA(viewMenu, MF_STRING, IdMenuViewChannelRack, "Channel Rack\tF6");
    AppendMenuA(viewMenu, MF_STRING, IdMenuViewPianoRoll, "Piano Roll\tF7");
    AppendMenuA(viewMenu, MF_STRING, IdMenuViewPlaylist, "Playlist\tF5");
    AppendMenuA(viewMenu, MF_STRING, IdMenuViewMixer, "Mixer\tF9");
    AppendMenuA(viewMenu, MF_STRING, IdMenuViewPlugin, "Plugin Window");

    AppendMenuA(optionsMenu, MF_STRING, IdMenuOptionsAutomation, "Toggle Automation");
    AppendMenuA(optionsMenu, MF_STRING, IdMenuOptionsPdc, "Toggle PDC");
    AppendMenuA(optionsMenu, MF_STRING, IdMenuOptionsAnticipative, "Toggle Anticipative");
    AppendMenuA(optionsMenu, MF_STRING, IdMenuOptionsPatSong, "Toggle Pattern / Song");
    AppendMenuA(optionsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(optionsMenu, MF_STRING, IdMenuOptionsManagePlugins, "Manage Plugins");

    AppendMenuA(toolsMenu, MF_STRING, IdMenuToolsStartEngine, "Start Engine");
    AppendMenuA(toolsMenu, MF_STRING, IdMenuToolsStopEngine, "Stop Engine");
    AppendMenuA(toolsMenu, MF_STRING, IdMenuToolsRebuildGraph, "Rebuild Graph");
    AppendMenuA(toolsMenu, MF_STRING, IdMenuToolsRenderOffline, "Offline Render");

    AppendMenuA(helpMenu, MF_STRING, IdMenuHelpAbout, "About");

    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), "File");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), "Edit");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(addMenu), "Add");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(patternsMenu), "Patterns");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), "View");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(optionsMenu), "Options");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(toolsMenu), "Tools");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), "Help");

    if (!SetMenu(hwnd_, mainMenu_))
    {
        throw UiInitializationException("No se pudo asociar el menu a la ventana.");
    }
}

void UI::createControls()
{
    const DWORD buttonStyle = WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON;
    const DWORD checkboxStyle = WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX;
    const DWORD staticStyle = WS_VISIBLE | WS_CHILD | SS_LEFT;

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
    tempoLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelTempo), hInstance_, nullptr);
    patternLabel_ = CreateWindowA("STATIC", "", staticStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPattern), hInstance_, nullptr);
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

    if (engineStartButton_ == nullptr || engineStopButton_ == nullptr || playButton_ == nullptr ||
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
    const int contentWidth = std::max(420, width - (kOuterPadding * 2));

    int x = kOuterPadding;
    const int row1Y = kOuterPadding;
    const int row2Y = row1Y + 38;

    const auto placeRow1Button = [&](HWND control, int buttonWidth)
    {
        MoveWindow(control, x, row1Y, buttonWidth, kTransportHeight, TRUE);
        x += buttonWidth + 6;
    };

    placeRow1Button(engineStartButton_, 74);
    placeRow1Button(engineStopButton_, 84);
    placeRow1Button(playButton_, 58);
    placeRow1Button(stopTransportButton_, 58);
    placeRow1Button(recordButton_, 52);
    placeRow1Button(patSongButton_, 58);
    placeRow1Button(tempoDownButton_, 24);
    MoveWindow(tempoLabel_, x, row1Y + 6, 84, 20, TRUE);
    x += 90;
    placeRow1Button(tempoUpButton_, 24);
    placeRow1Button(patternPrevButton_, 24);
    MoveWindow(patternLabel_, x, row1Y + 6, 96, 20, TRUE);
    x += 102;
    placeRow1Button(patternNextButton_, 24);
    placeRow1Button(snapPrevButton_, 24);
    MoveWindow(snapLabel_, x, row1Y + 6, 108, 20, TRUE);
    x += 114;
    placeRow1Button(snapNextButton_, 24);
    placeRow1Button(browserButton_, 72);
    placeRow1Button(channelRackButton_, 64);
    placeRow1Button(mixerButton_, 64);
    placeRow1Button(pianoRollButton_, 64);
    placeRow1Button(pluginButton_, 64);
    placeRow1Button(playlistButton_, 70);

    x = kOuterPadding;
    const auto placeRow2Button = [&](HWND control, int buttonWidth)
    {
        MoveWindow(control, x, row2Y, buttonWidth, 28, TRUE);
        x += buttonWidth + 6;
    };

    placeRow2Button(addTrackButton_, 88);
    placeRow2Button(addClipButton_, 72);
    placeRow2Button(addBusButton_, 72);
    placeRow2Button(undoButton_, 60);
    placeRow2Button(redoButton_, 60);
    placeRow2Button(saveProjectButton_, 60);
    placeRow2Button(loadProjectButton_, 60);
    placeRow2Button(prevTrackButton_, 72);
    placeRow2Button(nextTrackButton_, 72);
    placeRow2Button(rebuildGraphButton_, 68);
    placeRow2Button(renderOfflineButton_, 68);
    placeRow2Button(managePluginsButton_, 82);

    MoveWindow(automationCheckbox_, x, row2Y + 4, 98, 22, TRUE);
    x += 104;
    MoveWindow(pdcCheckbox_, x, row2Y + 4, 52, 22, TRUE);
    x += 58;
    MoveWindow(anticipativeCheckbox_, x, row2Y + 4, 102, 22, TRUE);
    x += 108;
    MoveWindow(systemLabel_, x, row2Y + 4, std::max(120, contentWidth - (x - kOuterPadding)), 22, TRUE);

    int y = kOuterPadding + kToolbarHeight + kGap;
    MoveWindow(statusLabel_, kOuterPadding, y, contentWidth, 20, TRUE);
    MoveWindow(projectSummaryLabel_, kOuterPadding, y + 20, contentWidth, 18, TRUE);
    MoveWindow(documentLabel_, kOuterPadding, y + 38, contentWidth / 2, 16, TRUE);
    MoveWindow(selectionLabel_, kOuterPadding + (contentWidth / 2), y + 38, contentWidth / 2, 16, TRUE);
    y += kInfoStripHeight + kGap;

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

    MoveWindow(hintsLabel_, workspaceX, height - kOuterPadding - footerHeight, workspaceWidth, footerHeight, TRUE);
    ensureDetachedWindows();
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
    SetWindowTextA(recordButton_, workspace_.recordArmed ? "Rec*" : "Rec");
    SetWindowTextA(patSongButton_, workspace_.songMode ? "Song" : "Pat");
    setStaticText(tempoLabel_, "BPM " + std::to_string(static_cast<int>(workspace_.tempoBpm + 0.5)));
    setStaticText(patternLabel_, "Pattern " + std::to_string(workspace_.activePattern));
    setStaticText(snapLabel_, "Snap " + currentSnapLabel());
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
    SetWindowTextA(browserButton_, workspace_.browserVisible ? "Browser*" : "Browser");
    SetWindowTextA(channelRackButton_, workspace_.channelRackVisible ? "Rack*" : "Rack");
    SetWindowTextA(pianoRollButton_, workspace_.pianoRollVisible ? "Piano*" : "Piano");
    SetWindowTextA(playlistButton_, "Arrange");
    SetWindowTextA(mixerButton_, workspace_.mixerVisible ? "Mixer*" : "Mixer");
    SetWindowTextA(pluginButton_, workspace_.pluginVisible ? "Plugin*" : "Plugin");
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

void UI::handleCommand(WORD commandId)
{
    switch (commandId)
    {
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

void UI::handlePluginManagerCommand(WORD commandId)
{
    if (pluginManagerHwnd_ == nullptr)
    {
        return;
    }

    if (commandId == 4001)
    {
        char buffer[256]{};
        GetWindowTextA(pluginManagerSearchEdit_, buffer, static_cast<int>(sizeof(buffer)));
        pluginManagerState_.searchText = buffer;
        applyPluginSearchFilter();
        updatePluginManagerControls();
        return;
    }

    if (commandId == 4002)
    {
        const LRESULT selection = SendMessageA(pluginManagerListBox_, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR)
        {
            pluginManagerState_.selectedIndex = static_cast<std::size_t>(selection);
        }
        updatePluginManagerControls();
        return;
    }

    switch (commandId)
    {
    case IdButtonPluginRescan:
        pluginManagerState_.statusText = "Plugin database refreshed.";
        break;

    case IdButtonPluginAddStub:
        requestCreatePluginStub();
        break;

    case IdButtonPluginToggleSandbox:
        requestTogglePluginSandboxMode();
        break;

    case IdButtonPluginClose:
        closePluginManagerWindow();
        return;

    default:
        break;
    }

    refreshPluginManagerState();
    updatePluginManagerControls();
}

bool UI::handleKeyDown(WPARAM wParam, LPARAM)
{
    const bool altPressed = (GetKeyState(VK_MENU) & 0x8000) != 0;

    switch (wParam)
    {
    case VK_F5:
        togglePane(WorkspacePane::Playlist);
        break;

    case VK_F6:
        togglePane(WorkspacePane::ChannelRack);
        break;

    case VK_F7:
        togglePane(WorkspacePane::PianoRoll);
        break;

    case VK_F9:
        togglePane(WorkspacePane::Mixer);
        break;

    case VK_F8:
        if (altPressed)
        {
            togglePane(WorkspacePane::Browser);
        }
        else
        {
            return false;
        }
        break;

    case VK_SPACE:
        requestTransportPlay();
        break;

    case VK_OEM_PLUS:
    case VK_ADD:
        cycleZoom(workspace_.focusedPane == WorkspacePane::PianoRoll, 1);
        break;

    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        cycleZoom(workspace_.focusedPane == WorkspacePane::PianoRoll, -1);
        break;

    default:
        return false;
    }

    layoutControls();
    refreshFromEngineSnapshot();
    return true;
}

void UI::showAboutDialog() const
{
    std::ostringstream oss;
    oss
        << "DAW Cloud Template\n\n"
        << "FL-style workspace layer implemented on top of the current engine.\n\n"
        << "Backend: " << visibleState_.backendName << "\n"
        << "Device: " << visibleState_.deviceName << "\n"
        << "Project: " << visibleState_.project.projectName << "\n"
        << "Tempo: " << static_cast<int>(workspace_.tempoBpm + 0.5) << " BPM\n"
        << "Pattern: " << workspace_.activePattern << "\n"
        << "Mode: " << (workspace_.songMode ? "Song" : "Pattern") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "On", "Off") << "\n"
        << "PDC: " << boolLabel(visibleState_.pdcEnabled, "On", "Off") << "\n"
        << "Anticipative: " << boolLabel(visibleState_.anticipativeProcessingEnabled, "On", "Off") << "\n";

    MessageBoxA(hwnd_, oss.str().c_str(), "About", MB_OK | MB_ICONINFORMATION);
}

void UI::selectNextTrack()
{
    if (visibleState_.project.tracks.empty())
    {
        selectedTrackIndex_ = 0;
        return;
    }

    selectedTrackIndex_ = (selectedTrackIndex_ + 1) % visibleState_.project.tracks.size();
}

void UI::selectPreviousTrack()
{
    if (visibleState_.project.tracks.empty())
    {
        selectedTrackIndex_ = 0;
        return;
    }

    selectedTrackIndex_ =
        selectedTrackIndex_ == 0
            ? (visibleState_.project.tracks.size() - 1)
            : (selectedTrackIndex_ - 1);
}

void UI::selectNextPattern()
{
    const int patternCount = std::max(1, static_cast<int>(workspaceModel_.patterns.size()));
    workspace_.activePattern = (workspace_.activePattern % patternCount) + 1;
    syncWorkspaceModel();
}

void UI::selectPreviousPattern()
{
    const int patternCount = std::max(1, static_cast<int>(workspaceModel_.patterns.size()));
    workspace_.activePattern = workspace_.activePattern <= 1 ? patternCount : workspace_.activePattern - 1;
    syncWorkspaceModel();
}

void UI::cycleZoom(bool pianoRoll, int delta)
{
    std::size_t& index = pianoRoll ? workspace_.pianoZoomIndex : workspace_.playlistZoomIndex;
    const int newIndex = static_cast<int>(index) + delta;
    if (newIndex < 0)
    {
        index = 4;
    }
    else
    {
        index = static_cast<std::size_t>(newIndex) % 5;
    }
}

void UI::cycleBrowserTab(int delta)
{
    const int newIndex = static_cast<int>(workspace_.browserTabIndex) + delta;
    if (newIndex < 0)
    {
        workspace_.browserTabIndex = kBrowserTabCount - 1;
    }
    else
    {
        workspace_.browserTabIndex = static_cast<std::size_t>(newIndex) % kBrowserTabCount;
    }
}

void UI::cycleEditorTool(bool pianoRoll, int delta)
{
    EditorTool& tool = pianoRoll ? workspace_.pianoTool : workspace_.playlistTool;
    int current = static_cast<int>(tool);
    current += delta;
    if (current < 0)
    {
        current = 5;
    }
    tool = static_cast<EditorTool>(current % 6);
}

void UI::cycleSnap(int delta)
{
    static constexpr const char* kSnapModes[] = {"Off", "Line", "Cell", "Beat", "Bar", "Step"};
    constexpr std::size_t modeCount = sizeof(kSnapModes) / sizeof(kSnapModes[0]);

    const int newIndex = static_cast<int>(workspace_.snapIndex) + delta;
    if (newIndex < 0)
    {
        workspace_.snapIndex = modeCount - 1;
    }
    else
    {
        workspace_.snapIndex = static_cast<std::size_t>(newIndex) % modeCount;
    }
}

void UI::togglePane(WorkspacePane pane)
{
    switch (pane)
    {
    case WorkspacePane::Browser:
        workspace_.browserVisible = !workspace_.browserVisible;
        workspace_.focusedPane = workspace_.browserVisible ? WorkspacePane::Browser : WorkspacePane::Playlist;
        break;

    case WorkspacePane::ChannelRack:
        workspace_.channelRackVisible = !workspace_.channelRackVisible;
        workspace_.focusedPane = workspace_.channelRackVisible ? WorkspacePane::ChannelRack : WorkspacePane::Playlist;
        break;

    case WorkspacePane::PianoRoll:
        workspace_.pianoRollVisible = !workspace_.pianoRollVisible;
        workspace_.focusedPane = workspace_.pianoRollVisible ? WorkspacePane::PianoRoll : WorkspacePane::Playlist;
        break;

    case WorkspacePane::Playlist:
        workspace_.playlistVisible = true;
        workspace_.focusedPane = WorkspacePane::Playlist;
        break;

    case WorkspacePane::Mixer:
        workspace_.mixerVisible = !workspace_.mixerVisible;
        workspace_.focusedPane = workspace_.mixerVisible ? WorkspacePane::Mixer : WorkspacePane::Playlist;
        break;

    case WorkspacePane::Plugin:
        workspace_.pluginVisible = !workspace_.pluginVisible;
        workspace_.focusedPane = workspace_.pluginVisible ? WorkspacePane::Plugin : WorkspacePane::Playlist;
        break;
    }
}

std::string UI::currentSnapLabel() const
{
    static constexpr const char* kSnapModes[] = {"Off", "Line", "Cell", "Beat", "Bar", "Step"};
    return kSnapModes[std::min<std::size_t>(workspace_.snapIndex, 5)];
}

std::string UI::currentBrowserTabLabel() const
{
    static constexpr const char* kBrowserTabs[] = {"Patterns", "Audio Clips", "Automation Clips"};
    return kBrowserTabs[std::min<std::size_t>(workspace_.browserTabIndex, kBrowserTabCount - 1)];
}

std::string UI::currentZoomLabel(bool pianoRoll) const
{
    static constexpr const char* kZoomLevels[] = {"Far", "Normal", "Close", "Detail", "Micro"};
    const std::size_t index = pianoRoll ? workspace_.pianoZoomIndex : workspace_.playlistZoomIndex;
    return kZoomLevels[std::min<std::size_t>(index, 4)];
}

std::string UI::currentToolLabel(bool pianoRoll) const
{
    const EditorTool tool = pianoRoll ? workspace_.pianoTool : workspace_.playlistTool;
    switch (tool)
    {
    case EditorTool::Draw: return "Draw";
    case EditorTool::Paint: return "Paint";
    case EditorTool::Select: return "Select";
    case EditorTool::DeleteTool: return "Delete";
    case EditorTool::Slice: return "Slice";
    case EditorTool::Mute: return "Mute";
    default: return "Draw";
    }
}

std::string UI::buildBrowserPanelText() const
{
    std::ostringstream browser;
    browser << "Browser\n\n";
    for (std::size_t index = 0; index < workspaceModel_.browserEntries.size(); ++index)
    {
        const BrowserEntry& entry = workspaceModel_.browserEntries[index];
        browser
            << (static_cast<int>(index) == workspaceModel_.selectedBrowserIndex ? "> " : "  ")
            << entry.category << " | " << entry.label;
        if (entry.favorite)
        {
            browser << " | Fav";
        }
        browser << "\n    " << entry.subtitle << "\n";
    }

    browser
        << "\nDrag-drop target: " << browserDropTargetLabel()
        << "\nAlt+F8 toggles this panel.";
    return browser.str();
}

std::string UI::buildChannelRackPanelText() const
{
    std::ostringstream rack;
    rack << "Channel Rack\n\n";
    if (workspaceModel_.patternLanes.empty())
    {
        rack << "No channels yet.\nUse Add Track to create instruments, samplers or automation channels.";
        return rack.str();
    }

    for (std::size_t index = 0; index < workspaceModel_.patternLanes.size(); ++index)
    {
        const PatternLaneState& lane = workspaceModel_.patternLanes[index];
        int enabledSteps = 0;
        for (const auto& step : lane.steps)
        {
            enabledSteps += step.enabled ? 1 : 0;
        }
        rack
            << (index == selectedTrackIndex_ ? "> " : "  ")
            << lane.name
            << " | Steps " << enabledSteps << '/' << lane.steps.size()
            << " | Swing " << lane.swing
            << " | Shuffle " << lane.shuffle
            << " | Notes " << lane.notes.size()
            << "\n";
    }

    rack << "\nCada fila ya modela un canal con secuencia, notas y groove por patrón.";
    return rack.str();
}

std::string UI::buildStepSequencerPanelText() const
{
    if (workspaceModel_.patternLanes.empty())
    {
        return "Step Sequencer\n\nNo active channels.";
    }

    const int laneIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];

    std::ostringstream steps;
    steps
        << "Step Sequencer\n\n"
        << lane.name << "\n"
        << "Pattern " << workspace_.activePattern
        << " | Snap " << currentSnapLabel()
        << " | Swing " << lane.swing
        << " | Shuffle " << lane.shuffle
        << "\n\n";

    for (std::size_t step = 0; step < lane.steps.size(); ++step)
    {
        const ChannelStepState& cell = lane.steps[step];
        steps << (cell.enabled ? 'x' : '.');
        if ((step + 1) % 4 == 0)
        {
            steps << ' ';
        }
    }
    steps
        << "\nVelocity lane: ";
    for (std::size_t step = 0; step < lane.steps.size(); ++step)
    {
        steps << (lane.steps[step].enabled ? std::to_string(lane.steps[step].velocity / 10) : "-");
        if ((step + 1) % 4 == 0)
        {
            steps << ' ';
        }
    }
    steps << "\nClick cells here to toggle the pattern like FL step programming.";
    return steps.str();
}

std::string UI::buildPianoRollPanelText() const
{
    if (workspaceModel_.patternLanes.empty())
    {
        return "Piano Roll\n\nNo pattern lane selected.";
    }

    const int laneIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
    std::ostringstream piano;
    piano
        << "Piano Roll\n\n"
        << "Target Channel: " << lane.name
        << "\nPattern: " << workspace_.activePattern
        << " | Snap: " << currentSnapLabel()
        << " | Zoom: " << currentZoomLabel(true)
        << " | Tool: " << currentToolLabel(true)
        << "\nGhost notes: visible | Helpers: on | Velocity lane: visible | Notes: " << lane.notes.size()
        << "\nRange: " << noteLabelFromLane(0) << " .. " << noteLabelFromLane(kPianoLaneCount - 1)
        << "\n\n";

    for (const auto& note : lane.notes)
    {
        piano
            << noteLabelFromLane(note.lane)
            << " @ " << note.step
            << " len " << note.length
            << " vel " << note.velocity
            << (note.slide ? " | slide" : "")
            << (note.accent ? " | accent" : "")
            << "\n";
    }

    piano << "\nDrag notes to reshape melodies and accents inside the active pattern.";
    return piano.str();
}

std::string UI::buildPlaylistPanelText() const
{
    std::ostringstream playlist;
    playlist
        << "Playlist\n\n"
        << "Mode: " << (workspace_.songMode ? "Song arrangement" : "Pattern preview")
        << " | Pattern: " << workspace_.activePattern
        << " | Zoom: " << currentZoomLabel(false)
        << " | Tool: " << currentToolLabel(false)
        << " | Patterns loaded: " << workspaceModel_.patterns.size()
        << "\nTimeline: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8\n\n";

    if (workspaceModel_.playlistBlocks.empty() && workspaceModel_.automationLanes.empty())
    {
        playlist << "Track 1  [empty lane]\n";
    }
    else
    {
        const int laneCount = std::max(1, static_cast<int>(workspaceModel_.patternLanes.size() + workspaceModel_.automationLanes.size()));
        for (int lane = 0; lane < laneCount; ++lane)
        {
            const std::string laneName =
                lane < static_cast<int>(workspaceModel_.patternLanes.size())
                    ? workspaceModel_.patternLanes[static_cast<std::size_t>(lane)].name
                    : ("Auto " + workspaceModel_.automationLanes[static_cast<std::size_t>(lane - static_cast<int>(workspaceModel_.patternLanes.size()))].target);
            playlist << "Lane " << (lane + 1) << " " << laneName << " : ";

            bool any = false;
            for (const auto& block : workspaceModel_.playlistBlocks)
            {
                if (block.lane == lane)
                {
                    playlist << "[" << block.label << " @" << block.startCell << " len " << block.lengthCells << " " << block.clipType;
                    if (block.patternNumber > 0)
                    {
                        playlist << " P" << block.patternNumber;
                    }
                    playlist << "] ";
                    any = true;
                }
            }
            for (const auto& automation : workspaceModel_.automationLanes)
            {
                if (automation.lane == lane)
                {
                    playlist << "[Auto " << automation.target << " @" << automation.startCell << " len " << automation.lengthCells << "] ";
                    any = true;
                }
            }
            if (!any)
            {
                playlist << "[empty]";
            }
            playlist << "\n";
        }
    }

    playlist
        << "\nMarkers: ";
    for (const auto& marker : workspaceModel_.markers)
    {
        playlist << "[" << marker.name << " @" << marker.timelineCell << "] ";
    }
    playlist
        << "\nPattern Clips, Audio Clips and Automation Clips share the same arranger."
        << "\nThis is the arrangement view targeted by F5.";
    return playlist.str();
}

std::string UI::buildMixerPanelText() const
{
    std::ostringstream mixer;
    mixer
        << "Mixer\n\n"
        << "Master | CPU " << static_cast<int>(visibleState_.cpuLoadApprox * 100.0 + 0.5)
        << "% | Peak " << visibleState_.peakBlockTimeUs
        << " us | Latency " << visibleState_.currentLatencySamples << " smp\n\n";

    for (const auto& strip : workspaceModel_.mixerStrips)
    {
        mixer
            << (strip.name == "Master" ? "Master" : ("Insert " + std::to_string(strip.insertSlot)))
            << " | " << strip.name
            << " | Fader " << strip.volumeDb << " dB"
            << " | Pan " << (strip.pan == 0.0 ? "C" : (strip.pan < 0.0 ? "L" : "R"))
            << " | Meter " << strip.peakLevel << "%"
            << " | Route " << (strip.routeTarget < 0 ? std::string("Master") : ("Ins " + std::to_string(strip.routeTarget + 1)))
            << " | Send " << static_cast<int>(strip.sendAmount * 100.0 + 0.5) << "% | FX ";
        for (std::size_t fx = 0; fx < strip.effects.size(); ++fx)
        {
            mixer << strip.effects[fx];
            if (fx + 1 < strip.effects.size())
            {
                mixer << '/';
            }
        }
        mixer << "\n";
    }

    mixer << "\nRouting, sends, insert FX and metering are represented in this strip rack.";
    return mixer.str();
}

std::string UI::buildPluginPanelText() const
{
    const ChannelSettingsState* channel = nullptr;
    if (!workspaceModel_.channelSettings.empty())
    {
        const int channelIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.channelSettings.size() - 1));
        channel = &workspaceModel_.channelSettings[static_cast<std::size_t>(channelIndex)];
    }

    std::ostringstream plugin;
    plugin
        << "Plugin / Channel Settings\n\n"
        << "Target: " << (channel == nullptr ? (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName) : channel->name) << "\n"
        << "Wrapper: " << boolLabel(visibleState_.pluginHostEnabled, "Plugin host enabled", "In-process") << "\n"
        << "Sandbox: " << boolLabel(visibleState_.pluginSandboxEnabled, "On", "Off") << "\n"
        << "64-bit path: " << boolLabel(visibleState_.prefer64BitMix, "On", "Off") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "Sample-accurate", "Off") << "\n"
        << "PDC: " << boolLabel(visibleState_.pdcEnabled, "On", "Off") << "\n\n"
        << "Sampler / Synth Controls\n"
        << "  - Gain " << (channel == nullptr ? 0.80 : channel->gain) << "\n"
        << "  - Pan " << (channel == nullptr ? 0.00 : channel->pan) << "\n"
        << "  - Pitch " << (channel == nullptr ? 0.0 : channel->pitchSemitones) << " st\n"
        << "  - Attack " << (channel == nullptr ? 12.0 : channel->attackMs) << " ms\n"
        << "  - Release " << (channel == nullptr ? 180.0 : channel->releaseMs) << " ms\n"
        << "  - Filter cutoff " << (channel == nullptr ? 8400.0 : channel->filterCutoffHz) << " Hz\n"
        << "  - Resonance " << (channel == nullptr ? 0.20 : channel->resonance) << "\n"
        << "  - Mixer insert " << (channel == nullptr ? 1 : channel->mixerInsert) << "\n"
        << "  - Route target " << (channel == nullptr ? 0 : channel->routeTarget) << "\n"
        << "  - Reverse " << boolLabel(channel != nullptr && channel->reverse, "On", "Off") << "\n"
        << "  - Stretch " << boolLabel(channel == nullptr || channel->timeStretch, "On", "Off") << "\n\n";

    if (channel != nullptr)
    {
        plugin << "Plugin Rack\n";
        for (const auto& entry : channel->pluginRack)
        {
            plugin << "  - " << entry << "\n";
        }
        plugin << "\nPresets\n";
        for (const auto& preset : channel->presets)
        {
            plugin << "  - " << preset << "\n";
        }
    }

    plugin << "\nThis pane now represents persistent channel settings, plugin rack and routing state.";
    return plugin.str();
}

std::string UI::buildPluginManagerDetailText() const
{
    std::ostringstream detail;
    detail << "Plugin Details\n\n";

    if (pluginManagerState_.filteredPlugins.empty())
    {
        detail << "No plugins match the current filter.\n";
    }
    else
    {
        const auto& plugin =
            pluginManagerState_.filteredPlugins[std::min(pluginManagerState_.selectedIndex, pluginManagerState_.filteredPlugins.size() - 1)];
        detail
            << "Name: " << plugin.name << "\n"
            << "Vendor: " << plugin.vendor << "\n"
            << "Format: " << plugin.format << "\n"
            << "Hosting: " << plugin.processModeLabel << "\n"
            << "Latency: " << plugin.latencySamples << " samples\n"
            << "Double precision: " << boolLabel(plugin.supportsDoublePrecision, "Yes", "No") << "\n"
            << "Sample accurate automation: " << boolLabel(plugin.supportsSampleAccurateAutomation, "Yes", "No") << "\n"
            << "Plugin host enabled: " << boolLabel(visibleState_.pluginHostEnabled, "Yes", "No") << "\n"
            << "Sandbox enabled: " << boolLabel(visibleState_.pluginSandboxEnabled, "Yes", "No") << "\n";
    }

    if (!pluginManagerState_.statusText.empty())
    {
        detail << "\nStatus: " << pluginManagerState_.statusText;
    }

    return detail.str();
}

std::string UI::buildPluginManagerPathText() const
{
    std::ostringstream paths;
    paths << "Search Paths / Favorites / Blacklist\n\nPaths\n";
    for (const auto& path : pluginManagerState_.searchPaths)
    {
        paths << "  - " << path << "\n";
    }

    paths << "\nFavorites\n";
    for (const auto& favorite : pluginManagerState_.favorites)
    {
        paths << "  - " << favorite << "\n";
    }

    paths << "\nBlacklist\n";
    for (const auto& blocked : pluginManagerState_.blacklist)
    {
        paths << "  - " << blocked << "\n";
    }

    paths << "\nThis window models plugin scan paths, database state, wrapper options and sandbox decisions.";
    return paths.str();
}

void UI::applyPluginSearchFilter()
{
    pluginManagerState_.filteredPlugins.clear();

    std::string needle = pluginManagerState_.searchText;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& plugin : pluginManagerState_.loadedPlugins)
    {
        std::string haystack = plugin.name + " " + plugin.vendor + " " + plugin.format;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (needle.empty() || haystack.find(needle) != std::string::npos)
        {
            pluginManagerState_.filteredPlugins.push_back(plugin);
        }
    }

    if (pluginManagerState_.selectedIndex >= pluginManagerState_.filteredPlugins.size())
    {
        pluginManagerState_.selectedIndex = 0;
    }
}

std::string UI::buildPluginManagerTableText() const
{
    std::ostringstream table;
    table << "Database View\r\n";
    table << "Name                Vendor          Fmt   Host           Lat\r\n";
    table << "-------------------------------------------------------------\r\n";

    if (pluginManagerState_.filteredPlugins.empty())
    {
        table << "<no plugins>\r\n";
        return table.str();
    }

    for (const auto& plugin : pluginManagerState_.filteredPlugins)
    {
        std::string name = plugin.name.substr(0, std::min<std::size_t>(18, plugin.name.size()));
        std::string vendor = plugin.vendor.substr(0, std::min<std::size_t>(14, plugin.vendor.size()));
        std::string fmt = plugin.format.substr(0, std::min<std::size_t>(5, plugin.format.size()));
        std::string host = plugin.processModeLabel.substr(0, std::min<std::size_t>(13, plugin.processModeLabel.size()));

        table
            << name << std::string(20 - name.size(), ' ')
            << vendor << std::string(16 - vendor.size(), ' ')
            << fmt << std::string(6 - fmt.size(), ' ')
            << host << std::string(15 - host.size(), ' ')
            << plugin.latencySamples << "\r\n";
    }

    return table.str();
}

bool UI::isPaneVisible(WorkspacePane pane) const
{
    switch (pane)
    {
    case WorkspacePane::Browser: return workspace_.browserVisible;
    case WorkspacePane::ChannelRack: return workspace_.channelRackVisible;
    case WorkspacePane::PianoRoll: return workspace_.pianoRollVisible;
    case WorkspacePane::Playlist: return workspace_.playlistVisible;
    case WorkspacePane::Mixer: return workspace_.mixerVisible;
    case WorkspacePane::Plugin: return workspace_.pluginVisible;
    default: return false;
    }
}

void UI::setPaneVisible(WorkspacePane pane, bool visible)
{
    switch (pane)
    {
    case WorkspacePane::Browser: workspace_.browserVisible = visible; break;
    case WorkspacePane::ChannelRack: workspace_.channelRackVisible = visible; break;
    case WorkspacePane::PianoRoll: workspace_.pianoRollVisible = visible; break;
    case WorkspacePane::Playlist: workspace_.playlistVisible = visible; break;
    case WorkspacePane::Mixer: workspace_.mixerVisible = visible; break;
    case WorkspacePane::Plugin: workspace_.pluginVisible = visible; break;
    default: break;
    }
}

UI::DockedPaneState* UI::findDockedPane(WorkspacePane pane)
{
    for (auto& entry : dockedPanes_)
    {
        if (entry.pane == pane)
        {
            return &entry;
        }
    }
    return nullptr;
}

UI::DockedPaneState* UI::findDockedPaneWindow(HWND hwnd)
{
    for (auto& entry : dockedPanes_)
    {
        if (entry.windowHandle == hwnd)
        {
            return &entry;
        }
    }
    return nullptr;
}

const UI::DockedPaneState* UI::findDockedPane(WorkspacePane pane) const
{
    for (const auto& entry : dockedPanes_)
    {
        if (entry.pane == pane)
        {
            return &entry;
        }
    }
    return nullptr;
}

const UI::DockedPaneState* UI::findDockedPaneWindow(HWND hwnd) const
{
    for (const auto& entry : dockedPanes_)
    {
        if (entry.windowHandle == hwnd)
        {
            return &entry;
        }
    }
    return nullptr;
}

void UI::detachPane(WorkspacePane pane)
{
    if (DockedPaneState* entry = findDockedPane(pane))
    {
        entry->detached = true;
    }
}

void UI::attachPane(WorkspacePane pane)
{
    if (DockedPaneState* entry = findDockedPane(pane))
    {
        entry->detached = false;
    }
}

std::string UI::paneTitle(WorkspacePane pane) const
{
    switch (pane)
    {
    case WorkspacePane::Browser: return "Browser";
    case WorkspacePane::ChannelRack: return "Channel Rack - Pattern " + std::to_string(workspace_.activePattern);
    case WorkspacePane::PianoRoll: return "Piano Roll";
    case WorkspacePane::Playlist: return "Playlist";
    case WorkspacePane::Mixer: return "Mixer - " + (visibleState_.project.projectName.empty() ? std::string("Untitled Project") : visibleState_.project.projectName);
    case WorkspacePane::Plugin: return "Plugin";
    default: return "Pane";
    }
}

UI::SurfaceKind UI::kindFromSurfaceHandle(HWND hwnd) const
{
    if (hwnd == browserPanel_) return SurfaceKind::Browser;
    if (hwnd == channelRackPanel_) return SurfaceKind::ChannelRack;
    if (hwnd == stepSequencerPanel_) return SurfaceKind::StepSequencer;
    if (hwnd == pianoRollPanel_) return SurfaceKind::PianoRoll;
    if (hwnd == playlistPanel_) return SurfaceKind::Playlist;
    if (hwnd == mixerPanel_) return SurfaceKind::Mixer;
    if (hwnd == pluginPanel_) return SurfaceKind::Plugin;
    return SurfaceKind::None;
}

void UI::paintSurface(HWND hwnd, SurfaceKind kind)
{
    PAINTSTRUCT paintStruct{};
    HDC dc = BeginPaint(hwnd, &paintStruct);

    RECT rect{};
    GetClientRect(hwnd, &rect);

    HBRUSH background = CreateSolidBrush(RGB(30, 33, 39));
    FillRect(dc, &rect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);

    switch (kind)
    {
    case SurfaceKind::Browser: paintBrowserSurface(dc, rect); break;
    case SurfaceKind::ChannelRack: paintChannelRackSurface(dc, rect); break;
    case SurfaceKind::StepSequencer: paintStepSequencerSurface(dc, rect); break;
    case SurfaceKind::PianoRoll: paintPianoRollSurface(dc, rect); break;
    case SurfaceKind::Playlist: paintPlaylistSurface(dc, rect); break;
    case SurfaceKind::Mixer: paintMixerSurface(dc, rect); break;
    case SurfaceKind::Plugin: paintPluginSurface(dc, rect); break;
    case SurfaceKind::None:
    default:
        break;
    }

    EndPaint(hwnd, &paintStruct);
}

void UI::paintBrowserSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Browser", "Docked browser");

    const int tabLeft = rect.left + 8;
    const int tabTop = rect.top + kSurfaceHeaderHeight + 6;
    const int clientWidth = static_cast<int>(rect.right - rect.left);
    const int tabWidth = std::max(60, (clientWidth - 16 - ((kBrowserTabCount - 1) * 6)) / kBrowserTabCount);
    const int contentTop = tabTop + kBrowserTabHeight + 10;
    const int footerHeight = 28;

    static constexpr const char* kBrowserTabs[] = {"Patterns", "Audio Clips", "Automation Clips"};
    for (int tabIndex = 0; tabIndex < kBrowserTabCount; ++tabIndex)
    {
        RECT tabRect{
            tabLeft + tabIndex * (tabWidth + 6),
            tabTop,
            tabLeft + tabIndex * (tabWidth + 6) + tabWidth,
            tabTop + kBrowserTabHeight};
        const bool selected = static_cast<std::size_t>(tabIndex) == workspace_.browserTabIndex;
        HBRUSH tabBrush = CreateSolidBrush(selected ? RGB(88, 104, 124) : RGB(42, 48, 58));
        FillRect(dc, &tabRect, tabBrush);
        DeleteObject(tabBrush);
        SetTextColor(dc, selected ? RGB(250, 250, 250) : RGB(178, 188, 202));
        DrawTextA(dc, kBrowserTabs[tabIndex], -1, &tabRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    int y = contentTop;
    for (std::size_t index = 0; index < workspaceModel_.browserEntries.size(); ++index)
    {
        if (y + kBrowserRowHeight > rect.bottom - footerHeight - 8)
        {
            break;
        }

        const BrowserEntry& entry = workspaceModel_.browserEntries[index];
        RECT itemRect{rect.left + 8, y, rect.right - 8, y + kBrowserRowHeight};
        const bool selected = static_cast<int>(index) == workspaceModel_.selectedBrowserIndex;
        HBRUSH rowBrush = CreateSolidBrush(selected ? RGB(55, 67, 82) : RGB(34, 39, 46));
        FillRect(dc, &itemRect, rowBrush);
        DeleteObject(rowBrush);

        RECT accentRect{itemRect.left, itemRect.top, itemRect.left + 4, itemRect.bottom};
        HBRUSH accentBrush = CreateSolidBrush(entry.favorite ? RGB(235, 167, 72) : RGB(74, 84, 98));
        FillRect(dc, &accentRect, accentBrush);
        DeleteObject(accentBrush);

        RECT categoryRect{itemRect.left + 12, itemRect.top + 4, itemRect.right - 12, itemRect.top + 17};
        SetTextColor(dc, RGB(150, 166, 184));
        DrawTextA(dc, entry.category.c_str(), -1, &categoryRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT labelRect{itemRect.left + 12, itemRect.top + 15, itemRect.right - 12, itemRect.top + 31};
        SetTextColor(dc, RGB(236, 239, 244));
        DrawTextA(dc, entry.label.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT subtitleRect{itemRect.left + 12, itemRect.top + 28, itemRect.right - 12, itemRect.bottom - 5};
        SetTextColor(dc, RGB(169, 178, 192));
        DrawTextA(dc, entry.subtitle.c_str(), -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        y += kBrowserRowHeight + 6;
    }

    RECT footerRect{rect.left + 8, rect.bottom - footerHeight, rect.right - 8, rect.bottom - 8};
    HBRUSH footerBrush = CreateSolidBrush(RGB(39, 45, 53));
    FillRect(dc, &footerRect, footerBrush);
    DeleteObject(footerBrush);
    SetTextColor(dc, RGB(174, 186, 200));
    DrawTextA(dc, browserDropTargetLabel().c_str(), -1, &footerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    drawSurfaceFrame(dc, rect, RGB(74, 90, 112));
}

void UI::paintChannelRackSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Channel Rack", "Detached window");
    const int rowHeight = 40;
    const int contentTop = rect.top + kSurfaceHeaderHeight + 8;
    const int nameWidth = 180;
    const int cellSize = 14;

    int y = contentTop;
    for (std::size_t index = 0; index < workspaceModel_.patternLanes.size(); ++index)
    {
        if (y + rowHeight > rect.bottom - 8)
        {
            break;
        }

        const PatternLaneState& lane = workspaceModel_.patternLanes[index];
        RECT rowRect{rect.left + 8, y, rect.right - 8, y + rowHeight};
        const bool selected = index == selectedTrackIndex_;
        HBRUSH rowBrush = CreateSolidBrush(selected ? RGB(73, 82, 68) : RGB(39, 44, 49));
        FillRect(dc, &rowRect, rowBrush);
        DeleteObject(rowBrush);

        RECT nameRect{rowRect.left + 12, rowRect.top + 4, rowRect.left + nameWidth, rowRect.top + 22};
        SetTextColor(dc, selected ? RGB(247, 247, 247) : RGB(224, 228, 232));
        DrawTextA(dc, lane.name.c_str(), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT metaRect{rowRect.left + 12, rowRect.top + 21, rowRect.left + nameWidth, rowRect.bottom - 6};
        const std::string metaText =
            "Pattern " + std::to_string(workspace_.activePattern) + " | Notes " + std::to_string(lane.notes.size());
        SetTextColor(dc, RGB(168, 179, 189));
        DrawTextA(dc, metaText.c_str(), -1, &metaRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        for (int step = 0; step < kStepCount; ++step)
        {
            const int cellX = rowRect.left + nameWidth + 26 + step * (cellSize + 4);
            RECT cellRect{cellX, rowRect.top + 9, cellX + cellSize, rowRect.top + 23};
            const bool enabled = step < static_cast<int>(lane.steps.size()) && lane.steps[static_cast<std::size_t>(step)].enabled;
            HBRUSH cellBrush = CreateSolidBrush(enabled ? RGB(235, 155, 49) : RGB(63, 70, 79));
            FillRect(dc, &cellRect, cellBrush);
            DeleteObject(cellBrush);
        }

        y += rowHeight + 6;
    }

    drawSurfaceFrame(dc, rect, RGB(90, 98, 72));
}

void UI::paintStepSequencerSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Step Sequencer", "Detached rack grid");
    drawSurfaceFrame(dc, rect, RGB(108, 88, 42));

    const int top = rect.top + 34;
    const int left = rect.left + 14;
    const int availableWidth = static_cast<int>(rect.right) - left - 14;
    const int cellSize = std::max<int>(16, availableWidth / kStepCount);
    const int laneCount = std::min(4, static_cast<int>(workspaceModel_.patternLanes.size()));

    for (int laneIndex = 0; laneIndex < laneCount; ++laneIndex)
    {
        const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
        const int y = top + laneIndex * 24;
        RECT labelRect{left, y, left + 90, y + 16};
        SetTextColor(dc, RGB(238, 238, 238));
        DrawTextA(dc, lane.name.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (int step = 0; step < kStepCount; ++step)
        {
            RECT cellRect{left + 96 + step * cellSize, y, left + 96 + (step + 1) * cellSize - 4, y + 16};
            const bool enabled = step < static_cast<int>(lane.steps.size()) && lane.steps[static_cast<std::size_t>(step)].enabled;
            HBRUSH brush = CreateSolidBrush(enabled ? RGB(235, 155, 49) : RGB(57, 62, 70));
            FillRect(dc, &cellRect, brush);
            DeleteObject(brush);
        }
    }

    RECT helpRect{rect.left + 14, rect.bottom - 28, rect.right - 14, rect.bottom - 8};
    SetTextColor(dc, RGB(212, 214, 218));
    DrawTextA(dc, "Click cells to toggle steps for the selected channel.", -1, &helpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void UI::paintPianoRollSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Piano Roll", "Pattern melody editor");
    RECT drawRect = rect;
    drawRect.top += 24;
    const int keyWidth = 48;
    const int laneHeight = 22;
    const int columns = kPlaylistCellCount;
    const int gridLeft = drawRect.left + keyWidth;
    const int gridTop = drawRect.top;

    HBRUSH keyBrush = CreateSolidBrush(RGB(42, 45, 52));
    RECT keyRect{drawRect.left, drawRect.top, drawRect.left + keyWidth, drawRect.bottom};
    FillRect(dc, &keyRect, keyBrush);
    DeleteObject(keyBrush);

    HPEN majorPen = CreatePen(PS_SOLID, 1, RGB(68, 77, 90));
    HPEN minorPen = CreatePen(PS_SOLID, 1, RGB(48, 54, 64));

    for (int lane = 0; lane < (drawRect.bottom - drawRect.top) / laneHeight; ++lane)
    {
        const int y = gridTop + (lane * laneHeight);
        SelectObject(dc, minorPen);
        MoveToEx(dc, drawRect.left, y, nullptr);
        LineTo(dc, drawRect.right, y);
    }

    for (int col = 0; col <= columns; ++col)
    {
        const int gridWidth = static_cast<int>(drawRect.right) - gridLeft;
        const int x = gridLeft + (col * std::max<int>(18, gridWidth / columns));
        SelectObject(dc, (col % 4 == 0) ? majorPen : minorPen);
        MoveToEx(dc, x, drawRect.top, nullptr);
        LineTo(dc, x, drawRect.bottom);
    }

    DeleteObject(majorPen);
    DeleteObject(minorPen);

    rebuildPianoVisuals(rect);

    HBRUSH noteBrush = CreateSolidBrush(RGB(233, 153, 44));
    HBRUSH selectedBrush = CreateSolidBrush(RGB(255, 214, 102));
    for (std::size_t index = 0; index < interactionState_.pianoNoteVisuals.size(); ++index)
    {
        const auto& note = interactionState_.pianoNoteVisuals[index];
        RECT noteRect{note.rect.x, note.rect.y, note.rect.x + note.rect.width, note.rect.y + note.rect.height};
        FillRect(dc, &noteRect, note.selected ? selectedBrush : noteBrush);
        RECT handleRect{noteRect.right - 5, noteRect.top, noteRect.right, noteRect.bottom};
        HBRUSH handleBrush = CreateSolidBrush(RGB(255, 235, 160));
        FillRect(dc, &handleRect, handleBrush);
        DeleteObject(handleBrush);
    }
    DeleteObject(noteBrush);
    DeleteObject(selectedBrush);

    if (interactionState_.marqueeActive && interactionState_.activeSurface == SurfaceKind::PianoRoll)
    {
        HPEN marqueePen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, marqueePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, interactionState_.marqueeRect.left, interactionState_.marqueeRect.top, interactionState_.marqueeRect.right, interactionState_.marqueeRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(marqueePen);
    }

    RECT textRect = drawRect;
    textRect.left += 8;
    textRect.top += 8;
    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextA(dc, buildPianoRollPanelText().c_str(), -1, &textRect, DT_LEFT | DT_TOP);
    drawSurfaceFrame(dc, rect, RGB(88, 100, 118));
}

void UI::paintPlaylistSurface(HDC dc, const RECT& rect)
{
    const std::string subtitle =
        std::string(workspace_.songMode ? "Song arrangement" : "Pattern preview") +
        " | Zoom " + currentZoomLabel(false) +
        " | Tool " + currentToolLabel(false);
    drawSurfaceHeader(dc, rect, "Playlist - Arrangement", subtitle);

    const int laneHeight = kPlaylistLaneHeight;
    const int timelineTop = rect.top + kSurfaceHeaderHeight;
    const int timelineHeight = kPlaylistTimelineHeight;
    const int gridTop = timelineTop + timelineHeight;
    const int leftInset = kPlaylistTrackHeaderWidth;
    const int columns = kPlaylistCellCount;
    const int laneCount = std::max<int>(10, static_cast<int>(workspaceModel_.patternLanes.size() + workspaceModel_.automationLanes.size()));
    const int playlistWidth = std::max<int>(120, static_cast<int>(rect.right) - leftInset);
    const int columnWidth = std::max<int>(24, playlistWidth / columns);

    RECT trackHeaderRect{rect.left, gridTop, rect.left + leftInset, rect.bottom};
    HBRUSH headerBrush = CreateSolidBrush(RGB(56, 63, 72));
    FillRect(dc, &trackHeaderRect, headerBrush);
    DeleteObject(headerBrush);

    RECT timelineRect{rect.left + leftInset, timelineTop, rect.right, gridTop};
    HBRUSH timelineBrush = CreateSolidBrush(RGB(53, 60, 69));
    FillRect(dc, &timelineRect, timelineBrush);
    DeleteObject(timelineBrush);

    RECT gutterRect{rect.left, timelineTop, rect.left + leftInset, gridTop};
    HBRUSH gutterBrush = CreateSolidBrush(RGB(49, 56, 66));
    FillRect(dc, &gutterRect, gutterBrush);
    DeleteObject(gutterBrush);

    HPEN majorPen = CreatePen(PS_SOLID, 1, RGB(74, 84, 98));
    HPEN minorPen = CreatePen(PS_SOLID, 1, RGB(48, 54, 64));

    for (int lane = 0; lane < laneCount; ++lane)
    {
        const int y = gridTop + (lane * laneHeight);
        RECT laneRect{rect.left + leftInset, y, rect.right, y + laneHeight};
        HBRUSH laneBrush = CreateSolidBrush((lane % 2) == 0 ? RGB(37, 42, 50) : RGB(33, 38, 46));
        FillRect(dc, &laneRect, laneBrush);
        DeleteObject(laneBrush);

        RECT labelRect{rect.left + 12, y + 3, rect.left + leftInset - 12, y + laneHeight - 3};
        const bool isTrackLane = lane < static_cast<int>(workspaceModel_.patternLanes.size());
        const int automationIndex = lane - static_cast<int>(workspaceModel_.patternLanes.size());
        const bool isAutomationLane =
            !isTrackLane && automationIndex >= 0 && automationIndex < static_cast<int>(workspaceModel_.automationLanes.size());
        const std::string laneName =
            isTrackLane
                ? workspaceModel_.patternLanes[static_cast<std::size_t>(lane)].name
                : (isAutomationLane
                       ? ("Automation " + workspaceModel_.automationLanes[static_cast<std::size_t>(automationIndex)].target)
                       : ("Track " + std::to_string(lane + 1)));
        SetTextColor(dc, isAutomationLane ? RGB(226, 165, 194) : RGB(215, 221, 228));
        DrawTextA(dc, laneName.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(dc, minorPen);
        MoveToEx(dc, rect.left, y, nullptr);
        LineTo(dc, rect.right, y);
    }

    for (int col = 0; col <= columns; ++col)
    {
        const int x = rect.left + leftInset + (col * columnWidth);
        SelectObject(dc, (col % 4 == 0) ? majorPen : minorPen);
        MoveToEx(dc, x, timelineTop, nullptr);
        LineTo(dc, x, rect.bottom);

        if (col < columns)
        {
            RECT beatRect{x + 4, timelineTop + 5, x + 32, gridTop - 4};
            SetTextColor(dc, (col % 4 == 0) ? RGB(222, 228, 236) : RGB(137, 148, 162));
            const std::string beatLabel = std::to_string(col + 1);
            DrawTextA(dc, beatLabel.c_str(), -1, &beatRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    DeleteObject(majorPen);
    DeleteObject(minorPen);

    for (const auto& marker : workspaceModel_.markers)
    {
        const int x = rect.left + leftInset + (marker.timelineCell * columnWidth);
        HPEN markerPen = CreatePen(PS_SOLID, 1, RGB(255, 207, 84));
        HGDIOBJ oldPen = SelectObject(dc, markerPen);
        MoveToEx(dc, x, timelineTop, nullptr);
        LineTo(dc, x, rect.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(markerPen);

        RECT markerRect{x + 4, timelineTop + 2, x + 72, timelineTop + 18};
        SetTextColor(dc, RGB(255, 222, 128));
        DrawTextA(dc, marker.name.c_str(), -1, &markerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    rebuildPlaylistVisuals(rect);

    for (const auto& clip : interactionState_.playlistClipVisuals)
    {
        if (clip.automation)
        {
            continue;
        }

        RECT clipRect{clip.rect.x, clip.rect.y, clip.rect.x + clip.rect.width, clip.rect.y + clip.rect.height};
        const bool isAudioClip = clip.clipType == "Audio";
        HBRUSH clipBrush = CreateSolidBrush(
            clip.selected
                ? (isAudioClip ? RGB(150, 192, 128) : RGB(139, 205, 244))
                : (isAudioClip ? RGB(98, 138, 81) : RGB(79, 156, 210)));
        FillRect(dc, &clipRect, clipBrush);
        DeleteObject(clipBrush);
        RECT leftHandle{clipRect.left, clipRect.top, clipRect.left + 6, clipRect.bottom};
        RECT rightHandle{clipRect.right - 6, clipRect.top, clipRect.right, clipRect.bottom};
        HBRUSH handleBrush = CreateSolidBrush(isAudioClip ? RGB(220, 235, 207) : RGB(220, 240, 255));
        FillRect(dc, &leftHandle, handleBrush);
        FillRect(dc, &rightHandle, handleBrush);
        DeleteObject(handleBrush);

        RECT clipLabelRect{clipRect.left + 10, clipRect.top + 2, clipRect.right - 8, clipRect.bottom - 2};
        SetTextColor(dc, RGB(18, 21, 28));
        DrawTextA( dc, clip.label.c_str(), -1, &clipLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    HPEN automationPen = CreatePen(PS_SOLID, 2, RGB(255, 216, 238));
    for (const auto& clip : interactionState_.playlistClipVisuals)
    {
        if (!clip.automation)
        {
            continue;
        }

        auto automationIt = std::find_if(
            workspaceModel_.automationLanes.begin(),
            workspaceModel_.automationLanes.end(),
            [&](const AutomationLaneState& automation) { return automation.clipId == clip.clipId; });
        if (automationIt == workspaceModel_.automationLanes.end())
        {
            continue;
        }

        RECT automationRect{clip.rect.x, clip.rect.y, clip.rect.x + clip.rect.width, clip.rect.y + clip.rect.height};
        HBRUSH automationBrush = CreateSolidBrush(automationIt->selected ? RGB(255, 160, 205) : RGB(228, 110, 168));
        FillRect(dc, &automationRect, automationBrush);
        DeleteObject(automationBrush);

        RECT rightHandle{automationRect.right - 6, automationRect.top, automationRect.right, automationRect.bottom};
        HBRUSH handleBrush = CreateSolidBrush(RGB(255, 232, 243));
        FillRect(dc, &rightHandle, handleBrush);
        DeleteObject(handleBrush);

        HGDIOBJ oldPen = SelectObject(dc, automationPen);
        const int pointCount = static_cast<int>(automationIt->values.size());
        for (int point = 0; point < pointCount; ++point)
        {
            const int pointX = automationRect.left + ((automationRect.right - automationRect.left - 6) * point) / std::max(1, pointCount - 1) + 3;
            const int pointY = automationRect.bottom - 4 - ((automationRect.bottom - automationRect.top - 8) * automationIt->values[static_cast<std::size_t>(point)]) / 100;
            if (point == 0)
            {
                MoveToEx(dc, pointX, pointY, nullptr);
            }
            else
            {
                LineTo(dc, pointX, pointY);
            }
        }
        SelectObject(dc, oldPen);

        RECT labelRect{automationRect.left + 4, automationRect.top + 2, automationRect.right - 4, automationRect.top + 18};
        SetTextColor(dc, RGB(40, 22, 34));
        DrawTextA(dc, automationIt->target.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DeleteObject(automationPen);

    if (interactionState_.marqueeActive && interactionState_.activeSurface == SurfaceKind::Playlist)
    {
        HPEN marqueePen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, marqueePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, interactionState_.marqueeRect.left, interactionState_.marqueeRect.top, interactionState_.marqueeRect.right, interactionState_.marqueeRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(marqueePen);
    }

    drawSurfaceFrame(dc, rect, RGB(84, 96, 116));
}

void UI::paintMixerSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Mixer", "Routing, FX and meters");
    const int stripWidth = 84;
    const int stripGap = 12;
    const int meterTop = rect.top + 40;
    const int meterBottom = rect.bottom - 18;
    int x = rect.left + 12;

    for (std::size_t stripIndex = 0; stripIndex < workspaceModel_.mixerStrips.size(); ++stripIndex)
    {
        const MixerStripState& strip = workspaceModel_.mixerStrips[stripIndex];
        RECT stripRect{x, rect.top + 8, x + stripWidth, rect.bottom - 8};
        HBRUSH stripBrush = CreateSolidBrush(RGB(44, 48, 56));
        FillRect(dc, &stripRect, stripBrush);
        DeleteObject(stripBrush);

        RECT meterRect{x + 28, meterTop, x + 50, meterBottom};
        HBRUSH meterBg = CreateSolidBrush(RGB(25, 26, 29));
        FillRect(dc, &meterRect, meterBg);
        DeleteObject(meterBg);

        const int fillHeight = ((meterBottom - meterTop) * strip.peakLevel) / 100;
        RECT fillRect{meterRect.left, meterBottom - fillHeight, meterRect.right, meterBottom};
        HBRUSH fillBrush = CreateSolidBrush(strip.name == "Master" ? RGB(239, 178, 52) : RGB(105, 230, 120));
        FillRect(dc, &fillRect, fillBrush);
        DeleteObject(fillBrush);

        RECT titleRect{x + 6, rect.bottom - 28, x + stripWidth - 6, rect.bottom - 8};
        SetTextColor(dc, RGB(232, 232, 232));
        DrawTextA(dc, strip.name.c_str(), -1, &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT fxRect{x + 6, rect.top + 10, x + stripWidth - 6, rect.top + 34};
        std::string fxLabel = strip.effects.empty() ? "Empty" : strip.effects.front();
        SetTextColor(dc, RGB(170, 180, 192));
        DrawTextA(dc, fxLabel.c_str(), -1, &fxRect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        x += stripWidth + stripGap;
    }
    drawSurfaceFrame(dc, rect, RGB(94, 104, 118));
}

void UI::paintPluginSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Plugin / Channel Settings", "Wrapper and macro controls");
    RECT drawRect = rect;
    drawRect.top += 24;
    HBRUSH moduleBrush = CreateSolidBrush(RGB(48, 54, 64));
    RECT upper{rect.left + 12, rect.top + 12, rect.right - 12, rect.top + 72};
    FillRect(dc, &upper, moduleBrush);
    RECT lower{rect.left + 12, rect.top + 86, rect.right - 12, rect.bottom - 12};
    FillRect(dc, &lower, moduleBrush);
    DeleteObject(moduleBrush);

    SetTextColor(dc, RGB(236, 236, 236));
    DrawTextA(dc, buildPluginPanelText().c_str(), -1, &drawRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    drawSurfaceFrame(dc, rect, RGB(88, 98, 110));
}

void UI::handleSurfaceMouseDown(HWND hwnd, SurfaceKind kind, int x, int y)
{
    workspace_.focusedPane =
        kind == SurfaceKind::Browser ? WorkspacePane::Browser :
        kind == SurfaceKind::ChannelRack ? WorkspacePane::ChannelRack :
        kind == SurfaceKind::StepSequencer ? WorkspacePane::ChannelRack :
        kind == SurfaceKind::PianoRoll ? WorkspacePane::PianoRoll :
        kind == SurfaceKind::Playlist ? WorkspacePane::Playlist :
        kind == SurfaceKind::Mixer ? WorkspacePane::Mixer :
        WorkspacePane::Plugin;

    interactionState_.activeSurface = kind;
    interactionState_.mouseDown = true;
    interactionState_.dragStart = POINT{x, y};
    interactionState_.dragCurrent = POINT{x, y};
    interactionState_.marqueeActive = false;
    SetCapture(hwnd);

    if (kind == SurfaceKind::Playlist)
    {
        bool hitClip = false;
        interactionState_.draggingClip = false;
        interactionState_.resizingClipLeft = false;
        interactionState_.resizingClipRight = false;
        interactionState_.editingAutomationPoint = false;
        interactionState_.selectedClipId = 0;
        interactionState_.selectedAutomationLaneIndex = static_cast<std::size_t>(-1);
        interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(-1);
        for (auto& automation : workspaceModel_.automationLanes)
        {
            automation.selected = false;
            automation.selectedPoint = static_cast<std::size_t>(-1);
        }
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            const bool hit =
                x >= clip.rect.x && x <= (clip.rect.x + clip.rect.width) &&
                y >= clip.rect.y && y <= (clip.rect.y + clip.rect.height);
            clip.selected = hit;
            if (hit)
            {
                hitClip = true;
                interactionState_.selectedClipId = clip.clipId;
                const bool leftEdge = x <= (clip.rect.x + 6);
                const bool rightEdge = x >= (clip.rect.x + clip.rect.width - 6);
                interactionState_.resizingClipLeft = leftEdge && clip.clipId < 4000;
                interactionState_.resizingClipRight = rightEdge;
                interactionState_.draggingClip = !interactionState_.resizingClipLeft && !interactionState_.resizingClipRight;
                if (clip.clipId >= 4000)
                {
                    for (std::size_t automationIndex = 0; automationIndex < workspaceModel_.automationLanes.size(); ++automationIndex)
                    {
                        auto& automation = workspaceModel_.automationLanes[automationIndex];
                        if (automation.clipId == clip.clipId)
                        {
                            automation.selected = true;
                            interactionState_.selectedAutomationLaneIndex = automationIndex;
                            const int pointCount = static_cast<int>(automation.values.size());
                            const int width = std::max(1, clip.rect.width - 6);
                            for (int point = 0; point < pointCount; ++point)
                            {
                                const int pointX = clip.rect.x + (width * point) / std::max(1, pointCount - 1) + 3;
                                const int pointY = clip.rect.y + clip.rect.height - 4 - ((clip.rect.height - 8) * automation.values[static_cast<std::size_t>(point)]) / 100;
                                if (std::abs(x - pointX) <= 6 && std::abs(y - pointY) <= 6)
                                {
                                    interactionState_.editingAutomationPoint = true;
                                    interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(point);
                                    automation.selectedPoint = static_cast<std::size_t>(point);
                                    interactionState_.draggingClip = false;
                                    interactionState_.resizingClipRight = false;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (!hitClip && !interactionState_.editingAutomationPoint)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::PianoRoll)
    {
        bool hitNote = false;
        interactionState_.draggingNote = false;
        interactionState_.resizingNote = false;
        interactionState_.selectedNoteIndex = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < interactionState_.pianoNoteVisuals.size(); ++index)
        {
            auto& note = interactionState_.pianoNoteVisuals[index];
            const bool hit =
                x >= note.rect.x && x <= (note.rect.x + note.rect.width) &&
                y >= note.rect.y && y <= (note.rect.y + note.rect.height);
            note.selected = hit;
            if (hit)
            {
                hitNote = true;
                interactionState_.resizingNote = x >= (note.rect.x + note.rect.width - 6);
                interactionState_.draggingNote = !interactionState_.resizingNote;
                interactionState_.selectedNoteIndex = index;
            }
        }
        if (!hitNote)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::Browser)
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int tabTop = kSurfaceHeaderHeight + 6;
        const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
        const int tabWidth = std::max(60, (clientWidth - 16 - ((kBrowserTabCount - 1) * 6)) / kBrowserTabCount);
        for (int tabIndex = 0; tabIndex < kBrowserTabCount; ++tabIndex)
        {
            const int tabLeft = 8 + tabIndex * (tabWidth + 6);
            if (x >= tabLeft && x <= (tabLeft + tabWidth) && y >= tabTop && y <= (tabTop + kBrowserTabHeight))
            {
                workspace_.browserTabIndex = static_cast<std::size_t>(tabIndex);
                workspaceModel_.selectedBrowserIndex = 0;
                rebuildBrowserEntries();
                interactionState_.browserDragActive = false;
                interactionState_.mouseDown = false;
                ReleaseCapture();
                invalidateSurface(hwnd);
                return;
            }
        }

        interactionState_.browserDragActive = false;
        const int browserIndex = std::max(0, (y - (tabTop + kBrowserTabHeight + 10)) / (kBrowserRowHeight + 6));
        if (!workspaceModel_.browserEntries.empty() &&
            browserIndex < static_cast<int>(workspaceModel_.browserEntries.size()))
        {
            interactionState_.browserDragActive = true;
            interactionState_.selectedBrowserItemIndex = static_cast<std::size_t>(browserIndex);
            workspaceModel_.selectedBrowserIndex = browserIndex;
        }
    }
    else if (kind == SurfaceKind::ChannelRack)
    {
        const int laneIndex = clampValue((y - 30) / 30, 0, std::max(0, static_cast<int>(workspaceModel_.patternLanes.size()) - 1));
        selectedTrackIndex_ = static_cast<std::size_t>(laneIndex);
        workspaceModel_.activeChannelIndex = laneIndex;
    }
    else if (kind == SurfaceKind::StepSequencer && !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int clientWidth = static_cast<int>(clientRect.right);
        const int stepWidth = std::max<int>(16, std::max<int>(1, clientWidth - 110) / kStepCount);
        const int laneIndex = clampValue((y - 34) / 24, 0, std::max(0, static_cast<int>(workspaceModel_.patternLanes.size()) - 1));
        const int stepIndex = clampValue((x - 110) / stepWidth, 0, kStepCount - 1);
        selectedTrackIndex_ = static_cast<std::size_t>(laneIndex);
        workspaceModel_.activeChannelIndex = laneIndex;
        auto& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
        if (stepIndex < static_cast<int>(lane.steps.size()))
        {
            lane.steps[static_cast<std::size_t>(stepIndex)].enabled = !lane.steps[static_cast<std::size_t>(stepIndex)].enabled;
            lane.steps[static_cast<std::size_t>(stepIndex)].velocity =
                lane.steps[static_cast<std::size_t>(stepIndex)].enabled ? 108 : 0;
            workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].lanes[static_cast<std::size_t>(laneIndex)] = lane;
        }
    }

    invalidateSurface(hwnd);
}

void UI::handleSurfaceMouseMove(HWND hwnd, SurfaceKind kind, int x, int y, WPARAM flags)
{
    if (!interactionState_.mouseDown || (flags & MK_LBUTTON) == 0)
    {
        return;
    }

    interactionState_.dragCurrent = POINT{x, y};

    if (kind == SurfaceKind::Playlist && interactionState_.draggingClip)
    {
        const int dx = x - interactionState_.dragStart.x;
        const int dy = y - interactionState_.dragStart.y;
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            if (clip.clipId == interactionState_.selectedClipId)
            {
                clip.rect.x += dx;
                clip.rect.y += dy;
                interactionState_.dragStart.x = x;
                interactionState_.dragStart.y = y;
                break;
            }
        }
    }
    else if (kind == SurfaceKind::Playlist && (interactionState_.resizingClipLeft || interactionState_.resizingClipRight))
    {
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            if (clip.clipId != interactionState_.selectedClipId)
            {
                continue;
            }

            if (interactionState_.resizingClipLeft)
            {
                const int right = clip.rect.x + clip.rect.width;
                clip.rect.x = std::min(x, right - 24);
                clip.rect.width = std::max<int>(24, right - clip.rect.x);
            }
            else
            {
                clip.rect.width = std::max<int>(24, clip.rect.width + (x - static_cast<int>(interactionState_.dragStart.x)));
                interactionState_.dragStart.x = x;
            }
            break;
        }
    }
    else if (kind == SurfaceKind::Playlist && interactionState_.editingAutomationPoint &&
             interactionState_.selectedAutomationLaneIndex < workspaceModel_.automationLanes.size() &&
             interactionState_.selectedAutomationPointIndex < workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex].values.size())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int laneHeight = kPlaylistLaneHeight;
        const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
        auto& automation = workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex];
        const int clipY = timelineHeight + 6 + (automation.lane * laneHeight);
        const int normalized = clampValue(100 - ((y - clipY) * 100) / std::max<int>(10, laneHeight - 10), 0, 100);
        automation.values[interactionState_.selectedAutomationPointIndex] = normalized;
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.draggingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size())
    {
        auto& note = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        note.rect.x += (x - interactionState_.dragStart.x);
        note.rect.y += (y - interactionState_.dragStart.y);
        interactionState_.dragStart = POINT{x, y};
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.resizingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size())
    {
        auto& note = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        note.rect.width = std::max<int>(18, note.rect.width + (x - static_cast<int>(interactionState_.dragStart.x)));
        interactionState_.dragStart = POINT{x, y};
    }
    else if (interactionState_.marqueeActive)
    {
        interactionState_.marqueeRect.left = std::min<int>(static_cast<int>(interactionState_.dragStart.x), x);
        interactionState_.marqueeRect.top = std::min<int>(static_cast<int>(interactionState_.dragStart.y), y);
        interactionState_.marqueeRect.right = std::max<int>(static_cast<int>(interactionState_.dragStart.x), x);
        interactionState_.marqueeRect.bottom = std::max<int>(static_cast<int>(interactionState_.dragStart.y), y);
    }

    invalidateSurface(hwnd);
}

void UI::handleSurfaceMouseUp(HWND hwnd, SurfaceKind kind, int x, int y)
{
    if (kind == SurfaceKind::PianoRoll && interactionState_.draggingNote &&
        interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size() &&
        !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 48;
        const int laneHeight = 22;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

        UiNoteVisual& noteVisual = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        PianoNoteState& noteState =
            workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                .notes[interactionState_.selectedNoteIndex];

        noteState.step = clampValue((noteVisual.rect.x - keyWidth - 20) / stepWidth, 0, kPlaylistCellCount - 1);
        const int laneFromTop = (noteVisual.rect.y - 24) / laneHeight;
        noteState.lane = clampValue((kPianoLaneCount - 1) - laneFromTop, 0, kPianoLaneCount - 1);
        noteState.selected = true;
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
            .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
            .notes[interactionState_.selectedNoteIndex] = noteState;
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.resizingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size() &&
             !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 48;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

        UiNoteVisual& noteVisual = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        PianoNoteState& noteState =
            workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                .notes[interactionState_.selectedNoteIndex];
        noteState.length = std::max<int>(1, noteVisual.rect.width / std::max<int>(1, stepWidth));
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
            .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
            .notes[interactionState_.selectedNoteIndex] = noteState;
    }

    if (kind == SurfaceKind::PianoRoll && !workspaceModel_.patternLanes.empty() &&
        !interactionState_.draggingNote && !interactionState_.resizingNote &&
        !interactionState_.marqueeActive && interactionState_.selectedNoteIndex == static_cast<std::size_t>(-1))
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 48;
        const int laneHeight = 22;
        const int gridTop = 24;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);
        if (x > keyWidth && y > gridTop)
        {
            const int laneFromTop = (y - gridTop) / laneHeight;
            const int lane = clampValue((kPianoLaneCount - 1) - laneFromTop, 0, kPianoLaneCount - 1);
            const int step = clampValue((x - keyWidth) / stepWidth, 0, kPlaylistCellCount - 1);
            auto& laneState = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)];
            laneState.notes.push_back(PianoNoteState{lane, step, 2, 96, false, false, false});
            workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
                .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)] = laneState;
        }
    }

    if (interactionState_.mouseDown && interactionState_.marqueeActive)
    {
        if (kind == SurfaceKind::Playlist)
        {
            for (auto& clip : interactionState_.playlistClipVisuals)
            {
                const RECT clipRect{
                    clip.rect.x,
                    clip.rect.y,
                    clip.rect.x + clip.rect.width,
                    clip.rect.y + clip.rect.height};
                RECT overlap{};
                clip.selected = IntersectRect(&overlap, &clipRect, &interactionState_.marqueeRect) != 0;
            }
        }
        else if (kind == SurfaceKind::PianoRoll)
        {
            for (auto& note : interactionState_.pianoNoteVisuals)
            {
                const RECT noteRect{
                    note.rect.x,
                    note.rect.y,
                    note.rect.x + note.rect.width,
                    note.rect.y + note.rect.height};
                RECT overlap{};
                note.selected = IntersectRect(&overlap, &noteRect, &interactionState_.marqueeRect) != 0;
            }
            if (workspaceModel_.activeChannelIndex >= 0 &&
                workspaceModel_.activeChannelIndex < static_cast<int>(workspaceModel_.patternLanes.size()))
            {
                auto& notes = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)].notes;
                for (std::size_t index = 0; index < notes.size() && index < interactionState_.pianoNoteVisuals.size(); ++index)
                {
                    notes[index].selected = interactionState_.pianoNoteVisuals[index].selected;
                }
                workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
                    .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                    .notes = notes;
            }
        }
    }

    if (kind == SurfaceKind::Playlist && interactionState_.selectedClipId != 0)
    {
        const auto clipIt = std::find_if(
            interactionState_.playlistClipVisuals.begin(),
            interactionState_.playlistClipVisuals.end(),
            [&](const UiClipVisual& clip) { return clip.clipId == interactionState_.selectedClipId; });

        if (clipIt != interactionState_.playlistClipVisuals.end() &&
            (clipIt->clipId >= 4000 || !visibleState_.project.tracks.empty()))
        {
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const int laneHeight = kPlaylistLaneHeight;
            const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
            const int leftInset = kPlaylistTrackHeaderWidth;
            const int clientWidth = static_cast<int>(clientRect.right);
            const int columnWidth = std::max<int>(24, std::max<int>(1, clientWidth - leftInset) / kPlaylistCellCount);
            const int targetLane = std::max<int>(0, (clipIt->rect.y - timelineHeight) / laneHeight);
            const int targetStartCell =
                clampValue((clipIt->rect.x - leftInset) / std::max<int>(1, columnWidth), 0, kPlaylistCellCount - 1);
            const int targetLengthCells =
                std::max<int>(2, clipIt->rect.width / std::max<int>(1, columnWidth));

            if (clipIt->clipId >= 4000)
            {
                for (auto& automation : workspaceModel_.automationLanes)
                {
                    if (automation.clipId == clipIt->clipId)
                    {
                        automation.lane = targetLane;
                        automation.startCell = targetStartCell;
                        automation.lengthCells = targetLengthCells;
                        automation.selected = true;
                        break;
                    }
                }
            }
            else
            {
                const int safeLane = clampValue(targetLane, 0, static_cast<int>(visibleState_.project.tracks.size() - 1));
                const double startTime = static_cast<double>(targetStartCell) / 2.0;
                for (auto& block : workspaceModel_.playlistBlocks)
                {
                    if (block.clipId == clipIt->clipId)
                    {
                        block.lane = safeLane;
                        block.startCell = targetStartCell;
                        block.lengthCells = targetLengthCells;
                        break;
                    }
                }

                AudioEngine::EngineCommand command{};
                command.type = AudioEngine::CommandType::MoveClip;
                command.uintValue = clipIt->clipId;
                command.secondaryUintValue = visibleState_.project.tracks[static_cast<std::size_t>(safeLane)].trackId;
                command.doubleValue = startTime;
                engine_.postCommand(command);
            }
        }
    }

    if (interactionState_.browserDragActive)
    {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        HWND targetWindow = WindowFromPoint(screenPoint);
        const SurfaceKind targetKind = kindFromSurfaceHandle(targetWindow);

        const std::string droppedName =
            workspaceModel_.browserEntries.empty()
                ? (currentBrowserTabLabel() + " Item " + std::to_string(interactionState_.selectedBrowserItemIndex + 1))
                : workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)].label;

        if (targetKind == SurfaceKind::Playlist && visibleState_.selection.selectedTrackId != 0)
        {
            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::AddClipToTrack;
            command.uintValue = visibleState_.selection.selectedTrackId;
            command.textValue = droppedName;
            engine_.postCommand(command);
        }
        else if (targetKind == SurfaceKind::ChannelRack)
        {
            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::AddTrack;
            command.textValue = droppedName;
            engine_.postCommand(command);
        }
    }

    interactionState_.mouseDown = false;
    interactionState_.draggingClip = false;
    interactionState_.draggingNote = false;
    interactionState_.resizingClipLeft = false;
    interactionState_.resizingClipRight = false;
    interactionState_.resizingNote = false;
    interactionState_.editingAutomationPoint = false;
    interactionState_.browserDragActive = false;
    interactionState_.marqueeActive = false;
    interactionState_.selectedAutomationLaneIndex = static_cast<std::size_t>(-1);
    interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(-1);
    ReleaseCapture();
    invalidateSurface(hwnd);
}

void UI::rebuildPlaylistVisuals(const RECT& rect)
{
    interactionState_.playlistClipVisuals.clear();
    const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
    const int laneHeight = kPlaylistLaneHeight;
    const int leftInset = kPlaylistTrackHeaderWidth;
    const int columnWidth = std::max<int>(24, (static_cast<int>(rect.right) - leftInset) / kPlaylistCellCount);

    for (const auto& block : workspaceModel_.playlistBlocks)
    {
        UiClipVisual visual{};
        visual.clipId = block.clipId;
        visual.trackId = block.trackId;
        visual.label = block.label;
        visual.clipType = block.clipType;
        visual.rect.x = leftInset + (block.startCell * columnWidth);
        visual.rect.y = timelineHeight + 6 + (block.lane * laneHeight);
        visual.rect.width = std::max(42, block.lengthCells * columnWidth - 6);
        visual.rect.height = laneHeight - 10;
        visual.selected = block.selected || (block.clipId == interactionState_.selectedClipId);
        visual.automation = false;
        visual.resizeLeftHot = false;
        visual.resizeRightHot = false;
        visual.rect.x = clampValue(visual.rect.x, leftInset, rect.right - 50);
        visual.rect.width = std::min<int>(visual.rect.width, std::max<int>(30, static_cast<int>(rect.right) - visual.rect.x - 6));
        interactionState_.playlistClipVisuals.push_back(visual);
    }

    for (const auto& automation : workspaceModel_.automationLanes)
    {
        UiClipVisual visual{};
        visual.clipId = automation.clipId;
        visual.trackId = 0;
        visual.label = automation.target;
        visual.clipType = "Automation";
        visual.rect.x = leftInset + (automation.startCell * columnWidth);
        visual.rect.y = timelineHeight + 6 + (automation.lane * laneHeight);
        visual.rect.width = std::max(42, automation.lengthCells * columnWidth - 6);
        visual.rect.height = laneHeight - 10;
        visual.selected = automation.selected || (automation.clipId == interactionState_.selectedClipId);
        visual.automation = true;
        visual.resizeLeftHot = false;
        visual.resizeRightHot = false;
        visual.rect.x = clampValue(visual.rect.x, leftInset, rect.right - 50);
        visual.rect.width = std::min<int>(visual.rect.width, std::max<int>(30, static_cast<int>(rect.right) - visual.rect.x - 6));
        interactionState_.playlistClipVisuals.push_back(visual);
    }
}

void UI::rebuildPianoVisuals(const RECT& rect)
{
    interactionState_.pianoNoteVisuals.clear();
    const int keyWidth = 48;
    const int laneHeight = 22;
    const int usableWidth = std::max<int>(1, static_cast<int>(rect.right) - keyWidth);
    const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

    if (workspaceModel_.patternLanes.empty())
    {
        return;
    }

    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)];
    for (std::size_t index = 0; index < lane.notes.size(); ++index)
    {
        const PianoNoteState& source = lane.notes[index];
        UiNoteVisual note{};
        note.lane = source.lane;
        note.step = source.step;
        note.rect.x = keyWidth + 20 + (source.step * stepWidth);
        note.rect.y = 24 + ((kPianoLaneCount - 1 - source.lane) * laneHeight);
        note.rect.width = std::max(18, source.length * stepWidth - 4);
        note.rect.height = laneHeight - 4;
        note.selected = source.selected || (interactionState_.selectedNoteIndex == index);
        note.resizeRightHot = false;
        note.rect.x = clampValue(note.rect.x, keyWidth + 2, rect.right - 60);
        interactionState_.pianoNoteVisuals.push_back(note);
    }
}

int UI::clampValue(int value, int minValue, int maxValue) const
{
    return std::max(minValue, std::min(value, maxValue));
}

void UI::setStaticText(HWND control, const std::string& text) const
{
    if (control != nullptr)
    {
        SetWindowTextA(control, text.c_str());
    }
}

void UI::syncWorkspaceModel()
{
    workspace_.browserTabIndex = std::min<std::size_t>(workspace_.browserTabIndex, kBrowserTabCount - 1);
    ensurePatternBank();
    rebuildPatternLanes();
    rebuildPlaylistBlocks();
    rebuildMixerStrips();
    rebuildChannelSettings();
    rebuildAutomationLanes();
    rebuildBrowserEntries();

    if (!workspaceModel_.patterns.empty())
    {
        workspaceModel_.selectedPatternIndex =
            clampValue(workspace_.activePattern - 1, 0, static_cast<int>(workspaceModel_.patterns.size() - 1));
        workspace_.activePattern = workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].patternNumber;
    }
    else
    {
        workspaceModel_.selectedPatternIndex = 0;
        workspace_.activePattern = 1;
    }

    if (!workspaceModel_.patternLanes.empty())
    {
        workspaceModel_.activeChannelIndex =
            clampValue(static_cast<int>(selectedTrackIndex_), 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    }
    else
    {
        workspaceModel_.activeChannelIndex = 0;
    }

    if (!workspaceModel_.browserEntries.empty())
    {
        workspaceModel_.selectedBrowserIndex =
            clampValue(workspaceModel_.selectedBrowserIndex, 0, static_cast<int>(workspaceModel_.browserEntries.size() - 1));
    }
    else
    {
        workspaceModel_.selectedBrowserIndex = 0;
    }
}

void UI::rebuildBrowserEntries()
{
    workspaceModel_.browserEntries.clear();

    auto addEntry = [&](std::string category, std::string label, std::string subtitle, bool favorite = false)
    {
        workspaceModel_.browserEntries.push_back(BrowserEntry{std::move(category), std::move(label), std::move(subtitle), favorite});
    };

    switch (std::min<std::size_t>(workspace_.browserTabIndex, kBrowserTabCount - 1))
    {
    case 0:
        for (const auto& pattern : workspaceModel_.patterns)
        {
            int laneCountWithSteps = 0;
            for (const auto& lane : pattern.lanes)
            {
                if (std::any_of(
                        lane.steps.begin(),
                        lane.steps.end(),
                        [](const ChannelStepState& step) { return step.enabled; }))
                {
                    ++laneCountWithSteps;
                }
            }

            addEntry(
                "Pattern",
                pattern.name,
                std::to_string(pattern.lanes.size()) + " channels | " +
                    std::to_string(laneCountWithSteps) + " active step lanes | " +
                    std::to_string(pattern.lengthInBars) + " bars",
                pattern.patternNumber == workspace_.activePattern);
        }
        break;
    case 1:
    {
        bool addedAudio = false;
        for (const auto& block : workspaceModel_.playlistBlocks)
        {
            if (block.clipType != "Audio")
            {
                continue;
            }

            addedAudio = true;
            addEntry(
                "Audio",
                block.label,
                "Playlist lane " + std::to_string(block.lane + 1) + " | Start " +
                    std::to_string(block.startCell + 1) + " | Length " + std::to_string(block.lengthCells) + " cells",
                block.selected);
        }

        if (!addedAudio)
        {
            addEntry("Audio", "Lead Vox Chop", "Preview sample ready for playlist", true);
            addEntry("Audio", "Impact Downlifter", "Transition FX clip");
            addEntry("Audio", "Analog Bass One-Shot", "Sampler-ready source clip");
        }
        break;
    }
    default:
        for (const auto& automation : workspaceModel_.automationLanes)
        {
            addEntry(
                "Automation",
                automation.target,
                "Lane " + std::to_string(automation.lane + 1) + " | Start " +
                    std::to_string(automation.startCell + 1) + " | " +
                    std::to_string(automation.values.size()) + " points",
                automation.selected);
        }

        if (workspaceModel_.automationLanes.empty())
        {
            addEntry("Automation", "Master Volume", "Fallback envelope lane", true);
            addEntry("Automation", "Filter Cutoff", "Sweep automation clip");
        }
        break;
    }
}

void UI::ensurePatternBank()
{
    const std::size_t expectedLaneCount = std::max<std::size_t>(1, visibleState_.project.tracks.empty() ? 1 : visibleState_.project.tracks.size());
    bool rebuildPatterns = workspaceModel_.patterns.empty();

    if (!rebuildPatterns)
    {
        for (const auto& pattern : workspaceModel_.patterns)
        {
            if (pattern.lanes.size() != expectedLaneCount)
            {
                rebuildPatterns = true;
                break;
            }
        }
    }

    if (!rebuildPatterns)
    {
        return;
    }

    workspaceModel_.patterns.clear();
    for (int patternNumber = 1; patternNumber <= 4; ++patternNumber)
    {
        workspaceModel_.patterns.push_back(makePatternState(patternNumber));
    }
}

void UI::rebuildPatternLanes()
{
    workspaceModel_.patternLanes.clear();

    if (workspaceModel_.patterns.empty())
    {
        workspaceModel_.patterns.push_back(makePatternState(1));
    }

    workspaceModel_.selectedPatternIndex =
        clampValue(workspace_.activePattern - 1, 0, static_cast<int>(workspaceModel_.patterns.size() - 1));
    workspaceModel_.patternLanes =
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].lanes;
}

void UI::rebuildPlaylistBlocks()
{
    workspaceModel_.playlistBlocks.clear();
    workspaceModel_.markers.clear();

    int lane = 0;
    for (const auto& track : visibleState_.project.tracks)
    {
        for (const auto& clip : track.clips)
        {
            PlaylistBlockState block{};
            block.clipId = clip.clipId;
            block.trackId = track.trackId;
            block.lane = lane;
            block.startCell = clampValue(static_cast<int>(clip.startTimeSeconds * 2.0), 0, kPlaylistCellCount - 2);
            block.lengthCells = std::max(2, static_cast<int>(clip.durationSeconds * 2.0 + 0.5));
            block.label = clip.name.empty() ? ("Clip " + std::to_string(clip.clipId)) : clip.name;
            block.clipType = clip.sourceLabel;
            block.patternNumber = workspace_.activePattern;
            block.muted = clip.muted;
            block.selected = (clip.clipId == interactionState_.selectedClipId);
            workspaceModel_.playlistBlocks.push_back(std::move(block));
        }
        ++lane;
    }

    if (workspaceModel_.playlistBlocks.empty())
    {
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{1, 0, 0, 0, 4, "Pattern 1 Intro", "Pattern", 1, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{2, 0, 1, 4, 6, "Pattern 2 Bass", "Pattern", 2, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{3, 0, 2, 10, 8, "Vocal Chop", "Audio", 0, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{4, 0, 0, 18, 6, "Pattern 3 Fill", "Pattern", 3, false, false});
    }

    workspaceModel_.markers = {
        {"Intro", 0},
        {"Drop", 8},
        {"Break", 16},
        {"Outro", 24}};
}

void UI::rebuildMixerStrips()
{
    workspaceModel_.mixerStrips.clear();

    int insertSlot = 1;
    for (const auto& bus : visibleState_.project.buses)
    {
        MixerStripState strip{};
        strip.busId = bus.busId;
        strip.name = bus.name;
        strip.insertSlot = insertSlot;
        strip.volumeDb = -4.5 + static_cast<double>(insertSlot % 5);
        strip.pan = (insertSlot % 3 == 0) ? -0.12 : ((insertSlot % 4 == 0) ? 0.15 : 0.0);
        strip.peakLevel = normalizedMeterFill(strip.volumeDb);
        strip.routeTarget = (insertSlot % 3 == 0) ? 0 : -1;
        strip.sendAmount = 0.12 * static_cast<double>(insertSlot % 4);
        strip.effects = {"EQ", "Comp", insertSlot % 2 == 0 ? "Saturator" : "Delay"};
        workspaceModel_.mixerStrips.push_back(std::move(strip));
        ++insertSlot;
    }

    if (workspaceModel_.mixerStrips.empty())
    {
        workspaceModel_.mixerStrips.push_back(MixerStripState{1, "Kick Bus", 1, -3.0, 0.0, 78, false, false, -1, 0.10, {"EQ", "Clipper"}});
        workspaceModel_.mixerStrips.push_back(MixerStripState{2, "Music Bus", 2, -4.5, -0.08, 66, false, false, 0, 0.26, {"EQ", "Compressor", "Stereo"}});
    }

    workspaceModel_.mixerStrips.push_back(MixerStripState{0, "Master", 0, -1.2, 0.0, 92, false, false, -1, 0.0, {"Glue", "Limiter"}});
}

void UI::rebuildChannelSettings()
{
    workspaceModel_.channelSettings.clear();

    std::size_t laneIndex = 0;
    for (const auto& lane : workspaceModel_.patternLanes)
    {
        ChannelSettingsState settings{};
        settings.trackId = lane.trackId;
        settings.name = lane.name;
        settings.gain = 0.72 + (static_cast<double>(laneIndex % 4) * 0.08);
        settings.pan = (laneIndex % 3 == 0) ? -0.12 : ((laneIndex % 4 == 0) ? 0.16 : 0.0);
        settings.pitchSemitones = static_cast<double>((static_cast<int>(laneIndex) % 5) - 2);
        settings.attackMs = 8.0 + static_cast<double>(laneIndex * 3);
        settings.releaseMs = 150.0 + static_cast<double>(laneIndex * 24);
        settings.filterCutoffHz = 6200.0 + static_cast<double>(laneIndex * 850);
        settings.resonance = 0.18 + static_cast<double>(laneIndex) * 0.03;
        settings.mixerInsert = static_cast<int>(laneIndex + 1);
        settings.routeTarget = (laneIndex % 2 == 0) ? 0 : -1;
        settings.reverse = (laneIndex % 5 == 0);
        settings.timeStretch = true;
        settings.pluginRack = {
            laneIndex % 2 == 0 ? "Sampler" : "Synth Rack",
            "Parametric EQ",
            laneIndex % 3 == 0 ? "Soft Clipper" : "Compressor"};
        settings.presets = {
            lane.name + " Init",
            lane.name + " Bright",
            lane.name + " Wide"};
        workspaceModel_.channelSettings.push_back(std::move(settings));
        ++laneIndex;
    }

    if (workspaceModel_.channelSettings.empty())
    {
        workspaceModel_.channelSettings.push_back(ChannelSettingsState{0, "Init Sampler", 0.8, 0.0, 0.0, 12.0, 180.0, 8400.0, 0.2, 1, 0, false, true, {"Sampler", "EQ"}, {"Init", "Bright", "Wide"}});
    }
}

void UI::rebuildAutomationLanes()
{
    workspaceModel_.automationLanes.clear();
    const int automationLaneBase = static_cast<int>(workspaceModel_.patternLanes.size());
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Master Volume", {85, 78, 80, 92, 88, 74, 96, 83}, 4001, automationLaneBase + 0, 0, 8, false});
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Lead Filter", {24, 28, 36, 48, 58, 72, 81, 94}, 4002, automationLaneBase + 1, 8, 8, false});
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Delay Send", {8, 12, 18, 12, 22, 14, 26, 18}, 4003, automationLaneBase + 2, 18, 6, false});
}

UI::PatternState UI::makePatternState(int patternNumber) const
{
    PatternState pattern{};
    pattern.patternNumber = patternNumber;
    pattern.name = "Pattern " + std::to_string(patternNumber);
    pattern.lengthInBars = 2 + ((patternNumber - 1) % 3);
    pattern.accentAmount = 8 * patternNumber;

    std::size_t laneIndex = 0;
    for (const auto& track : visibleState_.project.tracks)
    {
        PatternLaneState lane{};
        lane.trackId = track.trackId;
        lane.name = track.name;
        lane.steps.resize(kStepCount);
        for (int step = 0; step < kStepCount; ++step)
        {
            const int patternOffset = patternNumber - 1;
            const bool onQuarter = ((step + patternOffset) % 4 == 0);
            const bool grooveHit = ((step + static_cast<int>(laneIndex) + patternOffset) % (patternNumber + 3) == 0);
            lane.steps[static_cast<std::size_t>(step)].enabled = onQuarter || grooveHit;
            lane.steps[static_cast<std::size_t>(step)].velocity =
                68 + ((step * (patternNumber + 6) + static_cast<int>(laneIndex) * 11) % 56);
        }
        lane.swing = static_cast<int>((laneIndex * 9 + patternNumber * 5) % 48);
        lane.shuffle = static_cast<int>((laneIndex * 5 + patternNumber * 7) % 32);
        ensurePatternLaneNoteContent(lane, laneIndex + static_cast<std::size_t>(patternNumber - 1));
        pattern.lanes.push_back(std::move(lane));
        ++laneIndex;
    }

    if (pattern.lanes.empty())
    {
        PatternLaneState lane{};
        lane.name = "Init Sampler";
        lane.steps.resize(kStepCount);
        for (int step = 0; step < kStepCount; ++step)
        {
            lane.steps[static_cast<std::size_t>(step)].enabled = ((step + patternNumber - 1) % 4 == 0);
            lane.steps[static_cast<std::size_t>(step)].velocity = 88 + (patternNumber * 2);
        }
        ensurePatternLaneNoteContent(lane, static_cast<std::size_t>(patternNumber - 1));
        pattern.lanes.push_back(std::move(lane));
    }

    return pattern;
}

void UI::ensurePatternLaneNoteContent(PatternLaneState& lane, std::size_t laneIndex) const
{
    if (!lane.notes.empty())
    {
        return;
    }

    const int rootLane = 12 + static_cast<int>((laneIndex * 3) % 7);
    lane.notes.push_back(PianoNoteState{rootLane, 0, 3, 98, true, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 4, 4, 2, 84, false, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 7, 8, 4, 102, false, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 12, 14, 2, 92, false, (laneIndex % 2) == 0, false});
}

void UI::drawSurfaceHeader(HDC dc, const RECT& rect, const std::string& title, const std::string& subtitle) const
{
    RECT headerRect{rect.left, rect.top, rect.right, rect.top + kSurfaceHeaderHeight};
    HBRUSH headerBrush = CreateSolidBrush(RGB(43, 49, 58));
    FillRect(dc, &headerRect, headerBrush);
    DeleteObject(headerBrush);

    RECT titleRect{rect.left + 8, rect.top + 3, rect.right - 8, rect.top + kSurfaceHeaderHeight - 2};
    SetTextColor(dc, RGB(244, 244, 244));
    DrawTextA(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (!subtitle.empty())
    {
        RECT subtitleRect{rect.right - 280, rect.top + 3, rect.right - 8, rect.top + kSurfaceHeaderHeight - 2};
        SetTextColor(dc, RGB(173, 181, 193));
        DrawTextA(dc, subtitle.c_str(), -1, &subtitleRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void UI::drawSurfaceFrame(HDC dc, const RECT& rect, COLORREF borderColor) const
{
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

std::string UI::browserDropTargetLabel() const
{
    if (workspaceModel_.browserEntries.empty())
    {
        return "Rack / Playlist / Mixer";
    }

    const BrowserEntry& entry = workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)];
    return entry.label + " -> Rack / Playlist / Mixer";
}
