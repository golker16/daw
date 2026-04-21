#include "UI.h"

#include <cstdio>
#include <sstream>
#include <string>

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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

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
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        760, 430,
        nullptr,
        nullptr,
        hInstance_,
        this
    );

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
        throw UiInitializationException("No se pudo crear el menú principal.");
    }

    HMENU fileMenu = CreatePopupMenu();
    HMENU engineMenu = CreatePopupMenu();
    HMENU renderMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    if (fileMenu == nullptr || engineMenu == nullptr || renderMenu == nullptr || helpMenu == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno de los submenús.");
    }

    AppendMenuA(fileMenu, MF_STRING, IdMenuFileExit, "Exit");

    AppendMenuA(engineMenu, MF_STRING, IdMenuEngineStart, "Start Engine");
    AppendMenuA(engineMenu, MF_STRING, IdMenuEngineStop, "Stop Engine");
    AppendMenuA(engineMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(engineMenu, MF_STRING, IdMenuTransportPlay, "Play");
    AppendMenuA(engineMenu, MF_STRING, IdMenuTransportPause, "Pause");
    AppendMenuA(engineMenu, MF_STRING, IdMenuTransportStop, "Stop Transport");
    AppendMenuA(engineMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(engineMenu, MF_STRING, IdMenuGraphRebuild, "Rebuild Graph");
    AppendMenuA(engineMenu, MF_STRING, IdMenuToggleAutomation, "Toggle Automation");
    AppendMenuA(engineMenu, MF_STRING, IdMenuTogglePdc, "Toggle PDC");
    AppendMenuA(engineMenu, MF_STRING, IdMenuToggleAnticipative, "Toggle Anticipative");

    AppendMenuA(renderMenu, MF_STRING, IdMenuRenderOffline, "Offline Render");

    AppendMenuA(helpMenu, MF_STRING, IdMenuHelpAbout, "About");

    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), "File");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(engineMenu), "Engine");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(renderMenu), "Render");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), "Help");

    if (!SetMenu(hwnd_, mainMenu_))
    {
        throw UiInitializationException("No se pudo asociar el menú a la ventana.");
    }
}

