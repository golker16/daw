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

    struct VisiblePluginDescriptor
    {
        std::string name;
        std::string vendor;
        std::string format;
        std::string processModeLabel;
        std::uint32_t latencySamples = 0;
        bool supportsDoublePrecision = false;
        bool supportsSampleAccurateAutomation = false;
    };

    struct PluginManagerState
    {
        bool visible = false;
        std::string searchText;
        std::size_t selectedIndex = 0;
        std::vector<VisiblePluginDescriptor> loadedPlugins;
        std::vector<VisiblePluginDescriptor> filteredPlugins;
        std::vector<std::string> searchPaths;
        std::vector<std::string> favorites;
        std::vector<std::string> blacklist;
        std::string statusText;
    };

    enum class SurfaceKind
    {
        None,
        Browser,
        ChannelRack,
        StepSequencer,
        PianoRoll,
        Playlist,
        Mixer,
        Plugin
    };

    struct SurfaceRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    struct UiClipVisual
    {
        std::uint32_t clipId = 0;
        std::uint32_t trackId = 0;
        SurfaceRect rect{};
        bool selected = false;
        bool automation = false;
        bool resizeLeftHot = false;
        bool resizeRightHot = false;
    };

    struct UiNoteVisual
    {
        int lane = 0;
        int step = 0;
        SurfaceRect rect{};
        bool selected = false;
        bool resizeRightHot = false;
    };

    struct SurfaceInteractionState
    {
        SurfaceKind activeSurface = SurfaceKind::None;
        bool mouseDown = false;
        bool draggingClip = false;
        bool draggingNote = false;
        bool resizingClipLeft = false;
        bool resizingClipRight = false;
        bool resizingNote = false;
        bool marqueeActive = false;
        bool browserDragActive = false;
        bool editingAutomationPoint = false;
        POINT dragStart{};
        POINT dragCurrent{};
        std::uint32_t selectedClipId = 0;
        std::size_t selectedNoteIndex = static_cast<std::size_t>(-1);
        std::size_t selectedBrowserItemIndex = static_cast<std::size_t>(-1);
        std::size_t selectedAutomationLaneIndex = static_cast<std::size_t>(-1);
        std::size_t selectedAutomationPointIndex = static_cast<std::size_t>(-1);
        RECT marqueeRect{};
        std::vector<UiClipVisual> playlistClipVisuals;
        std::vector<UiNoteVisual> pianoNoteVisuals;
    };

    struct DockedPaneState
    {
        WorkspacePane pane = WorkspacePane::Playlist;
        RECT bounds{};
        bool detached = false;
        bool visible = true;
        std::string title;
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

    enum class EditorTool
    {
        Draw,
        Paint,
        Select,
        DeleteTool,
        Slice,
        Mute
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
        std::size_t browserTabIndex = 0;
        std::size_t playlistZoomIndex = 2;
        std::size_t pianoZoomIndex = 2;
        EditorTool playlistTool = EditorTool::Draw;
        EditorTool pianoTool = EditorTool::Draw;
        WorkspacePane focusedPane = WorkspacePane::Playlist;
    };

    struct BrowserEntry
    {
        std::string category;
        std::string label;
        std::string subtitle;
        bool favorite = false;
    };

    struct ChannelStepState
    {
        bool enabled = false;
        int velocity = 100;
    };

    struct PianoNoteState
    {
        int lane = 0;
        int step = 0;
        int length = 2;
        int velocity = 100;
        bool accent = false;
        bool slide = false;
        bool selected = false;
    };

    struct PatternLaneState
    {
        std::uint32_t trackId = 0;
        std::string name;
        std::vector<ChannelStepState> steps;
        std::vector<PianoNoteState> notes;
        int swing = 0;
        int shuffle = 0;
    };

    struct PatternState
    {
        int patternNumber = 1;
        std::string name;
        std::vector<PatternLaneState> lanes;
        int lengthInBars = 2;
        int accentAmount = 0;
    };

    struct PlaylistBlockState
    {
        std::uint32_t clipId = 0;
        std::uint32_t trackId = 0;
        int lane = 0;
        int startCell = 0;
        int lengthCells = 4;
        std::string label;
        std::string clipType;
        int patternNumber = 0;
        bool muted = false;
        bool selected = false;
    };

    struct MixerStripState
    {
        std::uint32_t busId = 0;
        std::string name;
        int insertSlot = 0;
        double volumeDb = -3.0;
        double pan = 0.0;
        int peakLevel = 0;
        bool solo = false;
        bool muted = false;
        int routeTarget = -1;
        double sendAmount = 0.0;
        std::vector<std::string> effects;
    };

    struct ChannelSettingsState
    {
        std::uint32_t trackId = 0;
        std::string name;
        double gain = 0.8;
        double pan = 0.0;
        double pitchSemitones = 0.0;
        double attackMs = 12.0;
        double releaseMs = 180.0;
        double filterCutoffHz = 8400.0;
        double resonance = 0.2;
        int mixerInsert = 1;
        int routeTarget = 0;
        bool reverse = false;
        bool timeStretch = true;
        std::vector<std::string> pluginRack;
        std::vector<std::string> presets;
    };

    struct PlaylistMarkerState
    {
        std::string name;
        int timelineCell = 0;
    };

    struct AutomationLaneState
    {
        std::string target;
        std::vector<int> values;
        std::uint32_t clipId = 0;
        int lane = 0;
        int startCell = 0;
        int lengthCells = 8;
        bool selected = false;
        std::size_t selectedPoint = static_cast<std::size_t>(-1);
    };

    struct WorkspaceModel
    {
        std::vector<BrowserEntry> browserEntries;
        std::vector<PatternState> patterns;
        std::vector<PatternLaneState> patternLanes;
        std::vector<PlaylistBlockState> playlistBlocks;
        std::vector<MixerStripState> mixerStrips;
        std::vector<ChannelSettingsState> channelSettings;
        std::vector<PlaylistMarkerState> markers;
        std::vector<AutomationLaneState> automationLanes;
        int activeChannelIndex = 0;
        int selectedPatternIndex = 0;
        int selectedBrowserIndex = 0;
    };

public:
    UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PluginManagerProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void registerWindowClass();
    void createMainWindow();
    void createMainMenu();
    void createControls();
    void layoutControls();
    void ensurePluginManagerWindow();
    void layoutPluginManagerWindow();
    void refreshPluginManagerState();
    void updatePluginManagerControls();
    void showPluginManagerWindow();
    void closePluginManagerWindow();
    void invalidateSurface(HWND surface);
    void invalidateAllSurfaces();
    void ensureDetachedWindows();
    void updateDockingModel();

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
    void requestCreatePluginStub();
    void requestTogglePluginSandboxMode();

    void handleCommand(WORD commandId);
    void handlePluginManagerCommand(WORD commandId);
    bool handleKeyDown(WPARAM wParam, LPARAM lParam);
    void showAboutDialog() const;
    void selectNextTrack();
    void selectPreviousTrack();
    void selectNextPattern();
    void selectPreviousPattern();
    void cycleSnap(int delta);
    void cycleZoom(bool pianoRoll, int delta);
    void cycleBrowserTab(int delta);
    void cycleEditorTool(bool pianoRoll, int delta);
    void togglePane(WorkspacePane pane);
    std::string currentSnapLabel() const;
    std::string currentBrowserTabLabel() const;
    std::string currentZoomLabel(bool pianoRoll) const;
    std::string currentToolLabel(bool pianoRoll) const;
    void setStaticText(HWND control, const std::string& text) const;
    std::string buildBrowserPanelText() const;
    std::string buildChannelRackPanelText() const;
    std::string buildStepSequencerPanelText() const;
    std::string buildPianoRollPanelText() const;
    std::string buildPlaylistPanelText() const;
    std::string buildMixerPanelText() const;
    std::string buildPluginPanelText() const;
    std::string buildPluginManagerDetailText() const;
    std::string buildPluginManagerPathText() const;
    void applyPluginSearchFilter();
    std::string buildPluginManagerTableText() const;
    SurfaceKind kindFromSurfaceHandle(HWND hwnd) const;
    void paintSurface(HWND hwnd, SurfaceKind kind);
    void paintBrowserSurface(HDC dc, const RECT& rect);
    void paintChannelRackSurface(HDC dc, const RECT& rect);
    void paintStepSequencerSurface(HDC dc, const RECT& rect);
    void paintPianoRollSurface(HDC dc, const RECT& rect);
    void paintPlaylistSurface(HDC dc, const RECT& rect);
    void paintMixerSurface(HDC dc, const RECT& rect);
    void paintPluginSurface(HDC dc, const RECT& rect);
    void handleSurfaceMouseDown(HWND hwnd, SurfaceKind kind, int x, int y);
    void handleSurfaceMouseMove(HWND hwnd, SurfaceKind kind, int x, int y, WPARAM flags);
    void handleSurfaceMouseUp(HWND hwnd, SurfaceKind kind, int x, int y);
    void syncWorkspaceModel();
    void rebuildBrowserEntries();
    void ensurePatternBank();
    void rebuildPatternLanes();
    void rebuildPlaylistBlocks();
    void rebuildMixerStrips();
    void rebuildChannelSettings();
    void rebuildAutomationLanes();
    PatternState makePatternState(int patternNumber) const;
    void ensurePatternLaneNoteContent(PatternLaneState& lane, std::size_t laneIndex);
    void drawSurfaceHeader(HDC dc, const RECT& rect, const std::string& title, const std::string& subtitle) const;
    void drawSurfaceFrame(HDC dc, const RECT& rect, COLORREF borderColor) const;
    std::string browserDropTargetLabel() const;
    void rebuildPlaylistVisuals(const RECT& rect);
    void rebuildPianoVisuals(const RECT& rect);
    int clampValue(int value, int minValue, int maxValue) const;
    bool isPaneVisible(WorkspacePane pane) const;
    void setPaneVisible(WorkspacePane pane, bool visible);
    DockedPaneState* findDockedPane(WorkspacePane pane);
    const DockedPaneState* findDockedPane(WorkspacePane pane) const;
    void detachPane(WorkspacePane pane);
    void attachPane(WorkspacePane pane);
    std::string paneTitle(WorkspacePane pane) const;

private:
    static constexpr const char* kWindowClassName = "DAWCloudTemplateWindowClass";
    static constexpr const char* kPluginManagerClassName = "DAWCloudPluginManagerClass";
    static constexpr const char* kSurfaceClassName = "DAWCloudSurfaceClass";
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
        IdButtonBrowserTabPrev = 1036,
        IdButtonBrowserTabNext = 1037,
        IdButtonPlaylistZoomPrev = 1038,
        IdButtonPlaylistZoomNext = 1039,
        IdButtonPianoZoomPrev = 1040,
        IdButtonPianoZoomNext = 1041,
        IdButtonPlaylistToolPrev = 1042,
        IdButtonPlaylistToolNext = 1043,
        IdButtonPianoToolPrev = 1044,
        IdButtonPianoToolNext = 1045,
        IdButtonManagePlugins = 1046,
        IdButtonPluginRescan = 1047,
        IdButtonPluginAddStub = 1048,
        IdButtonPluginToggleSandbox = 1049,
        IdButtonPluginClose = 1050,

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
        IdLabelSelection = 1216,
        IdLabelBrowserHeader = 1217,
        IdLabelChannelHeader = 1218,
        IdLabelPianoHeader = 1219,
        IdLabelPlaylistHeader = 1220,
        IdLabelMixerHeader = 1221,
        IdLabelPluginHeader = 1222,
        IdLabelContext = 1223,
        IdLabelPluginManagerHeader = 1224,
        IdLabelPluginManagerDetail = 1225,
        IdLabelPluginManagerPaths = 1226
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
        IdMenuOptionsManagePlugins = 2022,
        IdMenuToolsStartEngine = 2023,
        IdMenuToolsStopEngine = 2024,
        IdMenuToolsRebuildGraph = 2025,
        IdMenuToolsRenderOffline = 2026,
        IdMenuHelpAbout = 2027
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
    HWND managePluginsButton_ = nullptr;

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
    HWND browserTabPrevButton_ = nullptr;
    HWND browserTabNextButton_ = nullptr;
    HWND browserHeaderLabel_ = nullptr;
    HWND browserPanel_ = nullptr;
    HWND channelRackMenuButton_ = nullptr;
    HWND channelHeaderLabel_ = nullptr;
    HWND channelRackPanel_ = nullptr;
    HWND stepSequencerPanel_ = nullptr;
    HWND pianoRollMenuButton_ = nullptr;
    HWND pianoZoomPrevButton_ = nullptr;
    HWND pianoZoomNextButton_ = nullptr;
    HWND pianoToolPrevButton_ = nullptr;
    HWND pianoToolNextButton_ = nullptr;
    HWND pianoHeaderLabel_ = nullptr;
    HWND pianoRollPanel_ = nullptr;
    HWND playlistMenuButton_ = nullptr;
    HWND playlistZoomPrevButton_ = nullptr;
    HWND playlistZoomNextButton_ = nullptr;
    HWND playlistToolPrevButton_ = nullptr;
    HWND playlistToolNextButton_ = nullptr;
    HWND playlistHeaderLabel_ = nullptr;
    HWND playlistPanel_ = nullptr;
    HWND mixerMenuButton_ = nullptr;
    HWND mixerHeaderLabel_ = nullptr;
    HWND mixerPanel_ = nullptr;
    HWND pluginMenuButton_ = nullptr;
    HWND pluginHeaderLabel_ = nullptr;
    HWND pluginPanel_ = nullptr;
    HWND contextLabel_ = nullptr;

    HWND pluginManagerHwnd_ = nullptr;
    HWND pluginManagerHeaderLabel_ = nullptr;
    HWND pluginManagerSearchEdit_ = nullptr;
    HWND pluginManagerListBox_ = nullptr;
    HWND pluginManagerDetailLabel_ = nullptr;
    HWND pluginManagerPathsLabel_ = nullptr;
    HWND pluginManagerRescanButton_ = nullptr;
    HWND pluginManagerAddStubButton_ = nullptr;
    HWND pluginManagerToggleSandboxButton_ = nullptr;
    HWND pluginManagerCloseButton_ = nullptr;

    mutable VisibleEngineState visibleState_{};
    PluginManagerState pluginManagerState_{};
    SurfaceInteractionState interactionState_{};
    std::vector<DockedPaneState> dockedPanes_{};
    WorkspaceState workspace_{};
    WorkspaceModel workspaceModel_{};
    std::size_t selectedTrackIndex_ = 0;
};



