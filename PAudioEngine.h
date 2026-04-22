#pragma once

/*
 * PAudioEngine.h
 * --------------
 * Contrato publico y archivo compartible del motor de audio.
 * Si solo necesitas contexto corto para otra IA, empieza por:
 * - PProjectIndex.h
 * - PAudioEngineIndex.h
 * - PAudioEngineIndex.cpp
 *
 * Este archivo expone la API y estructuras publicas del motor.
 * La implementacion profunda vive en AudioEngineDev*.cpp y se agrega desde PAudioEngineIndex.cpp.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class AudioEngine
{
public:
    class ConfigurationException : public std::runtime_error
    {
    public:
        explicit ConfigurationException(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    class AudioBackendException : public std::runtime_error
    {
    public:
        explicit AudioBackendException(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    enum class EngineState
    {
        Uninitialized,
        Initializing,
        Stopped,
        Starting,
        Running,
        Stopping,
        Error
    };

    enum class TransportState
    {
        Stopped,
        Playing,
        Paused
    };

    enum class BackendType
    {
        None,
        Wasapi,
        Dummy
    };

    enum class NodeType
    {
        Input,
        Track,
        Bus,
        Instrument,
        Send,
        Return,
        Plugin,
        Summing,
        Monitor,
        Output,
        RenderSink
    };

    enum class PluginProcessMode
    {
        InProcess,
        GroupSandbox,
        PerPlugin
    };

    enum class ClipSourceType
    {
        GeneratedTone,
        AudioFile
    };

    enum class GeneratorType
    {
        Sampler,
        TestSynth,
        PluginInstrument
    };

    enum class PlaylistItemType
    {
        PatternClip,
        AudioClip,
        AutomationClip
    };

    enum class ParameterSmoothingMode
    {
        Step,
        Linear,
        Exponential
    };

    enum class CommandType
    {
        None,
        NewProject,
        StartEngine,
        StopEngine,
        RecoverAudioDevice,
        PlayTransport,
        PauseTransport,
        StopTransport,
        RebuildGraph,
        RecompileGraph,
        SetTempo,
        SetTimelinePosition,
        SetSamplePosition,
        ConfigureTransportLoop,
        ToggleAnticipativeProcessing,
        ToggleAutomation,
        TogglePdc,
        RenderOffline,
        FreezeTrack,
        AddTrack,
        AddBus,
        AddPattern,
        AddClipToTrack,
        TogglePatternStep,
        UpsertMidiNote,
        SetAutomationPoint,
        MoveClip,
        SaveProject,
        LoadProject,
        UndoEdit,
        RedoEdit
    };

    template <typename T, std::size_t Capacity>
    class SpscRingQueue
    {
    public:
        static_assert(Capacity >= 2, "SpscRingQueue capacity must be at least 2");

        bool push(const T& item) noexcept
        {
            const std::size_t head = head_.load(std::memory_order_relaxed);
            const std::size_t next = increment(head);

            if (next == tail_.load(std::memory_order_acquire))
            {
                return false;
            }

            buffer_[head] = item;
            head_.store(next, std::memory_order_release);
            return true;
        }

        bool pop(T& out) noexcept
        {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);

            if (tail == head_.load(std::memory_order_acquire))
            {
                return false;
            }

            out = buffer_[tail];
            tail_.store(increment(tail), std::memory_order_release);
            return true;
        }

        void clearUnsafe() noexcept
        {
            head_.store(0, std::memory_order_relaxed);
            tail_.store(0, std::memory_order_relaxed);
        }

        std::size_t sizeApprox() const noexcept
        {
            const std::size_t head = head_.load(std::memory_order_acquire);
            const std::size_t tail = tail_.load(std::memory_order_acquire);
            return head >= tail ? (head - tail) : (Capacity - tail + head);
        }

    private:
        static constexpr std::size_t increment(std::size_t index) noexcept
        {
            return (index + 1) % Capacity;
        }

        std::array<T, Capacity> buffer_{};
        std::atomic<std::size_t> head_{0};
        std::atomic<std::size_t> tail_{0};
    };

    struct EngineConfig
    {
        int preferredSampleRate = 48000;
        int preferredBlockSize = 256;
        int inputChannelCount = 2;
        int outputChannelCount = 2;
        int anticipativePrefetchBlocks = 2;
        int helperThreadCount = 2;
        int diskWorkerCount = 1;

        bool enableWasapi = true;
        bool enableCompiledGraph = true;
        bool enableAnticipativeProcessing = true;
        bool enableSampleAccurateAutomation = true;
        bool enablePdc = true;
        bool enableOfflineRender = true;
        bool enablePluginHost = true;
        bool enablePluginSandbox = true;
        bool prefer64BitInternalMix = true;
        bool safeMode = false;

        std::string preferredDeviceName;
        std::string projectName = "Untitled Project";
        std::string sessionPath = "session.dawproject";
    };

    struct EngineMetrics
    {
        std::uint64_t xruns = 0;
        std::uint64_t deadlineMisses = 0;
        std::uint64_t callbackCount = 0;
        std::uint64_t recoveryCount = 0;
        std::uint64_t renderedOfflineFrames = 0;
        std::uint64_t commandQueueDrops = 0;
        double cpuLoadApprox = 0.0;
        double averageBlockTimeUs = 0.0;
        double peakBlockTimeUs = 0.0;
        std::uint32_t currentLatencySamples = 0;
        std::uint64_t activeGraphVersion = 0;
        std::uint32_t cachedClipCount = 0;
    };

    struct TransportInfo
    {
        TransportState state = TransportState::Stopped;
        double tempoBpm = 120.0;
        double timelineSeconds = 0.0;
        std::uint64_t samplePosition = 0;
        bool loopEnabled = false;
        std::uint64_t loopStartSample = 0;
        std::uint64_t loopEndSample = 0;
        bool monitoringEnabled = true;
    };

    struct AudioBuffer
    {
        std::vector<double> left;
        std::vector<double> right;
        std::uint32_t frameCount = 0;

        void resize(std::uint32_t frames)
        {
            frameCount = frames;
            left.assign(frames, 0.0);
            right.assign(frames, 0.0);
        }

        void clear()
        {
            std::fill(left.begin(), left.end(), 0.0);
            std::fill(right.begin(), right.end(), 0.0);
        }
    };

    struct CallbackContext
    {
        std::uint32_t sampleRate = 48000;
        std::uint32_t blockSize = 256;
        std::uint32_t deviceBufferFrames = 0;
        std::uint64_t callbackIndex = 0;
        std::uint64_t transportSampleStart = 0;
        double blockStartTimeSeconds = 0.0;
    };

    struct RenderContext
    {
        CallbackContext callback{};
        bool isOffline = false;
        bool isLivePath = true;
        bool isAnticipativePass = false;
        bool supportsSampleAccurateAutomation = true;
        std::uint32_t futureBlockOffset = 0;
    };

    struct BackendFormat
    {
        std::uint32_t sampleRate = 48000;
        std::uint32_t channelCount = 2;
        std::uint32_t bitsPerSample = 32;
        bool floatingPoint = true;
        bool extensible = false;
    };

    struct DeviceRecoveryState
    {
        bool deviceLost = false;
        bool restartPending = false;
        std::uint32_t restartAttempts = 0;
        std::string lastFailure;
    };

    struct AudioThreadState
    {
        std::atomic<bool> running{false};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> maintenanceRunning{false};
        std::atomic<bool> helpersRunning{false};
        std::uint64_t lastCallbackIndex = 0;
        double lastBlockDurationUs = 0.0;
        bool mmcssRegistered = false;
        bool comInitialized = false;
    };

    struct AudioDeviceState
    {
        BackendType backendType = BackendType::None;
        std::string backendName = "None";
        std::string deviceName = "No Device";
        std::uint32_t sampleRate = 48000;
        std::uint32_t blockSize = 256;
        std::uint32_t deviceBufferFrames = 0;
        std::uint32_t inputChannels = 2;
        std::uint32_t outputChannels = 2;
        bool initialized = false;
        bool started = false;
        bool sharedMode = true;
        bool eventDriven = false;
        bool usingPreferredFormat = false;
        BackendFormat format{};
        DeviceRecoveryState recovery{};
        std::vector<std::uint8_t> mixFormatBlob{};
        std::uintptr_t eventHandle = 0;
        std::uintptr_t enumeratorHandle = 0;
        std::uintptr_t deviceHandle = 0;
        std::uintptr_t audioClientHandle = 0;
        std::uintptr_t renderClientHandle = 0;
        std::uintptr_t captureClientHandle = 0;
    };

    struct GraphNode
    {
        std::uint32_t id = 0;
        NodeType type = NodeType::Track;
        std::string name;
        std::uint32_t latencySamples = 0;
        std::uint32_t trackId = 0;
        std::uint32_t busId = 0;
        std::uint32_t pluginInstanceId = 0;
        double baseGain = 1.0;
        bool liveCritical = false;
        bool canProcessAnticipatively = false;
        bool bypassed = false;
        bool supportsDoublePrecision = true;
    };

    struct GraphEdge
    {
        std::uint32_t sourceNodeId = 0;
        std::uint32_t destinationNodeId = 0;
    };

    struct GraphSnapshot
    {
        std::uint64_t version = 0;
        std::vector<GraphNode> nodes;
        std::vector<GraphEdge> edges;
    };

    struct CompiledNode
    {
        std::uint32_t nodeId = 0;
        NodeType type = NodeType::Track;
        std::uint32_t executionOrder = 0;
        std::uint32_t stageIndex = 0;
        std::uint32_t branchGroup = 0;
        std::uint32_t accumulatedLatencySamples = 0;
        std::uint32_t compensationDelaySamples = 0;
        bool liveLane = true;
        bool anticipativeLane = false;
        bool supportsDoublePrecision = true;
        std::vector<std::uint32_t> upstreamNodeIds;
        std::vector<std::uint32_t> downstreamNodeIds;
    };

    struct ExecutionStage
    {
        std::uint32_t stageIndex = 0;
        bool liveLane = true;
        bool anticipativeLane = false;
        std::vector<std::uint32_t> nodeIds;
    };

    struct CompiledSubgraph
    {
        std::uint32_t rootNodeId = 0;
        bool containsLiveNode = false;
        bool containsBus = false;
        std::vector<std::uint32_t> nodeIds;
    };

    struct CompiledGraph
    {
        std::uint64_t sourceVersion = 0;
        std::vector<CompiledNode> executionPlan;
        std::vector<ExecutionStage> stages;
        std::vector<CompiledSubgraph> subgraphs;
        std::unordered_map<std::uint32_t, std::size_t> nodeLookup;
        std::vector<std::uint32_t> topologicalOrder;
        bool valid = false;
        bool hasCycle = false;
    };

    struct AutomationEvent
    {
        std::uint32_t nodeId = 0;
        std::uint32_t parameterId = 0;
        std::uint32_t sampleOffset = 0;
        float value = 0.0f;
    };

    struct ParameterBinding
    {
        std::uint32_t parameterId = 0;
        std::string name;
        bool bypassBinding = false;
    };

    struct ParameterSmoother
    {
        double currentValue = 1.0;
        double targetValue = 1.0;
        double smoothingFactor = 0.1;
        ParameterSmoothingMode mode = ParameterSmoothingMode::Linear;
        bool enabled = true;
    };

    struct ParameterState
    {
        std::uint32_t parameterId = 0;
        double currentValue = 1.0;
        double targetValue = 1.0;
        ParameterBinding binding{};
        ParameterSmoother smoother{};
    };

    struct AutomationSegment
    {
        std::uint32_t nodeId = 0;
        std::uint32_t parameterId = 0;
        std::uint32_t startSample = 0;
        std::uint32_t endSample = 0;
        double startValue = 1.0;
        double endValue = 1.0;
        ParameterSmoothingMode mode = ParameterSmoothingMode::Linear;
    };

    struct AutomationLane
    {
        std::uint32_t nodeId = 0;
        bool sampleAccurateEnabled = true;
        std::vector<AutomationEvent> pendingEvents;
        std::vector<AutomationSegment> activeSegments;
        std::vector<ParameterState> parameterStates;
    };

    struct DelayCompensationState
    {
        std::uint32_t nodeId = 0;
        std::uint32_t localLatencySamples = 0;
        std::uint32_t upstreamLatencySamples = 0;
        std::uint32_t accumulatedLatencySamples = 0;
        std::uint32_t compensationDelaySamples = 0;
    };

    struct DelayLine
    {
        std::vector<double> left;
        std::vector<double> right;
        std::uint32_t writeIndex = 0;
        std::uint32_t delaySamples = 0;
    };

    struct PluginDescriptor
    {
        std::string name;
        std::string vendor;
        std::string format;
        PluginProcessMode processMode = PluginProcessMode::InProcess;
        bool supportsDoublePrecision = false;
        bool supportsSampleAccurateAutomation = false;
        std::uint32_t reportedLatencySamples = 0;
    };

    struct PluginSandboxState
    {
        bool enabled = false;
        bool alive = false;
        bool crashRecovered = false;
        std::uint32_t watchdogMisses = 0;
        std::int32_t lastHeartbeat = 0;
        std::string executablePath;
        std::string sharedMemoryName;
        std::uintptr_t processHandle = 0;
        std::uintptr_t threadHandle = 0;
        std::uintptr_t mappingHandle = 0;
        std::chrono::steady_clock::time_point lastHeartbeatTime{};
    };

    struct PluginInstance
    {
        std::uint32_t instanceId = 0;
        PluginDescriptor descriptor{};
        std::uint32_t ownerNodeId = 0;
        bool active = false;
        bool sandboxed = false;
        bool autoBypassed = false;
        PluginSandboxState sandbox{};
    };

    struct DiskReadRequest
    {
        std::uint32_t clipId = 0;
        std::string filePath;
        std::uint64_t fileOffsetFrames = 0;
        std::uint32_t frameCount = 0;
    };

    struct ClipCacheEntry
    {
        std::uint32_t clipId = 0;
        std::string filePath;
        std::uint64_t cachedOffsetFrames = 0;
        std::uint32_t sampleRate = 48000;
        std::vector<double> left;
        std::vector<double> right;
        bool complete = false;
        std::chrono::steady_clock::time_point lastAccess{};
    };

    struct OfflineRenderRequest
    {
        double startTimeSeconds = 0.0;
        double endTimeSeconds = 0.0;
        std::uint32_t sampleRate = 48000;
        std::string outputPath;
        bool highQuality = true;
        bool renderStems = false;
        std::uint32_t targetTrackId = 0;
    };

    struct ClipState
    {
        std::uint32_t clipId = 0;
        std::uint32_t trackId = 0;
        std::string name;
        ClipSourceType sourceType = ClipSourceType::GeneratedTone;
        std::string filePath;
        int patternNumber = 0;
        double startTimeSeconds = 0.0;
        double durationSeconds = 1.0;
        double trimStartSeconds = 0.0;
        double trimEndSeconds = 0.0;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
        double gain = 1.0;
        double pan = 0.0;
        double pitchSemitones = 0.0;
        double stretchRatio = 1.0;
        bool loopEnabled = false;
        bool timeStretchEnabled = false;
        bool muted = false;
    };

    struct PlaylistItemState
    {
        std::uint32_t itemId = 0;
        std::uint32_t trackId = 0;
        PlaylistItemType type = PlaylistItemType::PatternClip;
        std::uint32_t sourceClipId = 0;
        std::uint32_t automationClipId = 0;
        int patternNumber = 0;
        std::string label;
        double startTimeSeconds = 0.0;
        double durationSeconds = 1.0;
        double trimStartSeconds = 0.0;
        double trimEndSeconds = 0.0;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
        double gain = 1.0;
        double pan = 0.0;
        double pitchSemitones = 0.0;
        double stretchRatio = 1.0;
        bool muted = false;
        bool loopEnabled = false;
        bool timeStretchEnabled = false;
    };

    struct TrackState
    {
        std::uint32_t trackId = 0;
        std::uint32_t busId = 0;
        std::string name;
        bool armed = false;
        bool muted = false;
        bool solo = false;
        bool recordEnabled = false;
        double gain = 1.0;
        double pan = 0.0;
        std::uint32_t routeTargetBusId = 0;
        std::uint32_t sendBusId = 0;
        double sendAmount = 0.0;
        std::vector<std::uint32_t> clipIds;
        std::vector<std::uint32_t> insertPluginIds;
    };

    struct BusState
    {
        std::uint32_t busId = 0;
        std::string name;
        double gain = 1.0;
        double pan = 0.0;
        bool muted = false;
        bool solo = false;
        std::uint32_t outputBusId = 0;
        std::vector<std::uint32_t> inputTrackIds;
    };

    struct MarkerState
    {
        std::uint32_t markerId = 0;
        std::string name;
        double timeSeconds = 0.0;
    };

    struct StepState
    {
        bool enabled = false;
        int velocity = 100;
    };

    struct MidiNoteState
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
        std::vector<StepState> steps;
        std::vector<MidiNoteState> notes;
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

    struct ChannelSettingsState
    {
        std::uint32_t trackId = 0;
        std::string name;
        GeneratorType generatorType = GeneratorType::Sampler;
        std::string sampleFilePath;
        std::string instrumentPluginName;
        double gain = 0.8;
        double pan = 0.0;
        double pitchSemitones = 0.0;
        double attackMs = 12.0;
        double decayMs = 70.0;
        double sustainLevel = 0.78;
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

    struct AutomationPointState
    {
        int cell = 0;
        int value = 0;
        double curve = 0.0;
    };

    struct AutomationClipState
    {
        std::uint32_t clipId = 0;
        std::string target;
        std::vector<AutomationPointState> points;
        std::uint32_t trackId = 0;
        int lane = 0;
        int startCell = 0;
        int lengthCells = 8;
    };

    struct ProjectState
    {
        std::string projectName = "Untitled Project";
        std::string sessionPath = "session.dawproject";
        std::uint64_t revision = 0;
        bool dirty = false;
        std::vector<TrackState> tracks;
        std::vector<ClipState> clips;
        std::vector<PlaylistItemState> playlistItems;
        std::vector<BusState> buses;
        std::vector<MarkerState> markers;
        std::vector<PatternState> patterns;
        std::vector<ChannelSettingsState> channelSettings;
        std::vector<AutomationClipState> automationClips;
        bool autosaveEnabled = true;
        bool recoveryAvailable = false;
        std::string autosavePath;
        std::string recoveryPath;
        std::uint64_t lastSavedRevision = 0;
        std::uint64_t lastAutosavedRevision = 0;
    };

    struct ProjectAction
    {
        std::string description;
        ProjectState before{};
        ProjectState after{};
    };

    struct ProjectSnapshot
    {
        ProjectState state{};
        std::uint32_t undoDepth = 0;
        std::uint32_t redoDepth = 0;
        std::uint64_t graphVersion = 0;
    };

    struct SchedulerJob
    {
        std::uint64_t blockIndex = 0;
        std::uint64_t graphVersion = 0;
        std::uint32_t prefetchDistance = 0;
        std::vector<std::uint32_t> stageIds;
    };

    struct AnticipativeBlockResult
    {
        std::uint64_t blockIndex = 0;
        std::uint64_t graphVersion = 0;
        bool valid = false;
        AudioBuffer buffer{};
    };

    struct EngineCommand
    {
        CommandType type = CommandType::None;
        double doubleValue = 0.0;
        std::uint64_t uintValue = 0;
        std::uint64_t secondaryUintValue = 0;
        bool boolValue = false;
        std::string textValue;
        std::string secondaryTextValue;
    };

    struct EngineSnapshot
    {
        EngineState engineState = EngineState::Uninitialized;
        EngineConfig config{};
        EngineMetrics metrics{};
        TransportInfo transport{};
        AudioDeviceState device{};
        ProjectSnapshot project{};
        std::string statusText;
        std::string lastErrorMessage;
        int lastErrorCode = 0;
    };

public:
    AudioEngine();
    ~AudioEngine();

    bool initialize(const EngineConfig& config);
    void shutdown();

    bool initializeAudioBackend();
    bool recoverAudioDevice();
    bool start();
    bool stop();

    EngineState getState() const noexcept;
    bool isRunning() const noexcept;
    bool isInitialized() const noexcept;

    std::string getStatusText() const;
    std::string getLastErrorMessage() const;
    int getLastErrorCode() const noexcept;

    const EngineConfig& getConfig() const noexcept;
    EngineMetrics getMetrics() const noexcept;
    EngineSnapshot getUiSnapshot() const;
    ProjectSnapshot getProjectSnapshot() const;

    std::string getBackendName() const;
    std::string getCurrentDeviceName() const;
    int getCurrentSampleRate() const noexcept;
    int getCurrentBlockSize() const noexcept;

    bool initializeTransport();
    void play();
    void pause();
    void stopTransport();

    void setTimelinePosition(double seconds);
    void setTempo(double bpm);
    void setSamplePosition(std::uint64_t samplePosition);
    void configureTransportLoop(std::uint64_t startSample, std::uint64_t endSample, bool enabled);

    bool isPlaying() const noexcept;
    bool isMonitoring() const noexcept;
    TransportInfo getTransportInfo() const noexcept;

    bool buildInitialGraph();
    bool compileGraph();
    void requestGraphRebuild();
    bool swapCompiledGraphAtSafePoint();
    std::uint64_t getActiveGraphVersion() const noexcept;

    void processLiveBlock(RenderContext& renderContext, AudioBuffer& ioBuffer);
    void processAnticipativeWork(RenderContext& renderContext);
    void mergeLiveAndAnticipativeResults(RenderContext& renderContext, AudioBuffer& ioBuffer);

    void enqueueAutomationEvent(const AutomationEvent& event);
    void applyAutomationForBlock(RenderContext& renderContext);
    void flushAutomationToNode(std::uint32_t nodeId);

    void recalculateLatencyModel();
    void alignBranchesForMix(AudioBuffer& ioBuffer);

    bool addPluginStub(const PluginDescriptor& descriptor, std::uint32_t ownerNodeId);
    std::vector<PluginDescriptor> getLoadedPluginDescriptors() const;

    bool renderOffline(const OfflineRenderRequest& request);
    bool freezeTrack(std::uint32_t trackNodeId);

    bool addTrack(const std::string& name);
    bool addBus(const std::string& name);
    bool addPattern(const std::string& name);
    bool addClipToTrack(std::uint32_t trackId, const std::string& clipName);
    bool togglePatternStep(int patternNumber, std::uint32_t trackId, int stepIndex);
    bool upsertMidiNote(int patternNumber, std::uint32_t trackId, std::size_t noteIndex, const MidiNoteState& note, bool createIfMissing);
    bool setAutomationPoint(std::uint32_t clipId, int pointIndex, int cell, int value);
    bool moveClip(std::uint32_t clipId, std::uint32_t targetTrackId, double startTimeSeconds, double durationSeconds = -1.0);
    bool newProject(const std::string& name);
    bool saveProject(const std::string& path);
    bool loadProject(const std::string& path);
    bool undoLastEdit();
    bool redoLastEdit();

    bool startTelemetry();
    void stopTelemetry();

    void postCommand(const EngineCommand& command);
    bool consumeNextCommand(EngineCommand& outCommand);

private:
    void setError(int errorCode, const std::string& message);
    void clearError();

    void resetMetrics();
    void updateStatusFromState();
    void publishSnapshot();
    void processPendingCommands();

    bool initializeProjectState();
    void synchronizeProjectMusicalState();
    bool writeProjectFile(const std::string& path, bool updateSessionPath, bool clearDirtyState);
    void maybeAutosaveProject();
    GraphSnapshot buildGraphFromProjectState() const;
    std::optional<GraphNode> findGraphNode(std::uint32_t nodeId, const GraphSnapshot& graph) const;
    const CompiledNode* findCompiledNode(std::uint32_t nodeId) const;
    const AutomationLane* findAutomationLane(std::uint32_t nodeId) const;
    AutomationLane* findAutomationLane(std::uint32_t nodeId);
    double getNodeAutomationValue(std::uint32_t nodeId, std::uint32_t sampleOffset) const;

    bool openWasapiBackend();
    void closeAudioBackend();
    bool startAudioStream();
    void stopAudioStream();
    bool waitForNextAudioBuffer(std::uint32_t& outFramesToRender);
    bool writeBufferToBackend(const AudioBuffer& buffer);
    void audioThreadMain();
    void maintenanceThreadMain();

    void scheduleAnticipativeBlock(std::uint64_t liveBlockIndex);
    void computeAnticipativeBlock(std::uint64_t targetBlockIndex);
    bool consumeAnticipativeBlock(std::uint64_t blockIndex, AudioBuffer& outBuffer);

    bool renderGraphBlock(RenderContext& renderContext, AudioBuffer& outputBuffer, bool includeAnticipativeNodes);
    void renderTrackClips(std::uint32_t trackId, const RenderContext& renderContext, AudioBuffer& destination);
    void applyNodeDelayCompensation(std::uint32_t nodeId, AudioBuffer& buffer);
    void mixBuffer(AudioBuffer& destination, const AudioBuffer& source, double gain = 1.0);
    void mixTrackToBus(AudioBuffer& destination, std::uint32_t trackId, const RenderContext& renderContext);

    void queueClipReadAhead(const RenderContext& renderContext);
    void diskWorkerMain();
    bool loadClipIntoCache(const DiskReadRequest& request);
    bool writeWavFile(const std::string& path, std::uint32_t sampleRate, const AudioBuffer& buffer);

    bool spawnPluginSandbox(PluginInstance& instance);
    void shutdownPluginSandbox(PluginInstance& instance);
    void serviceSandboxWatchdogs();
    void serviceClipCacheEviction();

    PatternState* findPatternState(int patternNumber);
    const PatternState* findPatternState(int patternNumber) const;
    PatternLaneState* findPatternLane(PatternState& pattern, std::uint32_t trackId);
    const PatternLaneState* findPatternLane(const PatternState& pattern, std::uint32_t trackId) const;
    ClipState* findClipState(std::uint32_t clipId);
    const ClipState* findClipState(std::uint32_t clipId) const;
    PlaylistItemState* findPlaylistItem(std::uint32_t itemId);
    const PlaylistItemState* findPlaylistItem(std::uint32_t itemId) const;
    BusState* findBusState(std::uint32_t busId);
    const BusState* findBusState(std::uint32_t busId) const;
    ChannelSettingsState* findChannelSettings(std::uint32_t trackId);
    const ChannelSettingsState* findChannelSettings(std::uint32_t trackId) const;
    AutomationClipState* findAutomationClip(std::uint32_t clipId);
    double evaluateAutomationTarget(const std::string& target, double timelineSeconds, double defaultValue) const;
    bool hasSoloedTracks() const;
    bool hasSoloedBuses() const;
    PatternLaneState makeDefaultPatternLane(std::uint32_t trackId, int patternNumber, std::size_t laneIndex) const;
    PatternState makeDefaultPatternState(int patternNumber) const;
    ChannelSettingsState makeDefaultChannelSettings(std::uint32_t trackId, std::size_t laneIndex) const;

    void pushUndoState(const std::string& description, const ProjectState& beforeState);
    std::uint32_t nextTrackId() const;
    std::uint32_t nextBusId() const;
    std::uint32_t nextClipId() const;
    std::uint32_t nextPlaylistItemId() const;
    std::uint32_t nextPluginInstanceId() const;
    std::uint32_t nextAutomationClipId() const;

private:
    EngineConfig config_{};
    std::atomic<EngineState> state_{EngineState::Uninitialized};

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> telemetryRunning_{false};

    int lastErrorCode_ = 0;
    std::string lastErrorMessage_;

    AudioDeviceState deviceState_{};
    AudioThreadState audioThreadState_{};
    AudioBuffer realtimeBuffer_{};
    AudioBuffer anticipativeBuffer_{};
    bool backendComInitialized_ = false;
    std::atomic<bool> diskWorkerRunning_{false};

    TransportInfo transportInfo_{};

    GraphSnapshot editableGraph_{};
    GraphSnapshot pendingGraph_{};
    CompiledGraph compiledGraph_{};
    bool graphSwapPending_ = false;

    std::vector<AutomationLane> automationLanes_{};

    std::vector<DelayCompensationState> latencyStates_{};
    std::unordered_map<std::uint32_t, DelayLine> delayLines_{};

    std::vector<PluginInstance> loadedPlugins_{};

    std::vector<std::thread> helperThreads_{};
    std::thread audioThread_{};
    std::thread maintenanceThread_{};
    std::thread diskThread_{};

    std::mutex anticipativeMutex_;
    std::condition_variable anticipativeCv_;
    std::uint64_t requestedAnticipativeBlock_ = 0;
    std::uint64_t completedAnticipativeBlock_ = 0;
    bool anticipativeWorkPending_ = false;
    AnticipativeBlockResult anticipativeResult_{};

    std::mutex diskQueueMutex_;
    std::condition_variable diskQueueCv_;
    std::deque<DiskReadRequest> diskReadQueue_{};
    std::vector<ClipCacheEntry> clipCache_{};
    std::optional<OfflineRenderRequest> lastOfflineRenderRequest_{};

    ProjectState projectState_{};
    std::vector<ProjectAction> undoStack_{};
    std::vector<ProjectAction> redoStack_{};

    EngineMetrics metrics_{};

    SpscRingQueue<EngineCommand, 1024> commandQueue_{};

    std::array<EngineSnapshot, 2> uiSnapshots_{};
    std::atomic<int> activeSnapshotIndex_{0};
    mutable std::mutex snapshotWriteMutex_;
    mutable std::mutex clipCacheMutex_;
    mutable std::mutex pluginMutex_;
};


