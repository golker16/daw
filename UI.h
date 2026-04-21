#pragma once

#include <windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "AudioEngine.h"

class UI
{
public:
    class UiInitializationException : public std::runtime_error
    {
    public:
        explicit UiInitializationException(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    struct VisibleClip
    {
        std::uint32_t clipId = 0;
        std::string name;
        std::string sourceLabel;
        double startTimeSeconds = 0.0;
        double durationSeconds = 0.0;
        bool muted = false;
    };

    struct VisibleTrack
    {
        std::uint32_t trackId = 0;
        std::uint32_t busId = 0;
        std::string name;
        bool armed = false;
        bool muted = false;
        bool solo = false;
        std::vector<VisibleClip> clips;
    };

    struct VisibleBus
    {
        std::uint32_t busId = 0;
        std::string name;
        std::vector<std::uint32_t> inputTrackIds;
    };

    struct VisibleProjectState
    {
        std::string projectName;
        std::string sessionPath;
        std::uint64_t revision = 0;
        bool dirty = false;
        std::uint32_t undoDepth = 0;
        std::uint32_t redoDepth = 0;
        std::vector<VisibleTrack> tracks;
        std::vector<VisibleBus> buses;
    };

    struct SelectionState
    {
        std::uint32_t selectedTrackId = 0;
        std::string selectedTrackName;
        std::uint32_t selectedClipCount = 0;
    };

    struct UiDocumentState
    {
        bool hasProject = false;
        bool dirty = false;
        std::string sessionPath;
        std::string statusSummary;
    };

    struct VisibleEngineState
    {
        AudioEngine::EngineState engineState = AudioEngine::EngineState::Uninitialized;
        AudioEngine::TransportState transportState = AudioEngine::TransportState::Stopped;

        int sampleRate = 0;
        int blockSize = 0;

        std::uint64_t xruns = 0;
        std::uint64_t deadlineMisses = 0;
        std::uint64_t callbackCount = 0;
        std::uint64_t recoveryCount = 0;
        double cpuLoadApprox = 0.0;
        double averageBlockTimeUs = 0.0;
        double peakBlockTimeUs = 0.0;
        std::uint32_t currentLatencySamples = 0;
        std::uint64_t activeGraphVersion = 0;

        bool anticipativeProcessingEnabled = false;
        bool automationEnabled = false;
        bool pdcEnabled = false;
        bool offlineRenderEnabled = false;
        bool pluginHostEnabled = false;
        bool pluginSandboxEnabled = false;
        bool prefer64BitMix = false;
        bool monitoringEnabled = false;
        bool safeMode = false;

        std::string backendName;
        std::string deviceName;
        std::string statusText;
        std::string lastErrorMessage;

        VisibleProjectState project{};
        SelectionState selection{};
        UiDocumentState document{};
    };

    enum class WorkspacePane
    {
        Browser,
        ChannelRack,
        PianoRoll,
        Playlist,
        Mixer,
        Plugin
    };

    struct WorkspaceState
    {
        bool browserVisible = true;
        bool channelRackVisible = true;
        bool pianoRollVisible = true;
        bool playlistVisible = true;
        bool mixerVisible = true;
        bool pluginVisible = true;
        bool songMode = true;
        bool recordArmed = false;
        int activePattern = 1;
        double tempoBpm = 120.0;
        std::size_t snapIndex = 2;
    };

public:
    UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void registerWindowClass();
    void createMainWindow();
    void createMainMenu();
    void createControls();
    void layoutControls();

    void startUiTimer();
    void stopUiTimer();
    void refreshFromEngineSnapshot();
    VisibleEngineState buildVisibleEngineState() const;

    void updateTransportControls();
    void updateStatusLabel();
    void updateMetricLabels();
    void updateProjectLabels();
    void updateToggleStates();
    void updateWindowTitle();
    void updateWorkspacePanels();
    void updateViewButtons();

    void requestEngineStart();
    void requestEngineStop();
    void requestTransportPlay();
    void requestTransportPause();
    void requestTransportStop();
    void requestGraphRebuild();
    void requestOfflineRender();
    void requestToggleAutomation();
    void requestTogglePdc();
    void requestToggleAnticipativeProcessing();
    void requestTempoChange(double bpm);
    void requestAddTrack();
    void requestAddBus();
    void requestAddClip();
    void requestNewProject();
    void requestUndo();
    void requestRedo();
    void requestSaveProject();
    void requestLoadProject();

    void handleCommand(WORD commandId);
    bool handleKeyDown(WPARAM wParam, LPARAM lParam);
    void showAboutDialog() const;
    void selectNextTrack();
    void selectPreviousTrack();
    void selectNextPattern();
    void selectPreviousPattern();
    void cycleSnap(int delta);
    void togglePane(WorkspacePane pane);
    std::string currentSnapLabel() const;
    void setStaticText(HWND control, const std::string& text) const;

private:
    static constexpr const char* kWindowClassName = "DAWCloudTemplateWindowClass";
    static constexpr const char* kWindowTitleBase = "DAW Cloud Template";
    static constexpr UINT_PTR kUiTimerId = 1;
    static constexpr UINT kUiTimerIntervalMs = 250;

    enum ControlId : WORD
    {
        IdButtonEngineStart = 1001,
        IdButtonEngineStop = 1002,
        IdButtonPlay = 1003,
        IdButtonStopTransport = 1004,
        IdButtonRecord = 1005,
        IdButtonPatSong = 1006,
        IdButtonTempoDown = 1007,
        IdButtonTempoUp = 1008,
        IdButtonPatternPrev = 1009,
        IdButtonPatternNext = 1010,
        IdButtonSnapPrev = 1011,
        IdButtonSnapNext = 1012,
        IdButtonBrowser = 1013,
        IdButtonChannelRack = 1014,
        IdButtonPianoRoll = 1015,
        IdButtonPlaylist = 1016,
        IdButtonMixer = 1017,
        IdButtonPlugin = 1018,
        IdButtonAddTrack = 1019,
        IdButtonAddBus = 1020,
        IdButtonAddClip = 1021,
        IdButtonUndo = 1022,
        IdButtonRedo = 1023,
        IdButtonSaveProject = 1024,
        IdButtonLoadProject = 1025,
        IdButtonPrevTrack = 1026,
        IdButtonNextTrack = 1027,
        IdButtonRebuildGraph = 1028,
        IdButtonRenderOffline = 1029,
        IdButtonMenuBrowser = 1030,
        IdButtonMenuChannelRack = 1031,
        IdButtonMenuPianoRoll = 1032,
        IdButtonMenuPlaylist = 1033,
        IdButtonMenuMixer = 1034,
        IdButtonMenuPlugin = 1035,

        IdCheckboxAutomation = 1101,
        IdCheckboxPdc = 1102,
        IdCheckboxAnticipative = 1103,

        IdLabelStatus = 1201,
        IdLabelTempo = 1202,
        IdLabelPattern = 1203,
        IdLabelSnap = 1204,
        IdLabelSystem = 1205,
        IdLabelHints = 1206,
        IdLabelBrowser = 1207,
        IdLabelChannelRack = 1208,
        IdLabelStepSequencer = 1209,
        IdLabelPianoRoll = 1210,
        IdLabelPlaylist = 1211,
        IdLabelMixer = 1212,
        IdLabelPlugin = 1213,
        IdLabelProjectSummary = 1214,
        IdLabelDocument = 1215,
        IdLabelSelection = 1216
    };

    enum MenuId : WORD
    {
        IdMenuFileNew = 2001,
        IdMenuFileOpen = 2002,
        IdMenuFileSave = 2003,
        IdMenuFileExit = 2004,
        IdMenuEditUndo = 2005,
        IdMenuEditRedo = 2006,
        IdMenuAddTrack = 2007,
        IdMenuAddBus = 2008,
        IdMenuAddClip = 2009,
        IdMenuPatternsPrev = 2010,
        IdMenuPatternsNext = 2011,
        IdMenuViewBrowser = 2012,
        IdMenuViewChannelRack = 2013,
        IdMenuViewPianoRoll = 2014,
        IdMenuViewPlaylist = 2015,
        IdMenuViewMixer = 2016,
        IdMenuViewPlugin = 2017,
        IdMenuOptionsAutomation = 2018,
        IdMenuOptionsPdc = 2019,
        IdMenuOptionsAnticipative = 2020,
        IdMenuOptionsPatSong = 2021,
        IdMenuToolsStartEngine = 2022,
        IdMenuToolsStopEngine = 2023,
        IdMenuToolsRebuildGraph = 2024,
        IdMenuToolsRenderOffline = 2025,
        IdMenuHelpAbout = 2026
    };

private:
    HINSTANCE hInstance_ = nullptr;
    int nCmdShow_ = SW_SHOWNORMAL;
    AudioEngine& engine_;

    HWND hwnd_ = nullptr;
    HMENU mainMenu_ = nullptr;

    HWND engineStartButton_ = nullptr;
    HWND engineStopButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND stopTransportButton_ = nullptr;
    HWND recordButton_ = nullptr;
    HWND patSongButton_ = nullptr;
    HWND tempoDownButton_ = nullptr;
    HWND tempoUpButton_ = nullptr;
    HWND patternPrevButton_ = nullptr;
    HWND patternNextButton_ = nullptr;
    HWND snapPrevButton_ = nullptr;
    HWND snapNextButton_ = nullptr;
    HWND browserButton_ = nullptr;
    HWND channelRackButton_ = nullptr;
    HWND pianoRollButton_ = nullptr;
    HWND playlistButton_ = nullptr;
    HWND mixerButton_ = nullptr;
    HWND pluginButton_ = nullptr;
    HWND addTrackButton_ = nullptr;
    HWND addBusButton_ = nullptr;
    HWND addClipButton_ = nullptr;
    HWND undoButton_ = nullptr;
    HWND redoButton_ = nullptr;
    HWND saveProjectButton_ = nullptr;
    HWND loadProjectButton_ = nullptr;
    HWND prevTrackButton_ = nullptr;
    HWND nextTrackButton_ = nullptr;
    HWND rebuildGraphButton_ = nullptr;
    HWND renderOfflineButton_ = nullptr;

    HWND automationCheckbox_ = nullptr;
    HWND pdcCheckbox_ = nullptr;
    HWND anticipativeCheckbox_ = nullptr;

    HWND statusLabel_ = nullptr;
    HWND tempoLabel_ = nullptr;
    HWND patternLabel_ = nullptr;
    HWND snapLabel_ = nullptr;
    HWND systemLabel_ = nullptr;
    HWND hintsLabel_ = nullptr;
    HWND projectSummaryLabel_ = nullptr;
    HWND documentLabel_ = nullptr;
    HWND selectionLabel_ = nullptr;

    HWND browserMenuButton_ = nullptr;
    HWND browserPanel_ = nullptr;
    HWND channelRackMenuButton_ = nullptr;
    HWND channelRackPanel_ = nullptr;
    HWND stepSequencerPanel_ = nullptr;
    HWND pianoRollMenuButton_ = nullptr;
    HWND pianoRollPanel_ = nullptr;
    HWND playlistMenuButton_ = nullptr;
    HWND playlistPanel_ = nullptr;
    HWND mixerMenuButton_ = nullptr;
    HWND mixerPanel_ = nullptr;
    HWND pluginMenuButton_ = nullptr;
    HWND pluginPanel_ = nullptr;

    mutable VisibleEngineState visibleState_{};
    WorkspaceState workspace_{};
    std::size_t selectedTrackIndex_ = 0;
};


