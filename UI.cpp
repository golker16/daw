#include "UI.h"

#include <algorithm>
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
        1120, 760,
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
    HMENU engineMenu = CreatePopupMenu();
    HMENU projectMenu = CreatePopupMenu();
    HMENU renderMenu = CreatePopupMenu();
    HMENU helpMenu = CreatePopupMenu();

    if (fileMenu == nullptr || engineMenu == nullptr || projectMenu == nullptr || renderMenu == nullptr || helpMenu == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno de los submenus.");
    }

    AppendMenuA(fileMenu, MF_STRING, IdMenuProjectLoad, "Load Session");
    AppendMenuA(fileMenu, MF_STRING, IdMenuProjectSave, "Save Session");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
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

    AppendMenuA(projectMenu, MF_STRING, IdMenuProjectAddTrack, "Add Track");
    AppendMenuA(projectMenu, MF_STRING, IdMenuProjectAddBus, "Add Bus");
    AppendMenuA(projectMenu, MF_STRING, IdMenuProjectAddClip, "Add Clip To Selected Track");
    AppendMenuA(projectMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(projectMenu, MF_STRING, IdMenuProjectUndo, "Undo");
    AppendMenuA(projectMenu, MF_STRING, IdMenuProjectRedo, "Redo");

    AppendMenuA(renderMenu, MF_STRING, IdMenuRenderOffline, "Offline Render");

    AppendMenuA(helpMenu, MF_STRING, IdMenuHelpAbout, "About");

    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), "File");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(engineMenu), "Engine");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(projectMenu), "Project");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(renderMenu), "Render");
    AppendMenuA(mainMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), "Help");

    if (!SetMenu(hwnd_, mainMenu_))
    {
        throw UiInitializationException("No se pudo asociar el menu a la ventana.");
    }
}

