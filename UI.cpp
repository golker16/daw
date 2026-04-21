#include "UI.h"

#include <algorithm>
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
}

UI::UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine)
    : hInstance_(hInstance),
      nCmdShow_(nCmdShow),
      engine_(engine)
{
    registerWindowClass();
    createMainWindow();
    createMainMenu();
    createControls();
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
    browserPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelBrowser), hInstance_, nullptr);
    channelRackMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuChannelRack), hInstance_, nullptr);
    channelRackPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelChannelRack), hInstance_, nullptr);
    stepSequencerPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelStepSequencer), hInstance_, nullptr);
    pianoRollMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPianoRoll), hInstance_, nullptr);
    pianoRollPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPianoRoll), hInstance_, nullptr);
    playlistMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPlaylist), hInstance_, nullptr);
    playlistPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPlaylist), hInstance_, nullptr);
    mixerMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuMixer), hInstance_, nullptr);
    mixerPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelMixer), hInstance_, nullptr);
    pluginMenuButton_ = CreateWindowA("BUTTON", "v", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdButtonMenuPlugin), hInstance_, nullptr);
    pluginPanel_ = CreateWindowA("STATIC", "", panelStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IdLabelPlugin), hInstance_, nullptr);

    if (engineStartButton_ == nullptr || engineStopButton_ == nullptr || playButton_ == nullptr ||
        stopTransportButton_ == nullptr || recordButton_ == nullptr || patSongButton_ == nullptr ||
        tempoDownButton_ == nullptr || tempoUpButton_ == nullptr || patternPrevButton_ == nullptr ||
        patternNextButton_ == nullptr || snapPrevButton_ == nullptr || snapNextButton_ == nullptr ||
        browserButton_ == nullptr || channelRackButton_ == nullptr || pianoRollButton_ == nullptr ||
        playlistButton_ == nullptr || mixerButton_ == nullptr || pluginButton_ == nullptr ||
        addTrackButton_ == nullptr || addBusButton_ == nullptr || addClipButton_ == nullptr ||
        undoButton_ == nullptr || redoButton_ == nullptr || saveProjectButton_ == nullptr ||
        loadProjectButton_ == nullptr || prevTrackButton_ == nullptr || nextTrackButton_ == nullptr ||
        rebuildGraphButton_ == nullptr || renderOfflineButton_ == nullptr || automationCheckbox_ == nullptr ||
        pdcCheckbox_ == nullptr || anticipativeCheckbox_ == nullptr || statusLabel_ == nullptr ||
        tempoLabel_ == nullptr || patternLabel_ == nullptr || snapLabel_ == nullptr ||
        systemLabel_ == nullptr || hintsLabel_ == nullptr || projectSummaryLabel_ == nullptr ||
        documentLabel_ == nullptr || selectionLabel_ == nullptr || browserMenuButton_ == nullptr ||
        browserPanel_ == nullptr || channelRackMenuButton_ == nullptr || channelRackPanel_ == nullptr ||
        stepSequencerPanel_ == nullptr || pianoRollMenuButton_ == nullptr || pianoRollPanel_ == nullptr ||
        playlistMenuButton_ == nullptr || playlistPanel_ == nullptr || mixerMenuButton_ == nullptr ||
        mixerPanel_ == nullptr || pluginMenuButton_ == nullptr || pluginPanel_ == nullptr)
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
    MoveWindow(automationCheckbox_, kOuterPadding + 804, toolbarY + 42, 92, 22, TRUE);
    MoveWindow(pdcCheckbox_, kOuterPadding + 902, toolbarY + 42, 50, 22, TRUE);
    MoveWindow(anticipativeCheckbox_, kOuterPadding + 958, toolbarY + 42, 96, 22, TRUE);
    MoveWindow(systemLabel_, kOuterPadding + 1064, toolbarY + 41, std::max(160, contentWidth - 1080), 24, TRUE);

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

    ShowWindow(browserMenuButton_, workspace_.browserVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(browserPanel_, workspace_.browserVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.browserVisible)
    {
        MoveWindow(browserMenuButton_, kOuterPadding + browserAreaWidth - 28, y, 28, kSectionHeaderHeight, TRUE);
        MoveWindow(browserPanel_, kOuterPadding, y + kSectionHeaderHeight, browserAreaWidth, workspaceHeight - kSectionHeaderHeight, TRUE);
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
    ShowWindow(stepSequencerPanel_, workspace_.channelRackVisible ? SW_SHOW : SW_HIDE);
    if (workspace_.channelRackVisible)
    {
        MoveWindow(stepSequencerPanel_, workspaceX, y + (channelRackHeight / 2) + kGap, leftWidth, std::max(72, (channelRackHeight / 2) - kGap), TRUE);
    }

    placePanel(workspace_.pianoRollVisible, pianoRollMenuButton_, pianoRollPanel_, workspaceX, y + channelRackHeight + kGap, leftWidth, pianoRollHeight);
    placePanel(workspace_.playlistVisible, playlistMenuButton_, playlistPanel_, workspaceX + leftWidth + kGap, y, rightWidth, playlistHeight);
    placePanel(workspace_.mixerVisible, mixerMenuButton_, mixerPanel_, workspaceX, mixerY, workspaceWidth, kMixerHeight);
    placePanel(workspace_.pluginVisible, pluginMenuButton_, pluginPanel_, workspaceX, pluginY, workspaceWidth, kPluginHeight);

    MoveWindow(hintsLabel_, workspaceX, height - kOuterPadding - 18, workspaceWidth, 18, TRUE);
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
        << " | Status " << visibleState_.statusText;
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
    std::ostringstream browser;
    browser
        << "Browser\n\n"
        << "Packs / Samples\n"
        << "  - Kicks\n"
        << "  - Snares\n"
        << "  - Hats\n"
        << "  - FX\n\n"
        << "Plugin Database\n"
        << "  - Generators\n"
        << "  - Effects\n\n"
        << "Current Project\n"
        << "  - Patterns " << std::max<std::size_t>(1, visibleState_.project.tracks.size()) << "\n"
        << "  - Mixer states " << visibleState_.project.buses.size() << "\n"
        << "  - Automation lanes " << visibleState_.project.tracks.size() << "\n\n"
        << "Session\n"
        << "  - " << (visibleState_.document.sessionPath.empty() ? "<unsaved>" : visibleState_.document.sessionPath) << "\n\n"
        << "Hint: Alt+F8 toggles this browser.";
    setStaticText(browserPanel_, browser.str());

    std::ostringstream rack;
    rack << "Channel Rack\n\n";
    if (visibleState_.project.tracks.empty())
    {
        rack << "No channels yet.\nUse Add Track to create instruments or automation channels.";
    }
    else
    {
        for (std::size_t index = 0; index < visibleState_.project.tracks.size(); ++index)
        {
            const VisibleTrack& track = visibleState_.project.tracks[index];
            rack
                << (index == selectedTrackIndex_ ? "> " : "  ")
                << track.name
                << " | Mute " << boolLabel(track.muted, "On", "Off")
                << " | Pan C"
                << " | Vol " << (track.muted ? "0%" : "80%")
                << " | Mixer " << track.busId
                << "\n";
        }
    }
    setStaticText(channelRackPanel_, rack.str());

    std::ostringstream steps;
    const std::string selectedName =
        visibleState_.selection.selectedTrackName.empty() ? std::string("Selected channel") : visibleState_.selection.selectedTrackName;
    steps
        << "Step Sequencer\n\n"
        << selectedName << "\n"
        << "|x . . x|. . x .|x . . x|. x . .|\n"
        << "Kick  : x . . x . . x . x . . x . x . .\n"
        << "Snare : . . x . . . x . . . x . . . x .\n"
        << "Hat   : x x x x x x x x x x x x x x x x\n"
        << "\nUse this lane for quick drum programming.";
    setStaticText(stepSequencerPanel_, steps.str());

    std::ostringstream piano;
    piano
        << "Piano Roll\n\n"
        << "Target Channel: "
        << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName)
        << "\nSnap: " << currentSnapLabel()
        << "\nGhost notes: visible\n\n"
        << "C5 |----[]--------[]------|\n"
        << "A4 |--[]----[]------------|\n"
        << "F4 |--------[]------[]----|\n"
        << "D4 |[]--------------------|\n"
        << "    1.1   1.2   1.3   1.4\n\n"
        << "Tools: Draw, Paint, Select, Delete, Quantize.";
    setStaticText(pianoRollPanel_, piano.str());

    std::ostringstream playlist;
    playlist
        << "Playlist\n\n"
        << "Timeline: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8\n"
        << "Mode: " << (workspace_.songMode ? "Song arrangement" : "Pattern preview") << "\n\n";

    if (visibleState_.project.tracks.empty())
    {
        playlist << "Track 1: [empty]\n";
    }
    else
    {
        for (const auto& track : visibleState_.project.tracks)
        {
            playlist << track.name << ": ";
            if (track.clips.empty())
            {
                playlist << "[empty]";
            }
            else
            {
                for (const auto& clip : track.clips)
                {
                    playlist << "[" << clip.name << " @" << clip.startTimeSeconds << "s] ";
                }
            }
            playlist << "\n";
        }
    }

    playlist
        << "\nPattern Clips, Audio Clips and Automation Clips share this space."
        << "\nF5 toggles the playlist.";
    setStaticText(playlistPanel_, playlist.str());

    std::ostringstream mixer;
    mixer
        << "Mixer\n\n"
        << "Master | CPU " << static_cast<int>(visibleState_.cpuLoadApprox * 100.0 + 0.5) << "% | Peak "
        << visibleState_.peakBlockTimeUs << " us\n\n";

    if (visibleState_.project.buses.empty())
    {
        mixer << "Insert 1 | Fader -6.0 dB | FX slot 1 Empty | Route -> Master\n";
        mixer << "Insert 2 | Fader -3.0 dB | FX slot 1 Empty | Route -> Master\n";
    }
    else
    {
        for (const auto& bus : visibleState_.project.buses)
        {
            mixer
                << bus.name
                << " | Inputs " << bus.inputTrackIds.size()
                << " | Fader -3.0 dB | Slots 4 | Route -> Master\n";
        }
    }

    mixer << "\nF9 toggles the mixer. Routing, sends and insert FX live here.";
    setStaticText(mixerPanel_, mixer.str());

    std::ostringstream plugin;
    plugin
        << "Plugin / Channel Settings\n\n"
        << "Target: " << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName) << "\n"
        << "Wrapper: " << boolLabel(visibleState_.pluginHostEnabled, "Plugin host enabled", "In-process") << "\n"
        << "Sandbox: " << boolLabel(visibleState_.pluginSandboxEnabled, "On", "Off") << "\n"
        << "64-bit path: " << boolLabel(visibleState_.prefer64BitMix, "On", "Off") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "Sample-accurate", "Off") << "\n\n"
        << "Parameters\n"
        << "  - Gain 0.80\n"
        << "  - Pan 0.00\n"
        << "  - Envelope A 12 ms\n"
        << "  - Filter cutoff 8.4 kHz\n\n"
        << "This pane acts like the channel settings / plugin editor.";
    setStaticText(pluginPanel_, plugin.str());

    setStaticText(
        hintsLabel_,
        "Shortcuts: F5 Playlist | F6 Channel Rack | F7 Piano Roll | F9 Mixer | Alt+F8 Browser");
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

    case IdButtonPlaylist:
    case IdMenuViewPlaylist:
    case IdButtonMenuPlaylist:
        togglePane(WorkspacePane::Playlist);
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
        break;

    case WorkspacePane::ChannelRack:
        if (workspace_.channelRackVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.channelRackVisible = !workspace_.channelRackVisible;
        break;

    case WorkspacePane::PianoRoll:
        if (workspace_.pianoRollVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.pianoRollVisible = !workspace_.pianoRollVisible;
        break;

    case WorkspacePane::Playlist:
        if (workspace_.playlistVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.playlistVisible = !workspace_.playlistVisible;
        break;

    case WorkspacePane::Mixer:
        if (workspace_.mixerVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.mixerVisible = !workspace_.mixerVisible;
        break;

    case WorkspacePane::Plugin:
        if (workspace_.pluginVisible && visibleWorkspacePanelCount() == 1)
        {
            return;
        }
        workspace_.pluginVisible = !workspace_.pluginVisible;
        break;
    }
}

std::string UI::currentSnapLabel() const
{
    static constexpr const char* kSnapModes[] = {"Off", "Line", "Cell", "Beat", "Bar", "Step"};
    return kSnapModes[std::min<std::size_t>(workspace_.snapIndex, 5)];
}

void UI::setStaticText(HWND control, const std::string& text) const
{
    if (control != nullptr)
    {
        SetWindowTextA(control, text.c_str());
    }
}