void UI::createControls()
{
    startButton_ = CreateWindowA(
        "BUTTON", "Start Engine",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        20, 20, 130, 32,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonStart),
        hInstance_,
        nullptr
    );

    stopButton_ = CreateWindowA(
        "BUTTON", "Stop Engine",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        160, 20, 130, 32,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonStop),
        hInstance_,
        nullptr
    );

    playButton_ = CreateWindowA(
        "BUTTON", "Play",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        300, 20, 100, 32,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonPlay),
        hInstance_,
        nullptr
    );

    pauseButton_ = CreateWindowA(
        "BUTTON", "Pause",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        410, 20, 100, 32,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonPause),
        hInstance_,
        nullptr
    );

    rebuildGraphButton_ = CreateWindowA(
        "BUTTON", "Rebuild Graph",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        520, 20, 120, 32,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonRebuildGraph),
        hInstance_,
        nullptr
    );

    renderOfflineButton_ = CreateWindowA(
        "BUTTON", "Offline Render",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 65, 130, 28,
        hwnd_,
        reinterpret_cast<HMENU>(IdButtonRenderOffline),
        hInstance_,
        nullptr
    );

    automationCheckbox_ = CreateWindowA(
        "BUTTON", "Automation",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        170, 65, 110, 24,
        hwnd_,
        reinterpret_cast<HMENU>(IdCheckboxAutomation),
        hInstance_,
        nullptr
    );

    pdcCheckbox_ = CreateWindowA(
        "BUTTON", "PDC",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        290, 65, 80, 24,
        hwnd_,
        reinterpret_cast<HMENU>(IdCheckboxPdc),
        hInstance_,
        nullptr
    );

    anticipativeCheckbox_ = CreateWindowA(
        "BUTTON", "Anticipative",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        380, 65, 120, 24,
        hwnd_,
        reinterpret_cast<HMENU>(IdCheckboxAnticipative),
        hInstance_,
        nullptr
    );

    statusLabel_ = CreateWindowA(
        "STATIC", "Status: unknown",
        WS_VISIBLE | WS_CHILD,
        20, 115, 700, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelStatus),
        hInstance_,
        nullptr
    );

    backendLabel_ = CreateWindowA(
        "STATIC", "Backend: -",
        WS_VISIBLE | WS_CHILD,
        20, 145, 700, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelBackend),
        hInstance_,
        nullptr
    );

    sampleRateLabel_ = CreateWindowA(
        "STATIC", "Sample Rate: -",
        WS_VISIBLE | WS_CHILD,
        20, 175, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelSampleRate),
        hInstance_,
        nullptr
    );

    blockSizeLabel_ = CreateWindowA(
        "STATIC", "Block Size: -",
        WS_VISIBLE | WS_CHILD,
        260, 175, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelBlockSize),
        hInstance_,
        nullptr
    );

    cpuLabel_ = CreateWindowA(
        "STATIC", "CPU Load: -",
        WS_VISIBLE | WS_CHILD,
        20, 205, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelCpu),
        hInstance_,
        nullptr
    );

    xrunsLabel_ = CreateWindowA(
        "STATIC", "XRuns: -",
        WS_VISIBLE | WS_CHILD,
        260, 205, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelXruns),
        hInstance_,
        nullptr
    );

    deadlineMissesLabel_ = CreateWindowA(
        "STATIC", "Deadline Misses: -",
        WS_VISIBLE | WS_CHILD,
        20, 235, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelDeadlineMisses),
        hInstance_,
        nullptr
    );

    graphVersionLabel_ = CreateWindowA(
        "STATIC", "Graph Version: -",
        WS_VISIBLE | WS_CHILD,
        260, 235, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelGraphVersion),
        hInstance_,
        nullptr
    );

    latencyLabel_ = CreateWindowA(
        "STATIC", "Latency: -",
        WS_VISIBLE | WS_CHILD,
        20, 265, 220, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelLatency),
        hInstance_,
        nullptr
    );

    transportLabel_ = CreateWindowA(
        "STATIC", "Transport: -",
        WS_VISIBLE | WS_CHILD,
        260, 265, 320, 22,
        hwnd_,
        reinterpret_cast<HMENU>(IdLabelTransport),
        hInstance_,
        nullptr
    );

    if (startButton_ == nullptr || stopButton_ == nullptr || playButton_ == nullptr ||
        pauseButton_ == nullptr || rebuildGraphButton_ == nullptr || renderOfflineButton_ == nullptr ||
        automationCheckbox_ == nullptr || pdcCheckbox_ == nullptr || anticipativeCheckbox_ == nullptr ||
        statusLabel_ == nullptr || backendLabel_ == nullptr || sampleRateLabel_ == nullptr ||
        blockSizeLabel_ == nullptr || cpuLabel_ == nullptr || xrunsLabel_ == nullptr ||
        deadlineMissesLabel_ == nullptr || graphVersionLabel_ == nullptr ||
        latencyLabel_ == nullptr || transportLabel_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno o más controles de la interfaz.");
    }
}