void UI::createControls()
{
    startButton_ = CreateWindowA("BUTTON", "Start", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 20, 90, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonStart), hInstance_, nullptr);
    stopButton_ = CreateWindowA("BUTTON", "Stop", WS_VISIBLE | WS_CHILD, 115, 20, 90, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonStop), hInstance_, nullptr);
    playButton_ = CreateWindowA("BUTTON", "Play", WS_VISIBLE | WS_CHILD, 210, 20, 90, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonPlay), hInstance_, nullptr);
    pauseButton_ = CreateWindowA("BUTTON", "Pause", WS_VISIBLE | WS_CHILD, 305, 20, 90, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonPause), hInstance_, nullptr);
    rebuildGraphButton_ = CreateWindowA("BUTTON", "Rebuild Graph", WS_VISIBLE | WS_CHILD, 400, 20, 130, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonRebuildGraph), hInstance_, nullptr);
    renderOfflineButton_ = CreateWindowA("BUTTON", "Offline Render", WS_VISIBLE | WS_CHILD, 535, 20, 130, 30, hwnd_, reinterpret_cast<HMENU>(IdButtonRenderOffline), hInstance_, nullptr);

    addTrackButton_ = CreateWindowA("BUTTON", "Add Track", WS_VISIBLE | WS_CHILD, 20, 60, 110, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonAddTrack), hInstance_, nullptr);
    addBusButton_ = CreateWindowA("BUTTON", "Add Bus", WS_VISIBLE | WS_CHILD, 135, 60, 100, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonAddBus), hInstance_, nullptr);
    addClipButton_ = CreateWindowA("BUTTON", "Add Clip", WS_VISIBLE | WS_CHILD, 240, 60, 100, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonAddClip), hInstance_, nullptr);
    undoButton_ = CreateWindowA("BUTTON", "Undo", WS_VISIBLE | WS_CHILD, 345, 60, 80, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonUndo), hInstance_, nullptr);
    redoButton_ = CreateWindowA("BUTTON", "Redo", WS_VISIBLE | WS_CHILD, 430, 60, 80, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonRedo), hInstance_, nullptr);
    saveProjectButton_ = CreateWindowA("BUTTON", "Save Session", WS_VISIBLE | WS_CHILD, 515, 60, 110, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonSaveProject), hInstance_, nullptr);
    loadProjectButton_ = CreateWindowA("BUTTON", "Load Session", WS_VISIBLE | WS_CHILD, 630, 60, 110, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonLoadProject), hInstance_, nullptr);
    prevTrackButton_ = CreateWindowA("BUTTON", "< Track", WS_VISIBLE | WS_CHILD, 745, 60, 85, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonPrevTrack), hInstance_, nullptr);
    nextTrackButton_ = CreateWindowA("BUTTON", "Track >", WS_VISIBLE | WS_CHILD, 835, 60, 85, 28, hwnd_, reinterpret_cast<HMENU>(IdButtonNextTrack), hInstance_, nullptr);

    automationCheckbox_ = CreateWindowA("BUTTON", "Automation", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 680, 24, 110, 24, hwnd_, reinterpret_cast<HMENU>(IdCheckboxAutomation), hInstance_, nullptr);
    pdcCheckbox_ = CreateWindowA("BUTTON", "PDC", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 795, 24, 70, 24, hwnd_, reinterpret_cast<HMENU>(IdCheckboxPdc), hInstance_, nullptr);
    anticipativeCheckbox_ = CreateWindowA("BUTTON", "Anticipative", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 870, 24, 110, 24, hwnd_, reinterpret_cast<HMENU>(IdCheckboxAnticipative), hInstance_, nullptr);

    statusLabel_ = CreateWindowA("STATIC", "Status: -", WS_VISIBLE | WS_CHILD, 20, 110, 1040, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelStatus), hInstance_, nullptr);
    backendLabel_ = CreateWindowA("STATIC", "Backend: -", WS_VISIBLE | WS_CHILD, 20, 138, 1040, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelBackend), hInstance_, nullptr);
    sampleRateLabel_ = CreateWindowA("STATIC", "Sample Rate: -", WS_VISIBLE | WS_CHILD, 20, 172, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelSampleRate), hInstance_, nullptr);
    blockSizeLabel_ = CreateWindowA("STATIC", "Block Size: -", WS_VISIBLE | WS_CHILD, 250, 172, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelBlockSize), hInstance_, nullptr);
    cpuLabel_ = CreateWindowA("STATIC", "CPU Load: -", WS_VISIBLE | WS_CHILD, 480, 172, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelCpu), hInstance_, nullptr);
    xrunsLabel_ = CreateWindowA("STATIC", "XRuns: -", WS_VISIBLE | WS_CHILD, 710, 172, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelXruns), hInstance_, nullptr);
    deadlineMissesLabel_ = CreateWindowA("STATIC", "Deadline Misses: -", WS_VISIBLE | WS_CHILD, 20, 198, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelDeadlineMisses), hInstance_, nullptr);
    graphVersionLabel_ = CreateWindowA("STATIC", "Graph Version: -", WS_VISIBLE | WS_CHILD, 250, 198, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelGraphVersion), hInstance_, nullptr);
    latencyLabel_ = CreateWindowA("STATIC", "Latency: -", WS_VISIBLE | WS_CHILD, 480, 198, 220, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelLatency), hInstance_, nullptr);
    transportLabel_ = CreateWindowA("STATIC", "Transport: -", WS_VISIBLE | WS_CHILD, 710, 198, 320, 22, hwnd_, reinterpret_cast<HMENU>(IdLabelTransport), hInstance_, nullptr);

    projectSummaryLabel_ = CreateWindowA("STATIC", "Project: -", WS_VISIBLE | WS_CHILD, 20, 242, 1040, 24, hwnd_, reinterpret_cast<HMENU>(IdLabelProjectSummary), hInstance_, nullptr);
    documentLabel_ = CreateWindowA("STATIC", "Document: -", WS_VISIBLE | WS_CHILD, 20, 270, 1040, 40, hwnd_, reinterpret_cast<HMENU>(IdLabelDocument), hInstance_, nullptr);
    selectionLabel_ = CreateWindowA("STATIC", "Selection: -", WS_VISIBLE | WS_CHILD, 20, 318, 1040, 24, hwnd_, reinterpret_cast<HMENU>(IdLabelSelection), hInstance_, nullptr);

    trackListLabel_ = CreateWindowA("STATIC", "Tracks", WS_VISIBLE | WS_CHILD | WS_BORDER, 20, 352, 660, 320, hwnd_, reinterpret_cast<HMENU>(IdLabelTrackList), hInstance_, nullptr);
    busListLabel_ = CreateWindowA("STATIC", "Buses", WS_VISIBLE | WS_CHILD | WS_BORDER, 700, 352, 360, 150, hwnd_, reinterpret_cast<HMENU>(IdLabelBusList), hInstance_, nullptr);
    errorLabel_ = CreateWindowA("STATIC", "Last error: none", WS_VISIBLE | WS_CHILD | WS_BORDER, 700, 522, 360, 150, hwnd_, reinterpret_cast<HMENU>(IdLabelError), hInstance_, nullptr);

    if (startButton_ == nullptr || stopButton_ == nullptr || playButton_ == nullptr || pauseButton_ == nullptr ||
        rebuildGraphButton_ == nullptr || renderOfflineButton_ == nullptr || addTrackButton_ == nullptr ||
        addBusButton_ == nullptr || addClipButton_ == nullptr || undoButton_ == nullptr || redoButton_ == nullptr ||
        saveProjectButton_ == nullptr || loadProjectButton_ == nullptr || prevTrackButton_ == nullptr ||
        nextTrackButton_ == nullptr || automationCheckbox_ == nullptr || pdcCheckbox_ == nullptr ||
        anticipativeCheckbox_ == nullptr || statusLabel_ == nullptr || backendLabel_ == nullptr ||
        sampleRateLabel_ == nullptr || blockSizeLabel_ == nullptr || cpuLabel_ == nullptr ||
        xrunsLabel_ == nullptr || deadlineMissesLabel_ == nullptr || graphVersionLabel_ == nullptr ||
        latencyLabel_ == nullptr || transportLabel_ == nullptr || projectSummaryLabel_ == nullptr ||
        documentLabel_ == nullptr || selectionLabel_ == nullptr || trackListLabel_ == nullptr ||
        busListLabel_ == nullptr || errorLabel_ == nullptr)
    {
        throw UiInitializationException("No se pudo crear uno o mas controles de la interfaz.");
    }
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

    updateStatusLabel();
    updateMetricLabels();
    updateProjectLabels();
    updateToggleStates();
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
                clip.sourceType == AudioEngine::ClipSourceType::GeneratedTone ? "Tone" : "Audio File",
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
        << "Callbacks: " << snapshot.callbackCount
        << " | Recoveries: " << snapshot.recoveryCount
        << " | Cached clips: " << engineSnapshot.metrics.cachedClipCount
        << " | Safe mode: " << (snapshot.safeMode ? "On" : "Off");
    snapshot.document.statusSummary = summary.str();

    return snapshot;
}

