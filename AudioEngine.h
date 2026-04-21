#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class AudioEngine
{
public:
    // =========================================================
    // Exceptions
    // =========================================================

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

    // =========================================================
    // Public enums and state models
    // =========================================================

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

    enum class CommandType
    {
        None,
        StartEngine,
        StopEngine,
        PlayTransport,
        PauseTransport,
        StopTransport,
        RebuildGraph,
        RecompileGraph,
        SetTempo,
        SetTimelinePosition,
        SetSamplePosition,
        ToggleAnticipativeProcessing,
        ToggleAutomation,
        TogglePdc,
        RenderOffline,
        FreezeTrack
    };

    // =========================================================
    // Configuration and metrics
    // =========================================================

    struct EngineConfig
    {
        int preferredSampleRate = 48000;
        int preferredBlockSize = 256;
        int inputChannelCount = 2;
        int outputChannelCount = 2;

        bool enableWasapi = true;
        bool enableCompiledGraph = true;
        bool enableAnticipativeProcessing = true;
        bool enableSampleAccurateAutomation = true;
        bool enablePdc = true;
        bool enableOfflineRender = true;
        bool enablePluginHost = false;
        bool enablePluginSandbox = false;
        bool prefer64BitInternalMix = true;

        std::string preferredDeviceName;
        std::string projectName = "Untitled Project";
    };

    struct EngineMetrics
    {
        std::uint64_t xruns = 0;
        std::uint64_t deadlineMisses = 0;
        double cpuLoadApprox = 0.0;
        double peakBlockTimeUs = 0.0;
        std::uint32_t currentLatencySamples = 0;
        std::uint64_t activeGraphVersion = 0;
    };

    struct TransportInfo
    {
        TransportState state = TransportState::Stopped;
        double tempoBpm = 120.0;
        double timelineSeconds = 0.0;
        std::uint64_t samplePosition = 0;
        bool monitoringEnabled = false;
    };

    // =========================================================
    // Audio/backend models
    // =========================================================

    struct AudioBuffer
    {
        std::vector<float> left;
        std::vector<float> right;
        std::uint32_t frameCount = 0;

        void resize(std::uint32_t frames)
        {
            frameCount = frames;
            left.resize(frames, 0.0f);
            right.resize(frames, 0.0f);
        }

        void clear()
        {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
        }
    };

    struct CallbackContext
    {
        std::uint32_t sampleRate = 48000;
        std::uint32_t blockSize = 256;
        std::uint64_t callbackIndex = 0;
        double blockStartTimeSeconds = 0.0;
    };

    struct RenderContext
    {
        CallbackContext callback;
        bool isOffline = false;
        bool isLivePath = true;
        bool supportsSampleAccurateAutomation = true;
    };

    struct AudioDeviceState
    {
        BackendType backendType = BackendType::None;
        std::string backendName = "None";
        std::string deviceName = "No Device";
        std::uint32_t sampleRate = 48000;
        std::uint32_t blockSize = 256;
        std::uint32_t inputChannels = 2;
        std::uint32_t outputChannels = 2;
        bool initialized = false;
        bool started = false;
    };

    // =========================================================
    // Graph models
    // =========================================================

    struct GraphNode
    {
        std::uint32_t id = 0;
        NodeType type = NodeType::Track;
        std::string name;
        std::uint32_t latencySamples = 0;
        bool liveCritical = false;
        bool canProcessAnticipatively = false;
        bool bypassed = false;
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
        bool liveLane = true;
        bool anticipativeLane = false;
        std::uint32_t accumulatedLatencySamples = 0;
    };

    struct CompiledGraph
    {
        std::uint64_t sourceVersion = 0;
        std::vector<CompiledNode> executionPlan;
        bool valid = false;
    };

    // =========================================================
    // Automation models
    // =========================================================

    struct AutomationEvent
    {
        std::uint32_t nodeId = 0;
        std::uint32_t parameterId = 0;
        std::uint32_t sampleOffset = 0;
        float value = 0.0f;
    };

    struct ParameterSmoother
    {
        float currentValue = 0.0f;
        float targetValue = 0.0f;
        float smoothingFactor = 0.1f;
        bool enabled = true;
    };

    struct ParameterState
    {
        std::uint32_t parameterId = 0;
        float currentValue = 0.0f;
        float targetValue = 0.0f;
        ParameterSmoother smoother{};
    };

    struct AutomationLane
    {
        std::uint32_t nodeId = 0;
        bool sampleAccurateEnabled = true;
        std::vector<AutomationEvent> pendingEvents;
        std::vector<ParameterState> parameterStates;
    };

    // =========================================================
    // PDC / latency models
    // =========================================================

    struct DelayCompensationState
    {
        std::uint32_t nodeId = 0;
        std::uint32_t localLatencySamples = 0;
        std::uint32_t accumulatedLatencySamples = 0;
    };

    struct DelayLine
    {
        std::vector<float> buffer;
        std::uint32_t writeIndex = 0;
        std::uint32_t delaySamples = 0;
    };

    // =========================================================
    // Plugin models
    // =========================================================

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

    struct PluginInstance
    {
        std::uint32_t instanceId = 0;
        PluginDescriptor descriptor{};
        std::uint32_t ownerNodeId = 0;
        bool active = false;
        bool sandboxed = false;
    };

    // =========================================================
    // Disk / offline render models
    // =========================================================

    struct DiskReadRequest
    {
        std::uint32_t clipId = 0;
        std::uint64_t fileOffsetFrames = 0;
        std::uint32_t frameCount = 0;
    };

    struct ClipCacheEntry
    {
        std::uint32_t clipId = 0;
        std::uint64_t cachedOffsetFrames = 0;
        std::vector<float> left;
        std::vector<float> right;
    };

    struct OfflineRenderRequest
    {
        double startTimeSeconds = 0.0;
        double endTimeSeconds = 0.0;
        std::uint32_t sampleRate = 48000;
        std::string outputPath;
        bool highQuality = true;
    };

    // =========================================================
    // Command queue models
    // =========================================================

    struct EngineCommand
    {
        CommandType type = CommandType::None;
        double doubleValue = 0.0;
        std::uint64_t uintValue = 0;
        std::string textValue;
    };

public:
    // =========================================================
    // Lifecycle
    // =========================================================

    AudioEngine();
    ~AudioEngine();

    bool initialize(const EngineConfig& config);
    void shutdown();

    bool initializeAudioBackend();
    bool start();
    bool stop();

    // =========================================================
    // Engine state and diagnostics
    // =========================================================

    EngineState getState() const noexcept;
    bool isRunning() const noexcept;
    bool isInitialized() const noexcept;

    std::string getStatusText() const;
    std::string getLastErrorMessage() const;
    int getLastErrorCode() const noexcept;

    const EngineConfig& getConfig() const noexcept;
    EngineMetrics getMetrics() const noexcept;

    std::string getBackendName() const;
    std::string getCurrentDeviceName() const;
    int getCurrentSampleRate() const noexcept;
    int getCurrentBlockSize() const noexcept;

    // =========================================================
    // Transport
    // =========================================================

    bool initializeTransport();
    void play();
    void pause();
    void stopTransport();

    void setTimelinePosition(double seconds);
    void setTempo(double bpm);
    void setSamplePosition(std::uint64_t samplePosition);

    bool isPlaying() const noexcept;
    bool isMonitoring() const noexcept;
    TransportInfo getTransportInfo() const noexcept;

    // =========================================================
    // Graph management
    // =========================================================

    bool buildInitialGraph();
    bool compileGraph();
    void requestGraphRebuild();
    bool swapCompiledGraphAtSafePoint();

    std::uint64_t getActiveGraphVersion() const noexcept;

    // =========================================================
    // Scheduler high-level entry points
    // =========================================================

    void processLiveBlock(RenderContext& renderContext, AudioBuffer& ioBuffer);
    void processAnticipativeWork(RenderContext& renderContext);
    void mergeLiveAndAnticipativeResults(RenderContext& renderContext, AudioBuffer& ioBuffer);

    // =========================================================
    // Automation
    // =========================================================

    void enqueueAutomationEvent(const AutomationEvent& event);
    void applyAutomationForBlock(RenderContext& renderContext);
    void flushAutomationToNode(std::uint32_t nodeId);

    // =========================================================
    // PDC
    // =========================================================

    void recalculateLatencyModel();
    void alignBranchesForMix(AudioBuffer& ioBuffer);

    // =========================================================
    // Plugin hosting
    // =========================================================

    bool addPluginStub(const PluginDescriptor& descriptor, std::uint32_t ownerNodeId);
    std::vector<PluginDescriptor> getLoadedPluginDescriptors() const;

    // =========================================================
    // Offline render / disk I/O stubs
    // =========================================================

    bool renderOffline(const OfflineRenderRequest& request);
    bool freezeTrack(std::uint32_t trackNodeId);

    // =========================================================
    // Telemetry and polling
    // =========================================================

    bool startTelemetry();
    void stopTelemetry();

    // =========================================================
    // Command queue
    // =========================================================

    void postCommand(const EngineCommand& command);
    bool consumeNextCommand(EngineCommand& outCommand);

private:
    // =========================================================
    // Internal helpers
    // =========================================================

    void setError(int errorCode, const std::string& message);
    void clearError();

    void resetMetrics();
    void updateStatusFromState();
    void processPendingCommands();

private:
    // =========================================================
    // Core engine state
    // =========================================================

    EngineConfig config_{};
    std::atomic<EngineState> state_{EngineState::Uninitialized};

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> telemetryRunning_{false};

    int lastErrorCode_ = 0;
    std::string lastErrorMessage_;

    // =========================================================
    // Audio/backend state
    // =========================================================

    AudioDeviceState deviceState_{};
    AudioBuffer realtimeBuffer_{};
    AudioBuffer anticipativeBuffer_{};

    // =========================================================
    // Transport state
    // =========================================================

    TransportInfo transportInfo_{};

    // =========================================================
    // Graph / compiled graph state
    // =========================================================

    GraphSnapshot editableGraph_{};
    GraphSnapshot pendingGraph_{};
    CompiledGraph compiledGraph_{};
    bool graphSwapPending_ = false;

    // =========================================================
    // Automation / parameters
    // =========================================================

    std::vector<AutomationLane> automationLanes_{};

    // =========================================================
    // Latency / PDC
    // =========================================================

    std::vector<DelayCompensationState> latencyStates_{};
    std::vector<DelayLine> delayLines_{};

    // =========================================================
    // Plugin host stub state
    // =========================================================

    std::vector<PluginInstance> loadedPlugins_{};

    // =========================================================
    // Disk / offline render stub state
    // =========================================================

    std::deque<DiskReadRequest> diskReadQueue_{};
    std::vector<ClipCacheEntry> clipCache_{};
    std::optional<OfflineRenderRequest> lastOfflineRenderRequest_{};

    // =========================================================
    // Metrics
    // =========================================================

    EngineMetrics metrics_{};

    // =========================================================
    // Command queue
    // =========================================================

    std::deque<EngineCommand> commandQueue_{};
    mutable std::mutex commandQueueMutex_;
};
