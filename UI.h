#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>

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

    struct VisibleEngineState
    {
        AudioEngine::EngineState engineState = AudioEngine::EngineState::Uninitialized;
        AudioEngine::TransportState transportState = AudioEngine::TransportState::Stopped;

        int sampleRate = 0;
        int blockSize = 0;

        std::uint64_t xruns = 0;
        std::uint64_t deadlineMisses = 0;
        double cpuLoadApprox = 0.0;
        double peakBlockTimeUs = 0.0;
        std::uint32_t currentLatencySamples = 0;
        std::uint64_t activeGraphVersion = 0;

        bool anticipativeProcessingEnabled = false;
        bool automationEnabled = false;
        bool pdcEnabled = false;
        bool offlineRenderEnabled = false;
        bool pluginHostEnabled = false;
        bool prefer64BitMix = false;
        bool monitoringEnabled = false;

        std::string backendName;
        std::string deviceName;
        std::string statusText;
    };

public:
    UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine);
    int run();

private:
    // =========================================================
    // Win32 message dispatch
    // =========================================================

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // =========================================================
    // Window/bootstrap
    // =========================================================

    void registerWindowClass();
    void createMainWindow();
    void createMainMenu();
    void createControls();

    // =========================================================
    // Polling / refresh
    // =========================================================

    void startUiTimer();
    void stopUiTimer();
    void refreshFromEngineSnapshot();
    VisibleEngineState buildVisibleEngineState() const;

    // =========================================================
    // Rendering UI state
    // =========================================================

    void updateStatusLabel();
    void updateMetricLabels();
    void updateToggleStates();
    void updateWindowTitle();

    // =========================================================
    // Command/request helpers
    // =========================================================

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

    // =========================================================
    // UI helpers
    // =========================================================

    void handleCommand(WORD commandId);
    void showAboutDialog() const;
    void setStaticText(HWND control, const std::string& text) const;

private:
    // =========================================================
    // Constants
    // =========================================================

    static constexpr const char* kWindowClassName = "DAWCloudTemplateWindowClass";
    static constexpr const char* kWindowTitleBase = "DAW Cloud Template";
    static constexpr UINT_PTR kUiTimerId = 1;
    static constexpr UINT kUiTimerIntervalMs = 250;

    // =========================================================
    // Command IDs
    // =========================================================

    enum ControlId : WORD
    {
        IdButtonStart = 1001,
        IdButtonStop = 1002,
        IdButtonPlay = 1003,
        IdButtonPause = 1004,
        IdButtonRebuildGraph = 1005,
        IdButtonRenderOffline = 1006,

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
        IdLabelTransport = 1210
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
        IdMenuHelpAbout = 2012
    };

private:
    // =========================================================
    // App references
    // =========================================================

    HINSTANCE hInstance_ = nullptr;
    int nCmdShow_ = SW_SHOWNORMAL;
    AudioEngine& engine_;

    // =========================================================
    // Main window
    // =========================================================

    HWND hwnd_ = nullptr;
    HMENU mainMenu_ = nullptr;

    // =========================================================
    // Command controls
    // =========================================================

    HWND startButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND pauseButton_ = nullptr;
    HWND rebuildGraphButton_ = nullptr;
    HWND renderOfflineButton_ = nullptr;

    HWND automationCheckbox_ = nullptr;
    HWND pdcCheckbox_ = nullptr;
    HWND anticipativeCheckbox_ = nullptr;

    // =========================================================
    // Read-only metric/status controls
    // =========================================================

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

    // =========================================================
    // Cached snapshot for UI rendering
    // =========================================================

    VisibleEngineState visibleState_{};
};