void UI::updateStatusLabel()
{
    setStaticText(statusLabel_, visibleState_.statusText);
    setStaticText(backendLabel_, "Backend: " + visibleState_.backendName + " | Device: " + visibleState_.deviceName);
}

void UI::updateMetricLabels()
{
    setStaticText(sampleRateLabel_, "Sample Rate: " + std::to_string(visibleState_.sampleRate));
    setStaticText(blockSizeLabel_, "Block Size: " + std::to_string(visibleState_.blockSize));

    {
        char buffer[128]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "CPU Load: %.2f%% | Avg block: %.2f us | Peak: %.2f us",
            visibleState_.cpuLoadApprox * 100.0,
            visibleState_.averageBlockTimeUs,
            visibleState_.peakBlockTimeUs);
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
    transportText += " | Plugin host: ";
    transportText += visibleState_.pluginHostEnabled ? "On" : "Off";
    transportText += " | Sandbox: ";
    transportText += visibleState_.pluginSandboxEnabled ? "On" : "Off";

    setStaticText(transportLabel_, transportText);
}

void UI::updateProjectLabels()
{
    std::ostringstream projectSummary;
    projectSummary
        << "Project: " << visibleState_.project.projectName
        << " | Revision: " << visibleState_.project.revision
        << " | Tracks: " << visibleState_.project.tracks.size()
        << " | Buses: " << visibleState_.project.buses.size()
        << " | Undo: " << visibleState_.project.undoDepth
        << " | Redo: " << visibleState_.project.redoDepth
        << " | Dirty: " << (visibleState_.project.dirty ? "Yes" : "No");
    setStaticText(projectSummaryLabel_, projectSummary.str());

    std::ostringstream document;
    document
        << "Document: " << (visibleState_.document.sessionPath.empty() ? "<unsaved>" : visibleState_.document.sessionPath)
        << "\r\n"
        << visibleState_.document.statusSummary;
    setStaticText(documentLabel_, document.str());

    std::ostringstream selection;
    selection
        << "Selection: "
        << (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName)
        << " | TrackId: " << visibleState_.selection.selectedTrackId
        << " | Clips: " << visibleState_.selection.selectedClipCount;
    setStaticText(selectionLabel_, selection.str());

    std::ostringstream tracks;
    tracks << "Tracks\r\n\r\n";
    if (visibleState_.project.tracks.empty())
    {
        tracks << "No tracks in project.";
    }
    else
    {
        for (std::size_t index = 0; index < visibleState_.project.tracks.size(); ++index)
        {
            const VisibleTrack& track = visibleState_.project.tracks[index];
            tracks
                << (index == selectedTrackIndex_ ? "> " : "  ")
                << track.name
                << " (id " << track.trackId << ", bus " << track.busId << ")";

            if (track.armed)
            {
                tracks << " [armed]";
            }
            if (track.muted)
            {
                tracks << " [muted]";
            }
            if (track.solo)
            {
                tracks << " [solo]";
            }

            tracks << "\r\n";

            if (track.clips.empty())
            {
                tracks << "    no clips\r\n";
            }
            else
            {
                for (const auto& clip : track.clips)
                {
                    tracks
                        << "    - " << clip.name
                        << " [" << clip.sourceLabel << "]"
                        << " start " << clip.startTimeSeconds
                        << "s len " << clip.durationSeconds << "s";
                    if (clip.muted)
                    {
                        tracks << " [muted]";
                    }
                    tracks << "\r\n";
                }
            }

            tracks << "\r\n";
        }
    }
    setStaticText(trackListLabel_, tracks.str());

    std::ostringstream buses;
    buses << "Buses\r\n\r\n";
    if (visibleState_.project.buses.empty())
    {
        buses << "No buses.";
    }
    else
    {
        for (const auto& bus : visibleState_.project.buses)
        {
            buses << bus.name << " (id " << bus.busId << ") <- ";
            if (bus.inputTrackIds.empty())
            {
                buses << "no tracks";
            }
            else
            {
                for (std::size_t index = 0; index < bus.inputTrackIds.size(); ++index)
                {
                    if (index > 0)
                    {
                        buses << ", ";
                    }
                    buses << bus.inputTrackIds[index];
                }
            }
            buses << "\r\n";
        }
    }
    setStaticText(busListLabel_, buses.str());

    setStaticText(
        errorLabel_,
        "Last error:\r\n" +
        (visibleState_.lastErrorMessage.empty() ? std::string("none") : visibleState_.lastErrorMessage));
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

void UI::requestAddTrack()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::AddTrack;
    command.textValue = "Track " + std::to_string(visibleState_.project.tracks.size() + 1);
    engine_.postCommand(command);
}