void UI::startUiTimer()
{
    if (SetTimer(hwnd_, kUiTimerId, kUiTimerIntervalMs, nullptr) == 0)
    {
        throw UiInitializationException("No se pudo iniciar el timer de polling de UI.");
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
    updateStatusLabel();
    updateMetricLabels();
    updateToggleStates();
    updateWindowTitle();
}

UI::VisibleEngineState UI::buildVisibleEngineState() const
{
    VisibleEngineState snapshot{};

    snapshot.engineState = engine_.getState();
    snapshot.sampleRate = engine_.getCurrentSampleRate();
    snapshot.blockSize = engine_.getCurrentBlockSize();

    const AudioEngine::EngineMetrics metrics = engine_.getMetrics();
    snapshot.xruns = metrics.xruns;
    snapshot.deadlineMisses = metrics.deadlineMisses;
    snapshot.cpuLoadApprox = metrics.cpuLoadApprox;
    snapshot.peakBlockTimeUs = metrics.peakBlockTimeUs;
    snapshot.currentLatencySamples = metrics.currentLatencySamples;
    snapshot.activeGraphVersion = metrics.activeGraphVersion;

    const AudioEngine::TransportInfo transport = engine_.getTransportInfo();
    snapshot.transportState = transport.state;
    snapshot.monitoringEnabled = transport.monitoringEnabled;

    const AudioEngine::EngineConfig& config = engine_.getConfig();
    snapshot.anticipativeProcessingEnabled = config.enableAnticipativeProcessing;
    snapshot.automationEnabled = config.enableSampleAccurateAutomation;
    snapshot.pdcEnabled = config.enablePdc;
    snapshot.offlineRenderEnabled = config.enableOfflineRender;
    snapshot.pluginHostEnabled = config.enablePluginHost;
    snapshot.prefer64BitMix = config.prefer64BitInternalMix;

    snapshot.backendName = engine_.getBackendName();
    snapshot.deviceName = engine_.getCurrentDeviceName();
    snapshot.statusText = engine_.getStatusText();

    return snapshot;
}

void UI::updateStatusLabel()
{
    setStaticText(statusLabel_, visibleState_.statusText);

    std::string backendText = "Backend: " + visibleState_.backendName + " | Device: " + visibleState_.deviceName;
    setStaticText(backendLabel_, backendText);
}

void UI::updateMetricLabels()
{
    setStaticText(sampleRateLabel_, "Sample Rate: " + std::to_string(visibleState_.sampleRate));
    setStaticText(blockSizeLabel_, "Block Size: " + std::to_string(visibleState_.blockSize));

    {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "CPU Load: %.2f%%", visibleState_.cpuLoadApprox * 100.0);
        setStaticText(cpuLabel_, buffer);
    }

    setStaticText(xrunsLabel_, "XRuns: " + std::to_string(visibleState_.xruns));
    setStaticText(deadlineMissesLabel_, "Deadline Misses: " + std::to_string(visibleState_.deadlineMisses));
    setStaticText(graphVersionLabel_, "Graph Version: " + std::to_string(visibleState_.activeGraphVersion));
    setStaticText(latencyLabel_, "Latency: " + std::to_string(visibleState_.currentLatencySamples) + " samples");

    std::string transportText = "Transport: ";
    switch (visibleState_.transportState)
    {
    case AudioEngine::TransportState::Playing:
        transportText += "Playing";
        break;
    case AudioEngine::TransportState::Paused:
        transportText += "Paused";
        break;
    case AudioEngine::TransportState::Stopped:
    default:
        transportText += "Stopped";
        break;
    }

    transportText += " | Monitoring: ";
    transportText += visibleState_.monitoringEnabled ? "On" : "Off";

    setStaticText(transportLabel_, transportText);
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
    oss << kWindowTitleBase
        << " | Graph v" << visibleState_.activeGraphVersion
        << " | SR " << visibleState_.sampleRate
        << " | Block " << visibleState_.blockSize;
    SetWindowTextA(hwnd_, oss.str().c_str());
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

void UI::handleCommand(WORD commandId)
{
    switch (commandId)
    {
    case IdButtonStart:
    case IdMenuEngineStart:
        requestEngineStart();
        break;

    case IdButtonStop:
    case IdMenuEngineStop:
        requestEngineStop();
        break;

    case IdButtonPlay:
    case IdMenuTransportPlay:
        requestTransportPlay();
        break;

    case IdButtonPause:
    case IdMenuTransportPause:
        requestTransportPause();
        break;

    case IdMenuTransportStop:
        requestTransportStop();
        break;

    case IdButtonRebuildGraph:
    case IdMenuGraphRebuild:
        requestGraphRebuild();
        break;

    case IdButtonRenderOffline:
    case IdMenuRenderOffline:
        requestOfflineRender();
        break;

    case IdCheckboxAutomation:
    case IdMenuToggleAutomation:
        requestToggleAutomation();
        break;

    case IdCheckboxPdc:
    case IdMenuTogglePdc:
        requestTogglePdc();
        break;

    case IdCheckboxAnticipative:
    case IdMenuToggleAnticipative:
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

    refreshFromEngineSnapshot();
}

void UI::showAboutDialog() const
{
    std::ostringstream oss;
    oss
        << "DAW Cloud Template\n\n"
        << "Backend: " << visibleState_.backendName << "\n"
        << "Device: " << visibleState_.deviceName << "\n"
        << "Sample Rate: " << visibleState_.sampleRate << "\n"
        << "Block Size: " << visibleState_.blockSize << "\n"
        << "Graph Version: " << visibleState_.activeGraphVersion << "\n"
        << "Latency: " << visibleState_.currentLatencySamples << " samples\n"
        << "64-bit Internal Mix: " << (visibleState_.prefer64BitMix ? "On" : "Off") << "\n"
        << "Automation: " << (visibleState_.automationEnabled ? "On" : "Off") << "\n"
        << "PDC: " << (visibleState_.pdcEnabled ? "On" : "Off") << "\n"
        << "Anticipative Processing: " << (visibleState_.anticipativeProcessingEnabled ? "On" : "Off") << "\n"
        << "Offline Render: " << (visibleState_.offlineRenderEnabled ? "On" : "Off") << "\n"
        << "Plugin Host: " << (visibleState_.pluginHostEnabled ? "On" : "Off") << "\n";

    MessageBoxA(hwnd_, oss.str().c_str(), "About", MB_OK | MB_ICONINFORMATION);
}

void UI::setStaticText(HWND control, const std::string& text) const
{
    if (control != nullptr)
    {
        SetWindowTextA(control, text.c_str());
    }
}
