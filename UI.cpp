#include "UI.h"

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

namespace
{
    constexpr int kOuterPadding = 12;
    constexpr int kGap = 8;
    constexpr int kToolbarHeight = 78;
    constexpr int kTransportHeight = 30;
    constexpr int kInfoStripHeight = 64;
    constexpr int kBrowserWidth = 290;
    constexpr int kPluginHeight = 150;
    constexpr int kMixerHeight = 190;
    constexpr int kSectionHeaderHeight = 24;

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
}

UI::UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine)
    : hInstance_(hInstance),
      nCmdShow_(nCmdShow),
      engine_(engine)
{
    dockedPanes_ = {
        {WorkspacePane::Browser, {}, false, true, "Browser"},
        {WorkspacePane::ChannelRack, {}, false, true, "Channel Rack"},
        {WorkspacePane::PianoRoll, {}, false, true, "Piano Roll"},
        {WorkspacePane::Playlist, {}, false, true, "Playlist"},
        {WorkspacePane::Mixer, {}, false, true, "Mixer"},
        {WorkspacePane::Plugin, {}, false, true, "Plugin"}};
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

void UI::registerWindowClass()
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = UI::WindowProc;
    wc.hInstance = hInstance_;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

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
    managerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

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
    surfaceClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExA(&surfaceClass))
    {
        throw UiInitializationException("No se pudo registrar la clase de superficies custom.");
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
    const DWORD panelStyle = WS_VISIBLE | WS_CHILD | WS_BORDER | SS_LEFT;

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
    const int contentWidth = std::max(400, width - (kOuterPadding * 2));

    int y = kOuterPadding;
    const int toolbarY = y;

    MoveWindow(engineStartButton_, kOuterPadding, toolbarY, 70, kTransportHeight, TRUE);
    MoveWindow(engineStopButton_, kOuterPadding + 74, toolbarY, 78, kTransportHeight, TRUE);
    MoveWindow(playButton_, kOuterPadding + 160, toolbarY, 56, kTransportHeight, TRUE);
    MoveWindow(stopTransportButton_, kOuterPadding + 220, toolbarY, 56, kTransportHeight, TRUE);
    MoveWindow(recordButton_, kOuterPadding + 280, toolbarY, 56, kTransportHeight, TRUE);
    MoveWindow(patSongButton_, kOuterPadding + 344, toolbarY, 62, kTransportHeight, TRUE);

    MoveWindow(tempoDownButton_, kOuterPadding + 420, toolbarY, 24, kTransportHeight, TRUE);
    MoveWindow(tempoLabel_, kOuterPadding + 448, toolbarY + 6, 78, 20, TRUE);
    MoveWindow(tempoUpButton_, kOuterPadding + 530, toolbarY, 24, kTransportHeight, TRUE);

    MoveWindow(patternPrevButton_, kOuterPadding + 568, toolbarY, 24, kTransportHeight, TRUE);
    MoveWindow(patternLabel_, kOuterPadding + 596, toolbarY + 6, 92, 20, TRUE);
    MoveWindow(patternNextButton_, kOuterPadding + 692, toolbarY, 24, kTransportHeight, TRUE);

    MoveWindow(snapPrevButton_, kOuterPadding + 730, toolbarY, 24, kTransportHeight, TRUE);
    MoveWindow(snapLabel_, kOuterPadding + 758, toolbarY + 6, 112, 20, TRUE);
    MoveWindow(snapNextButton_, kOuterPadding + 874, toolbarY, 24, kTransportHeight, TRUE);

    MoveWindow(browserButton_, kOuterPadding + 912, toolbarY, 66, kTransportHeight, TRUE);
    MoveWindow(channelRackButton_, kOuterPadding + 982, toolbarY, 60, kTransportHeight, TRUE);
    MoveWindow(pianoRollButton_, kOuterPadding + 1046, toolbarY, 60, kTransportHeight, TRUE);
    MoveWindow(playlistButton_, kOuterPadding + 1110, toolbarY, 66, kTransportHeight, TRUE);
    MoveWindow(mixerButton_, kOuterPadding + 1180, toolbarY, 60, kTransportHeight, TRUE);
    MoveWindow(pluginButton_, kOuterPadding + 1244, toolbarY, 60, kTransportHeight, TRUE);

    MoveWindow(addTrackButton_, kOuterPadding, toolbarY + 38, 86, 28, TRUE);
    MoveWindow(addBusButton_, kOuterPadding + 90, toolbarY + 38, 72, 28, TRUE);
    MoveWindow(addClipButton_, kOuterPadding + 166, toolbarY + 38, 72, 28, TRUE);
    MoveWindow(undoButton_, kOuterPadding + 242, toolbarY + 38, 60, 28, TRUE);
    MoveWindow(redoButton_, kOuterPadding + 306, toolbarY + 38, 60, 28, TRUE);
    MoveWindow(saveProjectButton_, kOuterPadding + 370, toolbarY + 38, 60, 28, TRUE);
    MoveWindow(loadProjectButton_, kOuterPadding + 434, toolbarY + 38, 60, 28, TRUE);
    MoveWindow(prevTrackButton_, kOuterPadding + 498, toolbarY + 38, 70, 28, TRUE);
    MoveWindow(nextTrackButton_, kOuterPadding + 572, toolbarY + 38, 70, 28, TRUE);
    MoveWindow(rebuildGraphButton_, kOuterPadding + 646, toolbarY + 38, 68, 28, TRUE);
    MoveWindow(renderOfflineButton_, kOuterPadding + 718, toolbarY + 38, 66, 28, TRUE);
    MoveWindow(managePluginsButton_, kOuterPadding + 788, toolbarY + 38, 76, 28, TRUE);
    MoveWindow(automationCheckbox_, kOuterPadding + 874, toolbarY + 42, 92, 22, TRUE);
    MoveWindow(pdcCheckbox_, kOuterPadding + 972, toolbarY + 42, 50, 22, TRUE);
    MoveWindow(anticipativeCheckbox_, kOuterPadding + 1028, toolbarY + 42, 96, 22, TRUE);
    MoveWindow(systemLabel_, kOuterPadding + 1130, toolbarY + 41, std::max(110, contentWidth - 1140), 24, TRUE);

    y += kToolbarHeight + kGap;

    MoveWindow(statusLabel_, kOuterPadding, y, contentWidth, 22, TRUE);
    MoveWindow(projectSummaryLabel_, kOuterPadding, y + 24, contentWidth, 18, TRUE);
    MoveWindow(documentLabel_, kOuterPadding, y + 42, contentWidth / 2, 18, TRUE);
    MoveWindow(selectionLabel_, kOuterPadding + (contentWidth / 2), y + 42, contentWidth / 2, 18, TRUE);
    y += kInfoStripHeight + kGap;

    const int browserAreaWidth = workspace_.browserVisible ? kBrowserWidth : 0;
    const int workspaceX = kOuterPadding + browserAreaWidth + (workspace_.browserVisible ? kGap : 0);
    const int workspaceWidth = width - workspaceX - kOuterPadding;
    const int workspaceHeight = height - y - kOuterPadding;
    updateDockingModel();
    ensureDetachedWindows();

    ShowWindow(browserMenuButton_, workspace_.browserVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(browserPanel_, workspace_.browserVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.browserVisible)
    {
        MoveWindow(browserMenuButton_, kOuterPadding + browserAreaWidth - 28, y, 28, kSectionHeaderHeight, TRUE);
        MoveWindow(browserTabPrevButton_, kOuterPadding + browserAreaWidth - 84, y, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(browserTabNextButton_, kOuterPadding + browserAreaWidth - 56, y, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(browserHeaderLabel_, kOuterPadding + 8, y + 4, browserAreaWidth - 96, 18, TRUE);
        MoveWindow(browserPanel_, kOuterPadding, y + kSectionHeaderHeight, browserAreaWidth, workspaceHeight - kSectionHeaderHeight - 24, TRUE);
        MoveWindow(contextLabel_, kOuterPadding, y + workspaceHeight - 20, browserAreaWidth, 18, TRUE);
        ShowWindow(contextLabel_, SW_SHOW);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::Browser))
        {
            pane->bounds = RECT{kOuterPadding, y, kOuterPadding + browserAreaWidth, y + workspaceHeight};
        }
    }
    else
    {
        ShowWindow(browserTabPrevButton_, SW_HIDE);
        ShowWindow(browserTabNextButton_, SW_HIDE);
        ShowWindow(browserHeaderLabel_, SW_HIDE);
        ShowWindow(contextLabel_, SW_HIDE);
    }

    if (workspaceWidth <= 120 || workspaceHeight <= 120)
    {
        return;
    }

    const int topRegionHeight = workspaceHeight - kMixerHeight - (workspace_.pluginVisible ? (kPluginHeight + kGap) : 0) - kGap;
    const int leftWidth = std::max(250, (workspaceWidth * 33) / 100);
    const int rightWidth = workspaceWidth - leftWidth - kGap;
    const int channelRackHeight = std::max(160, (topRegionHeight * 43) / 100);
    const int pianoRollHeight = std::max(150, topRegionHeight - channelRackHeight - kGap);
    const int playlistHeight = topRegionHeight;
    const int mixerY = y + topRegionHeight + kGap;
    const int pluginY = mixerY + kMixerHeight + kGap;

    const auto placePanel =
        [&](bool visible, HWND menuButton, HWND panel, int x, int panelY, int panelWidth, int panelHeight)
    {
        ShowWindow(menuButton, visible ? SW_SHOW : SW_HIDE);
        ShowWindow(panel, visible ? SW_SHOW : SW_HIDE);
        if (!visible)
        {
            return;
        }

        MoveWindow(menuButton, x + panelWidth - 28, panelY, 28, kSectionHeaderHeight, TRUE);
        MoveWindow(panel, x, panelY + kSectionHeaderHeight, panelWidth, std::max(60, panelHeight - kSectionHeaderHeight), TRUE);
    };

    placePanel(workspace_.channelRackVisible, channelRackMenuButton_, channelRackPanel_, workspaceX, y, leftWidth, channelRackHeight / 2);
    ShowWindow(channelHeaderLabel_, workspace_.channelRackVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.channelRackVisible)
    {
        MoveWindow(channelHeaderLabel_, workspaceX + 8, y + 4, leftWidth - 40, 18, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::ChannelRack))
        {
            pane->bounds = RECT{workspaceX, y, workspaceX + leftWidth, y + channelRackHeight};
        }
    }
    ShowWindow(stepSequencerPanel_, workspace_.channelRackVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.channelRackVisible)
    {
        MoveWindow(stepSequencerPanel_, workspaceX, y + (channelRackHeight / 2) + kGap, leftWidth, std::max(72, (channelRackHeight / 2) - kGap), TRUE);
    }

    placePanel(workspace_.pianoRollVisible, pianoRollMenuButton_, pianoRollPanel_, workspaceX, y + channelRackHeight + kGap, leftWidth, pianoRollHeight);
    ShowWindow(pianoZoomPrevButton_, workspace_.pianoRollVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(pianoZoomNextButton_, workspace_.pianoRollVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(pianoToolPrevButton_, workspace_.pianoRollVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(pianoToolNextButton_, workspace_.pianoRollVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(pianoHeaderLabel_, workspace_.pianoRollVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.pianoRollVisible)
    {
        const int pianoHeaderY = y + channelRackHeight + kGap;
        MoveWindow(pianoHeaderLabel_, workspaceX + 8, pianoHeaderY + 4, leftWidth - 136, 18, TRUE);
        MoveWindow(pianoZoomPrevButton_, workspaceX + leftWidth - 132, pianoHeaderY, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(pianoZoomNextButton_, workspaceX + leftWidth - 104, pianoHeaderY, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(pianoToolPrevButton_, workspaceX + leftWidth - 76, pianoHeaderY, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(pianoToolNextButton_, workspaceX + leftWidth - 48, pianoHeaderY, 24, kSectionHeaderHeight, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::PianoRoll))
        {
            pane->bounds = RECT{workspaceX, pianoHeaderY, workspaceX + leftWidth, pianoHeaderY + pianoRollHeight};
        }
    }
    placePanel(workspace_.playlistVisible, playlistMenuButton_, playlistPanel_, workspaceX + leftWidth + kGap, y, rightWidth, playlistHeight);
    ShowWindow(playlistZoomPrevButton_, workspace_.playlistVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(playlistZoomNextButton_, workspace_.playlistVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(playlistToolPrevButton_, workspace_.playlistVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(playlistToolNextButton_, workspace_.playlistVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(playlistHeaderLabel_, workspace_.playlistVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.playlistVisible)
    {
        MoveWindow(playlistHeaderLabel_, workspaceX + leftWidth + kGap + 8, y + 4, rightWidth - 136, 18, TRUE);
        MoveWindow(playlistZoomPrevButton_, workspaceX + workspaceWidth - 132, y, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(playlistZoomNextButton_, workspaceX + workspaceWidth - 104, y, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(playlistToolPrevButton_, workspaceX + workspaceWidth - 76, y, 24, kSectionHeaderHeight, TRUE);
        MoveWindow(playlistToolNextButton_, workspaceX + workspaceWidth - 48, y, 24, kSectionHeaderHeight, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::Playlist))
        {
            pane->bounds = RECT{workspaceX + leftWidth + kGap, y, workspaceX + workspaceWidth, y + playlistHeight};
        }
    }
    placePanel(workspace_.mixerVisible, mixerMenuButton_, mixerPanel_, workspaceX, mixerY, workspaceWidth, kMixerHeight);
    ShowWindow(mixerHeaderLabel_, workspace_.mixerVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.mixerVisible)
    {
        MoveWindow(mixerHeaderLabel_, workspaceX + 8, mixerY + 4, workspaceWidth - 40, 18, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::Mixer))
        {
            pane->bounds = RECT{workspaceX, mixerY, workspaceX + workspaceWidth, mixerY + kMixerHeight};
        }
    }
    placePanel(workspace_.pluginVisible, pluginMenuButton_, pluginPanel_, workspaceX, pluginY, workspaceWidth, kPluginHeight);
    ShowWindow(pluginHeaderLabel_, workspace_.pluginVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.pluginVisible)
    {
        MoveWindow(pluginHeaderLabel_, workspaceX + 8, pluginY + 4, workspaceWidth - 40, 18, TRUE);
        if (DockedPaneState* pane = findDockedPane(WorkspacePane::Plugin))
        {
            pane->bounds = RECT{workspaceX, pluginY, workspaceX + workspaceWidth, pluginY + kPluginHeight};
        }
    }

    MoveWindow(hintsLabel_, workspaceX, height - kOuterPadding - 18, workspaceWidth, 18, TRUE);
}

void UI::ensureDetachedWindows()
{
    for (const auto& pane : dockedPanes_)
    {
        (void)pane;
    }
}

void UI::updateDockingModel()
{
    for (auto& pane : dockedPanes_)
    {
        pane.visible = isPaneVisible(pane.pane);
        pane.title = paneTitle(pane.pane);
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
        "Shortcuts: F5 Playlist | F6 Channel Rack | F7 Piano Roll | F9 Mixer | Alt+F8 Browser | Space Play | +/- Zoom in panes");

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
    SetWindowTextA(playlistButton_, workspace_.playlistVisible ? "Playlist*" : "Playlist");
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
    workspace_.activePattern = std::min(999, workspace_.activePattern + 1);
}

void UI::selectPreviousPattern()
{
    workspace_.activePattern = std::max(1, workspace_.activePattern - 1);
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
        workspace_.browserTabIndex = 4;
    }
    else
    {
        workspace_.browserTabIndex = static_cast<std::size_t>(newIndex) % 5;
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
    const auto visibleWorkspacePanelCount = [this]() -> int
    {
        return (workspace_.channelRackVisible ? 1 : 0) +
               (workspace_.pianoRollVisible ? 1 : 0) +
               (workspace_.playlistVisible ? 1 : 0) +
               (workspace_.mixerVisible ? 1 : 0) +
               (workspace_.pluginVisible ? 1 : 0);
    };

    switch (pane)
    {
    case WorkspacePane::Browser:
        workspace_.browserVisible = !workspace_.browserVisible;
        workspace_.focusedPane = WorkspacePane::Browser;
        break;

    case WorkspacePane::ChannelRack:
        if (workspace_.channelRackVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.channelRackVisible = !workspace_.channelRackVisible;
        workspace_.focusedPane = WorkspacePane::ChannelRack;
        break;

    case WorkspacePane::PianoRoll:
        if (workspace_.pianoRollVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.pianoRollVisible = !workspace_.pianoRollVisible;
        workspace_.focusedPane = WorkspacePane::PianoRoll;
        break;

    case WorkspacePane::Playlist:
        if (workspace_.playlistVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.playlistVisible = !workspace_.playlistVisible;
        workspace_.focusedPane = WorkspacePane::Playlist;
        break;

    case WorkspacePane::Mixer:
        if (workspace_.mixerVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.mixerVisible = !workspace_.mixerVisible;
        workspace_.focusedPane = WorkspacePane::Mixer;
        break;

    case WorkspacePane::Plugin:
        if (workspace_.pluginVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.pluginVisible = !workspace_.pluginVisible;
        workspace_.focusedPane = WorkspacePane::Plugin;
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
    static constexpr const char* kBrowserTabs[] = {"Packs", "Plugin DB", "Current Project", "Automation", "Disk"};
    return kBrowserTabs[std::min<std::size_t>(workspace_.browserTabIndex, 4)];
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

    if (workspace_.browserTabIndex == 0)
    {
        browser
            << "Packs / Samples\n"
            << "  - Drums > Kicks\n"
            << "  - Drums > Snares\n"
            << "  - Drums > Hats\n"
            << "  - Loops > Percussion\n"
            << "  - FX > Risers\n"
            << "  - Instruments > Multi-samples\n";
    }
    else if (workspace_.browserTabIndex == 1)
    {
        browser
            << "Plugin Database\n"
            << "  - Generators > Sampler\n"
            << "  - Generators > Synth Lead\n"
            << "  - Generators > Bass Module\n"
            << "  - Effects > EQ\n"
            << "  - Effects > Compressor\n"
            << "  - Effects > Reverb\n";
    }
    else if (workspace_.browserTabIndex == 2)
    {
        browser
            << "Current Project\n"
            << "  - Patterns " << std::max<std::size_t>(1, visibleState_.project.tracks.size()) << "\n"
            << "  - Mixer states " << visibleState_.project.buses.size() << "\n"
            << "  - Automation clips " << visibleState_.project.tracks.size() << "\n"
            << "  - Project bones\n";
    }
    else if (workspace_.browserTabIndex == 3)
    {
        browser
            << "Automation / Scores\n"
            << "  - Volume envelopes\n"
            << "  - Filter cutoff\n"
            << "  - Pan lanes\n"
            << "  - MIDI scores\n"
            << "  - Articulation maps\n";
    }
    else
    {
        browser
            << "Linked Folders / Disk\n"
            << "  - " << (visibleState_.document.sessionPath.empty() ? "<unsaved project>" : visibleState_.document.sessionPath) << "\n"
            << "  - Audio renders\n"
            << "  - User presets\n"
            << "  - Imported loops\n";
    }

    browser
        << "\nDrag-drop target: Channel Rack, Playlist or Mixer"
        << "\nAlt+F8 toggles this panel.";
    return browser.str();
}

std::string UI::buildChannelRackPanelText() const
{
    std::ostringstream rack;
    rack << "Channel Rack\n\n";
    if (visibleState_.project.tracks.empty())
    {
        rack << "No channels yet.\nUse Add Track to create instruments, samplers or automation channels.";
        return rack.str();
    }

    for (std::size_t index = 0; index < visibleState_.project.tracks.size(); ++index)
    {
        const VisibleTrack& track = visibleState_.project.tracks[index];
        rack
            << (index == selectedTrackIndex_ ? "> " : "  ")
            << track.name
            << " | M " << boolLabel(track.muted, "On", "Off")
            << " | P C"
            << " | V " << (track.muted ? "0%" : "82%")
            << " | FX " << track.busId
            << " | Steps " << std::max<std::size_t>(4, track.clips.size() * 4)
            << "\n";
    }

    rack << "\nEvery row acts like a channel: mute, pan, volume, mixer destination and step source.";
    return rack.str();
}

std::string UI::buildStepSequencerPanelText() const
{
    const std::string selectedName =
        visibleState_.selection.selectedTrackName.empty() ? std::string("Selected channel") : visibleState_.selection.selectedTrackName;

    std::ostringstream steps;
    steps
        << "Step Sequencer\n\n"
        << selectedName << "\n"
        << "Bars 1-2 | Snap " << currentSnapLabel() << "\n\n"
        << "Kick  : x . . x  . . x .  x . . x  . . x .\n"
        << "Snare : . . x .  . . x .  . . x .  . . x .\n"
        << "Hat   : x x x x  x x x x  x x x x  x x x x\n"
        << "Clap  : . . . .  x . . .  . . . .  x . . .\n"
        << "\nUse draw/paint for fast drum programming before opening Piano Roll.";
    return steps.str();
}

std::string UI::buildPianoRollPanelText() const
{
    std::ostringstream piano;
    piano
        << "Piano Roll\n\n"
        << "Target Channel: "
        << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName)
        << "\nPattern: " << workspace_.activePattern
        << " | Snap: " << currentSnapLabel()
        << " | Zoom: " << currentZoomLabel(true)
        << " | Tool: " << currentToolLabel(true)
        << "\nGhost notes: visible | Helpers: on | Velocity lane: visible\n\n"
        << "C6 |----------------[]--------------|\n"
        << "A5 |----------[]----------[]--------|\n"
        << "G5 |------[]------------------------|\n"
        << "E5 |--[]----------[]----------------|\n"
        << "C5 |[]----------------------[]------|\n"
        << "    1.1   1.2   1.3   1.4   2.1   2.2\n\n"
        << "Velocity  | 84  72  91  68  95  76"
        << "\nSlide/porta/color lanes are represented conceptually here.";
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
        << "\nTimeline: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8\n\n";

    if (visibleState_.project.tracks.empty())
    {
        playlist << "Track 1  [empty lane]\n";
    }
    else
    {
        for (std::size_t lane = 0; lane < visibleState_.project.tracks.size(); ++lane)
        {
            const VisibleTrack& track = visibleState_.project.tracks[lane];
            playlist << "Lane " << (lane + 1) << " " << track.name << " : ";
            if (track.clips.empty())
            {
                playlist << "[empty]";
            }
            else
            {
                for (const auto& clip : track.clips)
                {
                    playlist << "[" << clip.name << " @" << clip.startTimeSeconds << "s len " << clip.durationSeconds << "s] ";
                }
            }
            playlist << "\n";
        }
    }

    playlist
        << "\nPattern Clips, Audio Clips and Automation Clips live in the same timeline."
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

    if (visibleState_.project.buses.empty())
    {
        mixer << "Insert 1 | Fader -6.0 dB | Pan C | FX 1 Empty | Route -> Master\n";
        mixer << "Insert 2 | Fader -3.0 dB | Pan C | FX 1 Empty | Route -> Master\n";
    }
    else
    {
        for (const auto& bus : visibleState_.project.buses)
        {
            mixer
                << bus.name
                << " | Inputs " << bus.inputTrackIds.size()
                << " | Fader -3.0 dB | Pan C | FX 1 EQ | FX 2 Comp | Route -> Master\n";
        }
    }

    mixer << "\nRouting, sends, insert FX and metering are represented in this strip view.";
    return mixer.str();
}

std::string UI::buildPluginPanelText() const
{
    std::ostringstream plugin;
    plugin
        << "Plugin / Channel Settings\n\n"
        << "Target: " << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName) << "\n"
        << "Wrapper: " << boolLabel(visibleState_.pluginHostEnabled, "Plugin host enabled", "In-process") << "\n"
        << "Sandbox: " << boolLabel(visibleState_.pluginSandboxEnabled, "On", "Off") << "\n"
        << "64-bit path: " << boolLabel(visibleState_.prefer64BitMix, "On", "Off") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "Sample-accurate", "Off") << "\n"
        << "PDC: " << boolLabel(visibleState_.pdcEnabled, "On", "Off") << "\n\n"
        << "Sampler / Synth Controls\n"
        << "  - Gain 0.80\n"
        << "  - Pan 0.00\n"
        << "  - Pitch 0 st\n"
        << "  - Attack 12 ms\n"
        << "  - Release 180 ms\n"
        << "  - Filter cutoff 8.4 kHz\n"
        << "  - Resonance 0.20\n\n"
        << "This pane represents Channel Settings + Wrapper + automatable parameters.";
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
    case WorkspacePane::ChannelRack: return "Channel Rack";
    case WorkspacePane::PianoRoll: return "Piano Roll";
    case WorkspacePane::Playlist: return "Playlist";
    case WorkspacePane::Mixer: return "Mixer";
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
    SetTextColor(dc, RGB(228, 228, 228));
    DrawTextA(dc, buildBrowserPanelText().c_str(), -1, const_cast<RECT*>(&rect), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void UI::paintChannelRackSurface(HDC dc, const RECT& rect)
{
    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextA(dc, buildChannelRackPanelText().c_str(), -1, const_cast<RECT*>(&rect), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void UI::paintStepSequencerSurface(HDC dc, const RECT& rect)
{
    SetTextColor(dc, RGB(224, 224, 224));
    DrawTextA(dc, buildStepSequencerPanelText().c_str(), -1, const_cast<RECT*>(&rect), DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void UI::paintPianoRollSurface(HDC dc, const RECT& rect)
{
    RECT drawRect = rect;
    const int keyWidth = 48;
    const int laneHeight = 22;
    const int columns = 24;
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
        const int x = gridLeft + (col * std::max(18, (drawRect.right - gridLeft) / columns));
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

    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextA(dc, buildPianoRollPanelText().c_str(), -1, &drawRect, DT_LEFT | DT_TOP);
}

void UI::paintPlaylistSurface(HDC dc, const RECT& rect)
{
    const int laneHeight = 34;
    const int timelineHeight = 24;
    const int leftInset = 90;
    const int columns = 16;
    const int columnWidth = std::max(28, (rect.right - leftInset) / columns);

    HPEN majorPen = CreatePen(PS_SOLID, 1, RGB(74, 84, 98));
    HPEN minorPen = CreatePen(PS_SOLID, 1, RGB(48, 54, 64));

    for (int lane = 0; lane < (rect.bottom - timelineHeight) / laneHeight; ++lane)
    {
        const int y = rect.top + timelineHeight + (lane * laneHeight);
        SelectObject(dc, minorPen);
        MoveToEx(dc, rect.left, y, nullptr);
        LineTo(dc, rect.right, y);
    }

    for (int col = 0; col <= columns; ++col)
    {
        const int x = rect.left + leftInset + (col * columnWidth);
        SelectObject(dc, (col % 4 == 0) ? majorPen : minorPen);
        MoveToEx(dc, x, rect.top, nullptr);
        LineTo(dc, x, rect.bottom);
    }

    DeleteObject(majorPen);
    DeleteObject(minorPen);

    rebuildPlaylistVisuals(rect);

    HBRUSH clipBrush = CreateSolidBrush(RGB(94, 170, 255));
    HBRUSH selectedBrush = CreateSolidBrush(RGB(145, 214, 255));
    for (const auto& clip : interactionState_.playlistClipVisuals)
    {
        RECT clipRect{clip.rect.x, clip.rect.y, clip.rect.x + clip.rect.width, clip.rect.y + clip.rect.height};
        FillRect(dc, &clipRect, clip.selected ? selectedBrush : clipBrush);
        SetTextColor(dc, RGB(20, 20, 26));
        DrawTextA(dc, ("Clip " + std::to_string(clip.clipId)).c_str(), -1, &clipRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    DeleteObject(clipBrush);
    DeleteObject(selectedBrush);

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

    RECT textRect = rect;
    textRect.left += 6;
    textRect.top += 4;
    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextA(dc, buildPlaylistPanelText().c_str(), -1, &textRect, DT_LEFT | DT_TOP);
}

void UI::paintMixerSurface(HDC dc, const RECT& rect)
{
    const int stripWidth = 78;
    const int stripGap = 12;
    const int meterTop = rect.top + 34;
    const int meterBottom = rect.bottom - 18;
    int x = rect.left + 12;

    const int stripCount = static_cast<int>(std::max<std::size_t>(6, visibleState_.project.buses.size() + 2));
    for (int strip = 0; strip < stripCount; ++strip)
    {
        RECT stripRect{x, rect.top + 8, x + stripWidth, rect.bottom - 8};
        HBRUSH stripBrush = CreateSolidBrush(RGB(44, 48, 56));
        FillRect(dc, &stripRect, stripBrush);
        DeleteObject(stripBrush);

        RECT meterRect{x + 28, meterTop, x + 50, meterBottom};
        HBRUSH meterBg = CreateSolidBrush(RGB(25, 26, 29));
        FillRect(dc, &meterRect, meterBg);
        DeleteObject(meterBg);

        const int fillHeight = ((meterBottom - meterTop) * ((strip % 5) + 3)) / 8;
        RECT fillRect{meterRect.left, meterBottom - fillHeight, meterRect.right, meterBottom};
        HBRUSH fillBrush = CreateSolidBrush(strip == stripCount - 1 ? RGB(239, 178, 52) : RGB(105, 230, 120));
        FillRect(dc, &fillRect, fillBrush);
        DeleteObject(fillBrush);

        RECT titleRect{x + 6, rect.bottom - 28, x + stripWidth - 6, rect.bottom - 8};
        SetTextColor(dc, RGB(232, 232, 232));
        DrawTextA(dc, (strip == stripCount - 1 ? "Master" : ("Ins " + std::to_string(strip + 1))).c_str(), -1, &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += stripWidth + stripGap;
    }
}

void UI::paintPluginSurface(HDC dc, const RECT& rect)
{
    RECT drawRect = rect;
    HBRUSH moduleBrush = CreateSolidBrush(RGB(48, 54, 64));
    RECT upper{rect.left + 12, rect.top + 12, rect.right - 12, rect.top + 72};
    FillRect(dc, &upper, moduleBrush);
    RECT lower{rect.left + 12, rect.top + 86, rect.right - 12, rect.bottom - 12};
    FillRect(dc, &lower, moduleBrush);
    DeleteObject(moduleBrush);

    SetTextColor(dc, RGB(236, 236, 236));
    DrawTextA(dc, buildPluginPanelText().c_str(), -1, &drawRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void UI::handleSurfaceMouseDown(HWND hwnd, SurfaceKind kind, int x, int y)
{
    workspace_.focusedPane =
        kind == SurfaceKind::Browser ? WorkspacePane::Browser :
        kind == SurfaceKind::ChannelRack ? WorkspacePane::ChannelRack :
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
        interactionState_.draggingClip = false;
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            const bool hit =
                x >= clip.rect.x && x <= (clip.rect.x + clip.rect.width) &&
                y >= clip.rect.y && y <= (clip.rect.y + clip.rect.height);
            clip.selected = hit;
            if (hit)
            {
                interactionState_.selectedClipId = clip.clipId;
                interactionState_.draggingClip = true;
            }
        }
        if (!interactionState_.draggingClip)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::PianoRoll)
    {
        interactionState_.draggingNote = false;
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
                interactionState_.draggingNote = true;
                interactionState_.selectedNoteIndex = index;
            }
        }
        if (!interactionState_.draggingNote)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::Browser)
    {
        interactionState_.browserDragActive = true;
        interactionState_.selectedBrowserItemIndex = static_cast<std::size_t>(std::max(0, (y - 28) / 20));
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
    else if (kind == SurfaceKind::PianoRoll && interactionState_.draggingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size())
    {
        auto& note = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        note.rect.x += (x - interactionState_.dragStart.x);
        note.rect.y += (y - interactionState_.dragStart.y);
        interactionState_.dragStart = POINT{x, y};
    }
    else if (interactionState_.marqueeActive)
    {
        interactionState_.marqueeRect.left = std::min(interactionState_.dragStart.x, x);
        interactionState_.marqueeRect.top = std::min(interactionState_.dragStart.y, y);
        interactionState_.marqueeRect.right = std::max(interactionState_.dragStart.x, x);
        interactionState_.marqueeRect.bottom = std::max(interactionState_.dragStart.y, y);
    }

    invalidateSurface(hwnd);
}

void UI::handleSurfaceMouseUp(HWND hwnd, SurfaceKind kind, int x, int y)
{
    (void)x;
    (void)y;

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
        }
    }

    if (kind == SurfaceKind::Playlist && interactionState_.selectedClipId != 0)
    {
        const auto clipIt = std::find_if(
            interactionState_.playlistClipVisuals.begin(),
            interactionState_.playlistClipVisuals.end(),
            [&](const UiClipVisual& clip) { return clip.clipId == interactionState_.selectedClipId; });

        if (clipIt != interactionState_.playlistClipVisuals.end() && !visibleState_.project.tracks.empty())
        {
            const int laneHeight = 34;
            const int timelineHeight = 24;
            const int leftInset = 90;
            const int targetLane = clampValue((clipIt->rect.y - timelineHeight) / laneHeight, 0, static_cast<int>(visibleState_.project.tracks.size() - 1));
            const double startTime = std::max(0.0, static_cast<double>(clipIt->rect.x - leftInset) / 14.0);

            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::MoveClip;
            command.uintValue = clipIt->clipId;
            command.secondaryUintValue = visibleState_.project.tracks[static_cast<std::size_t>(targetLane)].trackId;
            command.doubleValue = startTime;
            engine_.postCommand(command);
        }
    }

    if (interactionState_.browserDragActive)
    {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        HWND targetWindow = WindowFromPoint(screenPoint);
        const SurfaceKind targetKind = kindFromSurfaceHandle(targetWindow);

        const std::string droppedName =
            currentBrowserTabLabel() + " Item " + std::to_string(interactionState_.selectedBrowserItemIndex + 1);

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
    interactionState_.browserDragActive = false;
    interactionState_.marqueeActive = false;
    ReleaseCapture();
    invalidateSurface(hwnd);
}

void UI::rebuildPlaylistVisuals(const RECT& rect)
{
    interactionState_.playlistClipVisuals.clear();
    const int timelineHeight = 24;
    const int laneHeight = 34;
    const int leftInset = 90;
    const int columnWidth = std::max(28, (rect.right - leftInset) / 16);

    int lane = 0;
    for (const auto& track : visibleState_.project.tracks)
    {
        for (const auto& clip : track.clips)
        {
            UiClipVisual visual{};
            visual.clipId = clip.clipId;
            visual.trackId = track.trackId;
            visual.rect.x = leftInset + static_cast<int>(clip.startTimeSeconds * 14.0);
            visual.rect.y = timelineHeight + 6 + (lane * laneHeight);
            visual.rect.width = std::max(42, static_cast<int>(clip.durationSeconds * 16.0));
            visual.rect.height = laneHeight - 10;
            visual.selected = (clip.clipId == interactionState_.selectedClipId);
            visual.rect.x = clampValue(visual.rect.x, leftInset, rect.right - 50);
            visual.rect.width = std::min(visual.rect.width, std::max(30, rect.right - visual.rect.x - 6));
            interactionState_.playlistClipVisuals.push_back(visual);
        }
        ++lane;
    }
}

void UI::rebuildPianoVisuals(const RECT& rect)
{
    interactionState_.pianoNoteVisuals.clear();
    const int keyWidth = 48;
    const int laneHeight = 22;
    const int noteWidth = 54;

    for (int index = 0; index < 7; ++index)
    {
        UiNoteVisual note{};
        note.lane = index;
        note.step = index * 2;
        note.rect.x = keyWidth + 20 + (index * 36);
        note.rect.y = 30 + ((6 - index) * laneHeight);
        note.rect.width = noteWidth + ((index % 3) * 10);
        note.rect.height = laneHeight - 4;
        note.selected = (interactionState_.selectedNoteIndex == static_cast<std::size_t>(index));
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