void UI::requestAddBus()
{
    AudioEngine::EngineCommand command{};
    command.type = AudioEngine::CommandType::AddBus;
    command.textValue = "Bus " + std::to_string(visibleState_.project.buses.size() + 1);
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
    command.textValue = "Clip " + std::to_string(visibleState_.selection.selectedClipCount + 1);
    engine_.postCommand(command);
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

    case IdButtonAddTrack:
    case IdMenuProjectAddTrack:
        requestAddTrack();
        break;

    case IdButtonAddBus:
    case IdMenuProjectAddBus:
        requestAddBus();
        break;

    case IdButtonAddClip:
    case IdMenuProjectAddClip:
        requestAddClip();
        break;

    case IdButtonUndo:
    case IdMenuProjectUndo:
        requestUndo();
        break;

    case IdButtonRedo:
    case IdMenuProjectRedo:
        requestRedo();
        break;

    case IdButtonSaveProject:
    case IdMenuProjectSave:
        requestSaveProject();
        break;

    case IdButtonLoadProject:
    case IdMenuProjectLoad:
        requestLoadProject();
        break;

    case IdButtonPrevTrack:
        selectPreviousTrack();
        break;

    case IdButtonNextTrack:
        selectNextTrack();
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
        << "Project: " << visibleState_.project.projectName << "\n"
        << "Sample Rate: " << visibleState_.sampleRate << "\n"
        << "Block Size: " << visibleState_.blockSize << "\n"
        << "Graph Version: " << visibleState_.activeGraphVersion << "\n"
        << "Latency: " << visibleState_.currentLatencySamples << " samples\n"
        << "64-bit Internal Mix: " << (visibleState_.prefer64BitMix ? "On" : "Off") << "\n"
        << "Automation: " << (visibleState_.automationEnabled ? "On" : "Off") << "\n"
        << "PDC: " << (visibleState_.pdcEnabled ? "On" : "Off") << "\n"
        << "Anticipative Processing: " << (visibleState_.anticipativeProcessingEnabled ? "On" : "Off") << "\n"
        << "Plugin Host: " << (visibleState_.pluginHostEnabled ? "On" : "Off") << "\n"
        << "Sandbox: " << (visibleState_.pluginSandboxEnabled ? "On" : "Off") << "\n";

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

void UI::setStaticText(HWND control, const std::string& text) const
{
    if (control != nullptr)
    {
        SetWindowTextA(control, text.c_str());
    }
}
