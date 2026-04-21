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

public:
    UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void registerWindowClass();
    void createMainWindow();
    void createMainMenu();
    void createControls();

    void startUiTimer();
    void stopUiTimer();
    void refreshFromEngineSnapshot();
    VisibleEngineState buildVisibleEngineState() const;

    void updateStatusLabel();
    void updateMetricLabels();
    void updateProjectLabels();
    void updateToggleStates();
    void updateWindowTitle();

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
    void requestAddTrack();
    void requestAddBus();
    void requestAddClip();
    void requestUndo();
    void requestRedo();
    void requestSaveProject();
    void requestLoadProject();

    void handleCommand(WORD commandId);
    void showAboutDialog() const;
    void selectNextTrack();
    void selectPreviousTrack();
    void setStaticText(HWND control, const std::string& text) const;

private:
    static constexpr const char* kWindowClassName = "DAWCloudTemplateWindowClass";
    static constexpr const char* kWindowTitleBase = "DAW Cloud Template";
    static constexpr UINT_PTR kUiTimerId = 1;
    static constexpr UINT kUiTimerIntervalMs = 250;

    enum ControlId : WORD
    {
        IdButtonStart = 1001,
        IdButtonStop = 1002,
        IdButtonPlay = 1003,
        IdButtonPause = 1004,
        IdButtonRebuildGraph = 1005,
        IdButtonRenderOffline = 1006,
        IdButtonAddTrack = 1007,
        IdButtonAddBus = 1008,
        IdButtonAddClip = 1009,
        IdButtonUndo = 1010,
        IdButtonRedo = 1011,
        IdButtonSaveProject = 1012,
        IdButtonLoadProject = 1013,
        IdButtonPrevTrack = 1014,
        IdButtonNextTrack = 1015,

        IdCheckboxAutomation = 1101,
        IdCheckboxPdc = 1102,
        IdCheckboxAnticipative = 1103,

        IdLabelStatus = 1201,
        IdLabelBackend = 1202,
        IdLabelSampleRate = 1203,
        IdLabelBlockSize = 1204,
        IdLabelCpu = 1205,
        IdLabelXruns = 1206,
        IdLabelDeadlineMisses = 1207,
        IdLabelGraphVersion = 1208,
        IdLabelLatency = 1209,
        IdLabelTransport = 1210,
        IdLabelProjectSummary = 1211,
        IdLabelDocument = 1212,
        IdLabelSelection = 1213,
        IdLabelTrackList = 1214,
        IdLabelBusList = 1215,
        IdLabelError = 1216
    };

    enum MenuId : WORD
    {
        IdMenuFileExit = 2001,
        IdMenuEngineStart = 2002,
        IdMenuEngineStop = 2003,
        IdMenuTransportPlay = 2004,
        IdMenuTransportPause = 2005,
        IdMenuTransportStop = 2006,
        IdMenuGraphRebuild = 2007,
        IdMenuRenderOffline = 2008,
        IdMenuToggleAutomation = 2009,
        IdMenuTogglePdc = 2010,
        IdMenuToggleAnticipative = 2011,
        IdMenuHelpAbout = 2012,
        IdMenuProjectAddTrack = 2013,
        IdMenuProjectAddBus = 2014,
        IdMenuProjectAddClip = 2015,
        IdMenuProjectUndo = 2016,
        IdMenuProjectRedo = 2017,
        IdMenuProjectSave = 2018,
        IdMenuProjectLoad = 2019
    };

private:
    HINSTANCE hInstance_ = nullptr;
    int nCmdShow_ = SW_SHOWNORMAL;
    AudioEngine& engine_;

    HWND hwnd_ = nullptr;
    HMENU mainMenu_ = nullptr;

    HWND startButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND pauseButton_ = nullptr;
    HWND rebuildGraphButton_ = nullptr;
    HWND renderOfflineButton_ = nullptr;
    HWND addTrackButton_ = nullptr;
    HWND addBusButton_ = nullptr;
    HWND addClipButton_ = nullptr;
    HWND undoButton_ = nullptr;
    HWND redoButton_ = nullptr;
    HWND saveProjectButton_ = nullptr;
    HWND loadProjectButton_ = nullptr;
    HWND prevTrackButton_ = nullptr;
    HWND nextTrackButton_ = nullptr;

    HWND automationCheckbox_ = nullptr;
    HWND pdcCheckbox_ = nullptr;
    HWND anticipativeCheckbox_ = nullptr;

    HWND statusLabel_ = nullptr;
    HWND backendLabel_ = nullptr;
    HWND sampleRateLabel_ = nullptr;
    HWND blockSizeLabel_ = nullptr;
    HWND cpuLabel_ = nullptr;
    HWND xrunsLabel_ = nullptr;
    HWND deadlineMissesLabel_ = nullptr;
    HWND graphVersionLabel_ = nullptr;
    HWND latencyLabel_ = nullptr;
    HWND transportLabel_ = nullptr;
    HWND projectSummaryLabel_ = nullptr;
    HWND documentLabel_ = nullptr;
    HWND selectionLabel_ = nullptr;
    HWND trackListLabel_ = nullptr;
    HWND busListLabel_ = nullptr;
    HWND errorLabel_ = nullptr;

    mutable VisibleEngineState visibleState_{};
    std::size_t selectedTrackIndex_ = 0;
};

