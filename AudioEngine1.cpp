#include "AudioEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <windows.h>
#include <mmreg.h>

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <propsys.h>
#include <Functiondiscoverykeys_devpkey.h>
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int kErrorInvalidConfig = 1001;
    constexpr int kErrorBackendInit = 1002;
    constexpr int kErrorGraphBuild = 1003;
    constexpr int kErrorGraphCompile = 1004;
    constexpr int kErrorTransportInit = 1005;
    constexpr int kErrorOfflineRender = 1006;
    constexpr int kErrorProjectIo = 1007;
    constexpr int kErrorAudioRuntime = 1008;

    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr std::uint32_t kInputNodeId = 1;
    constexpr std::uint32_t kOutputNodeId = 2;
    constexpr std::uint32_t kTrackNodeBase = 1000;
    constexpr std::uint32_t kPluginNodeBase = 1001;
    constexpr std::uint32_t kBusNodeBase = 100000;
    constexpr std::uint32_t kDefaultAutomationParameter = 0;
    constexpr DWORD kBackendWaitTimeoutMs = 2000;
    constexpr DWORD kMaintenanceTickMs = 250;
    constexpr std::uint32_t kSandboxWatchdogThreshold = 6;

    struct SandboxSharedState
    {
        volatile LONG heartbeat = 0;
        volatile LONG stopRequested = 0;
        volatile LONG lastError = 0;
        volatile LONG reserved = 0;
    };

    struct DecodedWavData
    {
        std::uint32_t sampleRate = 48000;
        std::vector<double> left;
        std::vector<double> right;
    };

    template <typename T>
    void safeRelease(T*& value)
    {
        if (value != nullptr)
        {
            value->Release();
            value = nullptr;
        }
    }

    void safeCloseHandle(std::uintptr_t& handleValue)
    {
        if (handleValue != 0)
        {
            CloseHandle(reinterpret_cast<HANDLE>(handleValue));
            handleValue = 0;
        }
    }

    std::string toLowerCopy(const std::string& text)
    {
        std::string result = text;
        std::transform(
            result.begin(),
            result.end(),
            result.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return result;
    }

    std::string quoteArgument(const std::string& text)
    {
        if (text.find(' ') == std::string::npos && text.find('"') == std::string::npos)
        {
            return text;
        }

        std::string quoted = "\"";
        for (const char character : text)
        {
            if (character == '"')
            {
                quoted += "\\\"";
            }
            else
            {
                quoted += character;
            }
        }
        quoted += "\"";
        return quoted;
    }

    std::vector<std::string> splitString(const std::string& text, char delimiter)
    {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string item;

        while (std::getline(stream, item, delimiter))
        {
            parts.push_back(item);
        }

        return parts;
    }

    double clampSample(double value)
    {
        return std::clamp(value, -1.0, 1.0);
    }

    const char* toString(AudioEngine::EngineState state)
    {
        switch (state)
        {
        case AudioEngine::EngineState::Uninitialized: return "Uninitialized";
        case AudioEngine::EngineState::Initializing: return "Initializing";
        case AudioEngine::EngineState::Stopped: return "Stopped";
        case AudioEngine::EngineState::Starting: return "Starting";
        case AudioEngine::EngineState::Running: return "Running";
        case AudioEngine::EngineState::Stopping: return "Stopping";
        case AudioEngine::EngineState::Error: return "Error";
        default: return "Unknown";
        }
    }

    const char* backendName(AudioEngine::BackendType type)
    {
        switch (type)
        {
        case AudioEngine::BackendType::Wasapi: return "WASAPI";
        case AudioEngine::BackendType::Dummy: return "Dummy";
        default: return "None";
        }
    }

    double interpolateValue(double startValue, double endValue, double t, AudioEngine::ParameterSmoothingMode mode)
    {
        const double clampedT = std::clamp(t, 0.0, 1.0);

        switch (mode)
        {
        case AudioEngine::ParameterSmoothingMode::Step:
            return clampedT < 1.0 ? startValue : endValue;

        case AudioEngine::ParameterSmoothingMode::Exponential:
        {
            const double safeStart = std::max(0.0001, startValue);
            const double safeEnd = std::max(0.0001, endValue);
            return safeStart * std::pow(safeEnd / safeStart, clampedT);
        }

        case AudioEngine::ParameterSmoothingMode::Linear:
        default:
            return startValue + ((endValue - startValue) * clampedT);
        }
    }

    std::uint32_t trackNodeId(std::uint32_t trackId)
    {
        return kTrackNodeBase + (trackId * 10);
    }

    std::uint32_t pluginNodeId(std::uint32_t trackId)
    {
        return kPluginNodeBase + (trackId * 10);
    }

    std::uint32_t busNodeId(std::uint32_t busId)
    {
        return kBusNodeBase + (busId * 10);
    }

    bool readChunkHeader(std::ifstream& stream, char (&chunkId)[4], std::uint32_t& chunkSize)
    {
        stream.read(chunkId, 4);
        stream.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
        return stream.good();
    }

    bool decodeWavFile(const std::string& path, DecodedWavData& output)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
        {
            return false;
        }

        char riff[4]{};
        char wave[4]{};
        std::uint32_t riffSize = 0;

        stream.read(riff, 4);
        stream.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize));
        stream.read(wave, 4);

        if (std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0)
        {
            return false;
        }

        std::uint16_t formatTag = 0;
        std::uint16_t channelCount = 0;
        std::uint32_t sampleRate = 0;
        std::uint16_t bitsPerSample = 0;
        std::vector<std::uint8_t> pcmData;

        while (stream.good() && !stream.eof())
        {
            char chunkId[4]{};
            std::uint32_t chunkSize = 0;

            if (!readChunkHeader(stream, chunkId, chunkSize))
            {
                break;
            }

            if (std::strncmp(chunkId, "fmt ", 4) == 0)
            {
                std::vector<std::uint8_t> fmtData(chunkSize);
                stream.read(reinterpret_cast<char*>(fmtData.data()), static_cast<std::streamsize>(chunkSize));
                if (chunkSize < 16)
                {
                    return false;
                }

                formatTag = *reinterpret_cast<std::uint16_t*>(fmtData.data());
                channelCount = *reinterpret_cast<std::uint16_t*>(fmtData.data() + 2);
                sampleRate = *reinterpret_cast<std::uint32_t*>(fmtData.data() + 4);
                bitsPerSample = *reinterpret_cast<std::uint16_t*>(fmtData.data() + 14);
            }
            else if (std::strncmp(chunkId, "data", 4) == 0)
            {
                pcmData.resize(chunkSize);
                stream.read(reinterpret_cast<char*>(pcmData.data()), static_cast<std::streamsize>(chunkSize));
            }
            else
            {
                stream.seekg(chunkSize, std::ios::cur);
            }

            if ((chunkSize & 1u) != 0u)
            {
                stream.seekg(1, std::ios::cur);
            }
        }

        if (pcmData.empty() || sampleRate == 0 || channelCount == 0)
        {
            return false;
        }

        const std::size_t bytesPerSample = std::max<std::size_t>(1, bitsPerSample / 8);
        const std::size_t frameSize = bytesPerSample * channelCount;
        if (frameSize == 0)
        {
            return false;
        }

        const std::size_t frameCount = pcmData.size() / frameSize;
        output.sampleRate = sampleRate;
        output.left.assign(frameCount, 0.0);
        output.right.assign(frameCount, 0.0);

        for (std::size_t frame = 0; frame < frameCount; ++frame)
        {
            const std::uint8_t* framePtr = pcmData.data() + (frame * frameSize);

            auto readSample = [&](std::size_t channel) -> double
            {
                const std::uint8_t* samplePtr = framePtr + (channel * bytesPerSample);

                if (formatTag == WAVE_FORMAT_IEEE_FLOAT && bitsPerSample == 32)
                {
                    float value = 0.0f;
                    std::memcpy(&value, samplePtr, sizeof(float));
                    return static_cast<double>(value);
                }

                if (formatTag == WAVE_FORMAT_PCM && bitsPerSample == 16)
                {
                    std::int16_t value = 0;
                    std::memcpy(&value, samplePtr, sizeof(value));
                    return static_cast<double>(value) / 32768.0;
                }

                if (formatTag == WAVE_FORMAT_PCM && bitsPerSample == 32)
                {
                    std::int32_t value = 0;
                    std::memcpy(&value, samplePtr, sizeof(value));
                    return static_cast<double>(value) / 2147483648.0;
                }

                return 0.0;
            };

            output.left[frame] = readSample(0);
            output.right[frame] = readSample(channelCount > 1 ? 1 : 0);
        }

        return true;
    }
}

AudioEngine::AudioEngine()
{
    resetMetrics();
    publishSnapshot();
}

AudioEngine::~AudioEngine()
{
    stopTelemetry();
    shutdown();
}

bool AudioEngine::initialize(const EngineConfig& config)
{
    state_.store(EngineState::Initializing);
    clearError();

    if (config.preferredSampleRate <= 0)
    {
        setError(kErrorInvalidConfig, "preferredSampleRate must be > 0");
        throw ConfigurationException(lastErrorMessage_);
    }

    if (config.preferredBlockSize <= 0)
    {
        setError(kErrorInvalidConfig, "preferredBlockSize must be > 0");
        throw ConfigurationException(lastErrorMessage_);
    }

    if (config.outputChannelCount <= 0)
    {
        setError(kErrorInvalidConfig, "outputChannelCount must be > 0");
        throw ConfigurationException(lastErrorMessage_);
    }

    stopTelemetry();
    stop();
    closeAudioBackend();

    config_ = config;
    deviceState_ = {};
    deviceState_.sampleRate = static_cast<std::uint32_t>(config_.preferredSampleRate);
    deviceState_.blockSize = static_cast<std::uint32_t>(config_.preferredBlockSize);
    deviceState_.inputChannels = static_cast<std::uint32_t>(config_.inputChannelCount);
    deviceState_.outputChannels = static_cast<std::uint32_t>(config_.outputChannelCount);
    deviceState_.backendName = "None";
    deviceState_.deviceName = "No Device";

    realtimeBuffer_.resize(deviceState_.blockSize);
    anticipativeBuffer_.resize(deviceState_.blockSize);

    editableGraph_ = {};
    pendingGraph_ = {};
    compiledGraph_ = {};
    graphSwapPending_ = false;

    automationLanes_.clear();
    latencyStates_.clear();
    delayLines_.clear();

    {
        std::lock_guard<std::mutex> pluginLock(pluginMutex_);
        for (auto& plugin : loadedPlugins_)
        {
            shutdownPluginSandbox(plugin);
        }
        loadedPlugins_.clear();
    }

    {
        std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
        clipCache_.clear();
    }

    {
        std::lock_guard<std::mutex> diskLock(diskQueueMutex_);
        diskReadQueue_.clear();
    }

    lastOfflineRenderRequest_.reset();
    undoStack_.clear();
    redoStack_.clear();
    commandQueue_.clearUnsafe();

    if (!initializeProjectState())
    {
        return false;
    }

    transportInfo_ = {};
    transportInfo_.tempoBpm = 120.0;
    transportInfo_.monitoringEnabled = true;

    resetMetrics();
    initialized_.store(true);
    running_.store(false);
    telemetryRunning_.store(false);

    state_.store(EngineState::Stopped);
    updateStatusFromState();
    publishSnapshot();
    return true;
}

void AudioEngine::shutdown()
{
    stopTelemetry();
    stop();

    {
        std::lock_guard<std::mutex> pluginLock(pluginMutex_);
        for (auto& plugin : loadedPlugins_)
        {
            shutdownPluginSandbox(plugin);
        }
    }

    closeAudioBackend();
    initialized_.store(false);
    state_.store(EngineState::Uninitialized);
    updateStatusFromState();
    publishSnapshot();
}

bool AudioEngine::initializeAudioBackend()
{
    if (!initialized_.load())
    {
        setError(kErrorBackendInit, "Engine must be initialized before backend setup.");
        return false;
    }

    clearError();
    closeAudioBackend();

    if (config_.enableWasapi && openWasapiBackend())
    {
        if (state_.load() == EngineState::Error)
        {
            state_.store(EngineState::Stopped);
        }
        publishSnapshot();
        return true;
    }

    clearError();
    deviceState_.backendType = BackendType::Dummy;
    deviceState_.backendName = "Dummy";
    deviceState_.deviceName = "Dummy Audio Device";
    deviceState_.sampleRate = static_cast<std::uint32_t>(config_.preferredSampleRate);
    deviceState_.blockSize = static_cast<std::uint32_t>(config_.preferredBlockSize);
    deviceState_.deviceBufferFrames = deviceState_.blockSize * 2;
    deviceState_.format.sampleRate = deviceState_.sampleRate;
    deviceState_.format.channelCount = static_cast<std::uint32_t>(config_.outputChannelCount);
    deviceState_.format.bitsPerSample = 32;
    deviceState_.format.floatingPoint = true;
    deviceState_.initialized = true;
    if (state_.load() == EngineState::Error)
    {
        state_.store(EngineState::Stopped);
    }
    publishSnapshot();
    return true;
}

bool AudioEngine::recoverAudioDevice()
{
    ++metrics_.recoveryCount;
    deviceState_.recovery.restartPending = false;
    deviceState_.recovery.deviceLost = false;

    stopAudioStream();
    closeAudioBackend();

    if (!initializeAudioBackend())
    {
        return false;
    }

    if (running_.load())
    {
        return startAudioStream();
    }

    return true;
}

bool AudioEngine::start()
{
    if (!initialized_.load())
    {
        setError(kErrorBackendInit, "Engine not initialized.");
        return false;
    }

    if (!deviceState_.initialized && !initializeAudioBackend())
    {
        return false;
    }

    if (!compiledGraph_.valid)
    {
        if (!buildInitialGraph() || !compileGraph())
        {
            return false;
        }
    }

    if (running_.load())
    {
        return true;
    }

    state_.store(EngineState::Starting);
    updateStatusFromState();
    publishSnapshot();

    audioThreadState_.stopRequested.store(false);
    audioThreadState_.helpersRunning.store(true);
    diskWorkerRunning_.store(true);
    running_.store(true);

    helperThreads_.clear();
    const int helperCount = std::max(1, config_.helperThreadCount);
    for (int index = 0; index < helperCount; ++index)
    {
        helperThreads_.emplace_back([this]()
        {
            while (audioThreadState_.helpersRunning.load())
            {
                std::uint64_t targetBlock = 0;

                {
                    std::unique_lock<std::mutex> lock(anticipativeMutex_);
                    anticipativeCv_.wait(
                        lock,
                        [this]()
                        {
                            return !audioThreadState_.helpersRunning.load() || anticipativeWorkPending_;
                        });

                    if (!audioThreadState_.helpersRunning.load())
                    {
                        return;
                    }

                    if (!anticipativeWorkPending_)
                    {
                        continue;
                    }

                    targetBlock = requestedAnticipativeBlock_;
                    anticipativeWorkPending_ = false;
                }

                computeAnticipativeBlock(targetBlock);
            }
        });
    }

    if (!diskThread_.joinable())
    {
        diskThread_ = std::thread(&AudioEngine::diskWorkerMain, this);
    }

    audioThread_ = std::thread(&AudioEngine::audioThreadMain, this);
    return true;
}

bool AudioEngine::stop()
{
    if (!initialized_.load())
    {
        return false;
    }

    state_.store(EngineState::Stopping);
    updateStatusFromState();

    running_.store(false);
    audioThreadState_.stopRequested.store(true);
    audioThreadState_.helpersRunning.store(false);
    diskWorkerRunning_.store(false);
    anticipativeCv_.notify_all();
    diskQueueCv_.notify_all();

    if (audioThread_.joinable())
    {
        audioThread_.join();
    }

    for (auto& thread : helperThreads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    helperThreads_.clear();

    if (diskThread_.joinable())
    {
        diskThread_.join();
    }

    stopAudioStream();

    if (state_.load() != EngineState::Error)
    {
        state_.store(EngineState::Stopped);
    }

    updateStatusFromState();
    publishSnapshot();
    return true;
}

AudioEngine::EngineState AudioEngine::getState() const noexcept
{
    return state_.load();
}

bool AudioEngine::isRunning() const noexcept
{
    return running_.load();
}

bool AudioEngine::isInitialized() const noexcept
{
    return initialized_.load();
}

std::string AudioEngine::getStatusText() const
{
    return getUiSnapshot().statusText;
}

std::string AudioEngine::getLastErrorMessage() const
{
    return getUiSnapshot().lastErrorMessage;
}

int AudioEngine::getLastErrorCode() const noexcept
{
    return getUiSnapshot().lastErrorCode;
}

const AudioEngine::EngineConfig& AudioEngine::getConfig() const noexcept
{
    return config_;
}

AudioEngine::EngineMetrics AudioEngine::getMetrics() const noexcept
{
    return getUiSnapshot().metrics;
}

AudioEngine::EngineSnapshot AudioEngine::getUiSnapshot() const
{
    const int activeIndex = activeSnapshotIndex_.load(std::memory_order_acquire);
    return uiSnapshots_[activeIndex];
}

AudioEngine::ProjectSnapshot AudioEngine::getProjectSnapshot() const
{
    return getUiSnapshot().project;
}

std::string AudioEngine::getBackendName() const
{
    return getUiSnapshot().device.backendName;
}

std::string AudioEngine::getCurrentDeviceName() const
{
    return getUiSnapshot().device.deviceName;
}

int AudioEngine::getCurrentSampleRate() const noexcept
{
    return static_cast<int>(getUiSnapshot().device.sampleRate);
}

int AudioEngine::getCurrentBlockSize() const noexcept
{
    return static_cast<int>(getUiSnapshot().device.blockSize);
}

bool AudioEngine::initializeTransport()
{
    if (!initialized_.load())
    {
        setError(kErrorTransportInit, "Engine must be initialized before transport.");
        return false;
    }

    transportInfo_.state = TransportState::Stopped;
    transportInfo_.tempoBpm = 120.0;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;
    transportInfo_.monitoringEnabled = true;
    publishSnapshot();
    return true;
}

void AudioEngine::play()
{
    transportInfo_.state = TransportState::Playing;
    publishSnapshot();
}

void AudioEngine::pause()
{
    transportInfo_.state = TransportState::Paused;
    publishSnapshot();
}

void AudioEngine::stopTransport()
{
    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;
    publishSnapshot();
}

void AudioEngine::setTimelinePosition(double seconds)
{
    transportInfo_.timelineSeconds = std::max(0.0, seconds);
    transportInfo_.samplePosition = static_cast<std::uint64_t>(
        transportInfo_.timelineSeconds * static_cast<double>(std::max(1u, deviceState_.sampleRate)));
    publishSnapshot();
}

void AudioEngine::setTempo(double bpm)
{
    transportInfo_.tempoBpm = std::max(1.0, bpm);
    publishSnapshot();
}

void AudioEngine::setSamplePosition(std::uint64_t samplePosition)
{
    transportInfo_.samplePosition = samplePosition;
    transportInfo_.timelineSeconds =
        static_cast<double>(samplePosition) / static_cast<double>(std::max(1u, deviceState_.sampleRate));
    publishSnapshot();
}

bool AudioEngine::isPlaying() const noexcept
{
    return getUiSnapshot().transport.state == TransportState::Playing;
}

bool AudioEngine::isMonitoring() const noexcept
{
    return getUiSnapshot().transport.monitoringEnabled;
}

AudioEngine::TransportInfo AudioEngine::getTransportInfo() const noexcept
{
    return getUiSnapshot().transport;
}

bool AudioEngine::buildInitialGraph()
{
    if (!initialized_.load())
    {
        setError(kErrorGraphBuild, "Engine must be initialized before building graph.");
        return false;
    }

    editableGraph_ = buildGraphFromProjectState();
    editableGraph_.version = std::max<std::uint64_t>(1, projectState_.revision + 1);
    pendingGraph_ = editableGraph_;
    graphSwapPending_ = true;
    publishSnapshot();
    return true;
}

bool AudioEngine::compileGraph()
{
    const GraphSnapshot& sourceGraph = graphSwapPending_ ? pendingGraph_ : editableGraph_;

    if (sourceGraph.nodes.empty())
    {
        setError(kErrorGraphCompile, "Cannot compile an empty graph.");
        return false;
    }

    std::unordered_map<std::uint32_t, GraphNode> nodesById;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> adjacency;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> reverseAdjacency;
    std::unordered_map<std::uint32_t, std::uint32_t> indegree;

    for (const auto& node : sourceGraph.nodes)
    {
        nodesById[node.id] = node;
        indegree[node.id] = 0;
    }

    for (const auto& edge : sourceGraph.edges)
    {
        adjacency[edge.sourceNodeId].push_back(edge.destinationNodeId);
        reverseAdjacency[edge.destinationNodeId].push_back(edge.sourceNodeId);
        ++indegree[edge.destinationNodeId];
    }

    std::vector<std::uint32_t> currentLayer;
    for (const auto& [nodeId, degree] : indegree)
    {
        if (degree == 0)
        {
            currentLayer.push_back(nodeId);
        }
    }

    std::sort(currentLayer.begin(), currentLayer.end());

    CompiledGraph compiled{};
    compiled.sourceVersion = sourceGraph.version;
    compiled.valid = true;

    std::unordered_map<std::uint32_t, std::uint32_t> accumulatedLatency;
    std::unordered_map<std::uint32_t, std::uint32_t> branchGroup;
    std::uint32_t executionOrder = 0;
    std::uint32_t stageIndex = 0;
    std::size_t processedCount = 0;

    while (!currentLayer.empty())
    {
        ExecutionStage stage{};
        stage.stageIndex = stageIndex++;
        stage.liveLane = false;
        stage.anticipativeLane = false;

        std::vector<std::uint32_t> nextLayer;
        std::sort(currentLayer.begin(), currentLayer.end());

        for (const std::uint32_t nodeId : currentLayer)
        {
            const GraphNode& sourceNode = nodesById[nodeId];

            CompiledNode compiledNode{};
            compiledNode.nodeId = nodeId;
            compiledNode.type = sourceNode.type;
            compiledNode.executionOrder = executionOrder++;
            compiledNode.stageIndex = stage.stageIndex;
            compiledNode.supportsDoublePrecision = sourceNode.supportsDoublePrecision;
            compiledNode.upstreamNodeIds = reverseAdjacency[nodeId];
            compiledNode.downstreamNodeIds = adjacency[nodeId];

            bool liveLane = sourceNode.liveCritical;
            bool anticipativeLane = sourceNode.canProcessAnticipatively && config_.enableAnticipativeProcessing;
            std::uint32_t upstreamLatency = 0;

            for (const std::uint32_t upstreamId : compiledNode.upstreamNodeIds)
            {
                upstreamLatency = std::max(upstreamLatency, accumulatedLatency[upstreamId]);

                const auto upstreamIt = compiled.nodeLookup.find(upstreamId);
                if (upstreamIt != compiled.nodeLookup.end())
                {
                    const CompiledNode& upstreamNode = compiled.executionPlan[upstreamIt->second];
                    liveLane = liveLane || upstreamNode.liveLane;
                    anticipativeLane = anticipativeLane || upstreamNode.anticipativeLane;
                    compiledNode.branchGroup = upstreamNode.branchGroup;
                }
            }

            if (compiledNode.upstreamNodeIds.empty())
            {
                compiledNode.branchGroup = nodeId;
            }

            if (sourceNode.type == NodeType::Input)
            {
                anticipativeLane = false;
            }

            compiledNode.liveLane = liveLane;
            compiledNode.anticipativeLane = anticipativeLane;
            compiledNode.accumulatedLatencySamples = upstreamLatency + sourceNode.latencySamples;
            accumulatedLatency[nodeId] = compiledNode.accumulatedLatencySamples;
            branchGroup[nodeId] = compiledNode.branchGroup;

            stage.liveLane = stage.liveLane || liveLane;
            stage.anticipativeLane = stage.anticipativeLane || anticipativeLane;
            stage.nodeIds.push_back(nodeId);

            compiled.nodeLookup[nodeId] = compiled.executionPlan.size();
            compiled.executionPlan.push_back(compiledNode);
            compiled.topologicalOrder.push_back(nodeId);
            ++processedCount;

            for (const std::uint32_t downstreamId : adjacency[nodeId])
            {
                auto indegreeIt = indegree.find(downstreamId);
                if (indegreeIt == indegree.end())
                {
                    continue;
                }

                if (indegreeIt->second > 0)
                {
                    --indegreeIt->second;
                }

                if (indegreeIt->second == 0)
                {
                    nextLayer.push_back(downstreamId);
                }
            }
        }

        compiled.stages.push_back(stage);
        currentLayer = nextLayer;
    }

    if (processedCount != sourceGraph.nodes.size())
    {
        compiled.valid = false;
        compiled.hasCycle = true;
        setError(kErrorGraphCompile, "Cycle detected while compiling graph.");
        return false;
    }

    std::unordered_map<std::uint32_t, CompiledSubgraph> subgraphs;
    for (const auto& node : sourceGraph.nodes)
    {
        if (node.type != NodeType::Track && node.type != NodeType::Input)
        {
            continue;
        }

        CompiledSubgraph subgraph{};
        subgraph.rootNodeId = node.id;
        subgraph.containsLiveNode = node.liveCritical;
        subgraph.containsBus = false;

        std::vector<std::uint32_t> stack{node.id};
        std::unordered_set<std::uint32_t> visited;
        while (!stack.empty())
        {
            const std::uint32_t current = stack.back();
            stack.pop_back();

            if (!visited.insert(current).second)
            {
                continue;
            }

            subgraph.nodeIds.push_back(current);

            const auto nodeIt = nodesById.find(current);
            if (nodeIt != nodesById.end())
            {
                subgraph.containsLiveNode = subgraph.containsLiveNode || nodeIt->second.liveCritical;
                subgraph.containsBus = subgraph.containsBus || nodeIt->second.type == NodeType::Bus;
            }

            for (const std::uint32_t downstream : adjacency[current])
            {
                stack.push_back(downstream);
            }
        }

        subgraphs[subgraph.rootNodeId] = subgraph;
    }

    compiled.subgraphs.clear();
    for (auto& [_, subgraph] : subgraphs)
    {
        std::sort(subgraph.nodeIds.begin(), subgraph.nodeIds.end());
        compiled.subgraphs.push_back(std::move(subgraph));
    }

    editableGraph_ = sourceGraph;
    compiledGraph_ = compiled;
    metrics_.activeGraphVersion = compiledGraph_.sourceVersion;
    graphSwapPending_ = false;

    std::vector<AutomationLane> newLanes;
    for (const auto& node : editableGraph_.nodes)
    {
        if (node.type != NodeType::Track && node.type != NodeType::Plugin)
        {
            continue;
        }

        AutomationLane lane{};
        lane.nodeId = node.id;
        lane.sampleAccurateEnabled = config_.enableSampleAccurateAutomation;
        lane.parameterStates.push_back(ParameterState{
            kDefaultAutomationParameter,
            node.baseGain,
            node.baseGain,
            ParameterBinding{kDefaultAutomationParameter, "gain", false},
            ParameterSmoother{node.baseGain, node.baseGain, 0.1, ParameterSmoothingMode::Linear, true}});

        if (AutomationLane* existingLane = findAutomationLane(node.id))
        {
            lane.pendingEvents = existingLane->pendingEvents;
            lane.parameterStates = existingLane->parameterStates;
        }

        newLanes.push_back(std::move(lane));
    }
    automationLanes_ = std::move(newLanes);

    recalculateLatencyModel();
    publishSnapshot();
    return true;
}

void AudioEngine::requestGraphRebuild()
{
    pendingGraph_ = buildGraphFromProjectState();
    pendingGraph_.version = std::max(editableGraph_.version + 1, projectState_.revision + 1);
    graphSwapPending_ = true;
    publishSnapshot();
}

bool AudioEngine::swapCompiledGraphAtSafePoint()
{
    if (!graphSwapPending_)
    {
        return false;
    }

    return compileGraph();
}

std::uint64_t AudioEngine::getActiveGraphVersion() const noexcept
{
    return getUiSnapshot().metrics.activeGraphVersion;
}

void AudioEngine::processLiveBlock(RenderContext& renderContext, AudioBuffer& ioBuffer)
{
    ioBuffer.resize(renderContext.callback.blockSize);
    ioBuffer.clear();
    renderGraphBlock(renderContext, ioBuffer, false);
}

void AudioEngine::processAnticipativeWork(RenderContext& renderContext)
{
    anticipativeBuffer_.resize(renderContext.callback.blockSize);
    anticipativeBuffer_.clear();

    if (!config_.enableAnticipativeProcessing)
    {
        return;
    }

    consumeAnticipativeBlock(renderContext.callback.callbackIndex, anticipativeBuffer_);
    scheduleAnticipativeBlock(renderContext.callback.callbackIndex);
}

void AudioEngine::mergeLiveAndAnticipativeResults(RenderContext&, AudioBuffer& ioBuffer)
{
    mixBuffer(ioBuffer, anticipativeBuffer_);
}

void AudioEngine::enqueueAutomationEvent(const AutomationEvent& event)
{
    AutomationLane* lane = findAutomationLane(event.nodeId);
    if (lane == nullptr)
    {
        AutomationLane newLane{};
        newLane.nodeId = event.nodeId;
        newLane.sampleAccurateEnabled = config_.enableSampleAccurateAutomation;
        newLane.pendingEvents.push_back(event);
        newLane.parameterStates.push_back(ParameterState{
            event.parameterId,
            event.value,
            event.value,
            ParameterBinding{event.parameterId, "param", false},
            ParameterSmoother{event.value, event.value, 0.1, ParameterSmoothingMode::Linear, true}});
        automationLanes_.push_back(std::move(newLane));
        return;
    }

    lane->pendingEvents.push_back(event);
}

void AudioEngine::applyAutomationForBlock(RenderContext& renderContext)
{
    for (auto& lane : automationLanes_)
    {
        lane.activeSegments.clear();

        if (!lane.sampleAccurateEnabled || lane.parameterStates.empty())
        {
            continue;
        }

        std::sort(
            lane.pendingEvents.begin(),
            lane.pendingEvents.end(),
            [](const AutomationEvent& left, const AutomationEvent& right)
            {
                if (left.parameterId != right.parameterId)
                {
                    return left.parameterId < right.parameterId;
                }

                return left.sampleOffset < right.sampleOffset;
            });

        for (auto& parameterState : lane.parameterStates)
        {
            std::uint32_t previousSample = 0;
            double previousValue = parameterState.currentValue;
            bool emittedAnySegment = false;

            for (const auto& event : lane.pendingEvents)
            {
                if (event.parameterId != parameterState.parameterId)
                {
                    continue;
                }

                const std::uint32_t clampedOffset = std::min(event.sampleOffset, renderContext.callback.blockSize);
                lane.activeSegments.push_back(AutomationSegment{
                    lane.nodeId,
                    event.parameterId,
                    previousSample,
                    clampedOffset,
                    previousValue,
                    event.value,
                    parameterState.smoother.mode});

                previousSample = clampedOffset;
                previousValue = event.value;
                emittedAnySegment = true;
            }

            lane.activeSegments.push_back(AutomationSegment{
                lane.nodeId,
                parameterState.parameterId,
                previousSample,
                renderContext.callback.blockSize,
                previousValue,
                previousValue,
                parameterState.smoother.mode});

            if (!emittedAnySegment && !config_.enableSampleAccurateAutomation)
            {
                parameterState.currentValue = parameterState.targetValue;
            }
            else
            {
                parameterState.currentValue = previousValue;
                parameterState.targetValue = previousValue;
                parameterState.smoother.currentValue = previousValue;
                parameterState.smoother.targetValue = previousValue;
            }
        }

        lane.pendingEvents.clear();
    }
}

void AudioEngine::flushAutomationToNode(std::uint32_t nodeId)
{
    if (AutomationLane* lane = findAutomationLane(nodeId))
    {
        lane->pendingEvents.clear();
        lane->activeSegments.clear();
    }
}

void AudioEngine::recalculateLatencyModel()
{
    latencyStates_.clear();
    delayLines_.clear();

    if (!compiledGraph_.valid)
    {
        metrics_.currentLatencySamples = 0;
        return;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> localLatency;
    for (const auto& node : editableGraph_.nodes)
    {
        localLatency[node.id] = node.latencySamples;
    }

    std::uint32_t maxLatency = 0;
    for (auto& compiledNode : compiledGraph_.executionPlan)
    {
        std::uint32_t upstreamMaxLatency = 0;
        for (const std::uint32_t upstreamId : compiledNode.upstreamNodeIds)
        {
            const auto it = std::find_if(
                latencyStates_.begin(),
                latencyStates_.end(),
                [&](const DelayCompensationState& state) { return state.nodeId == upstreamId; });

            if (it != latencyStates_.end())
            {
                upstreamMaxLatency = std::max(upstreamMaxLatency, it->accumulatedLatencySamples);
            }
        }

        DelayCompensationState state{};
        state.nodeId = compiledNode.nodeId;
        state.localLatencySamples = localLatency[compiledNode.nodeId];
        state.upstreamLatencySamples = upstreamMaxLatency;
        state.accumulatedLatencySamples = upstreamMaxLatency + state.localLatencySamples;
        latencyStates_.push_back(state);
        maxLatency = std::max(maxLatency, state.accumulatedLatencySamples);
    }

    for (auto& state : latencyStates_)
    {
        state.compensationDelaySamples = maxLatency > state.accumulatedLatencySamples
            ? maxLatency - state.accumulatedLatencySamples
            : 0;

        DelayLine delayLine{};
        delayLine.delaySamples = state.compensationDelaySamples;
        const std::uint32_t capacity = std::max<std::uint32_t>(1, delayLine.delaySamples + deviceState_.blockSize + 1);
        delayLine.left.assign(capacity, 0.0);
        delayLine.right.assign(capacity, 0.0);
        delayLines_[state.nodeId] = std::move(delayLine);

        const auto compiledIt = compiledGraph_.nodeLookup.find(state.nodeId);
        if (compiledIt != compiledGraph_.nodeLookup.end())
        {
            CompiledNode& compiledNode = compiledGraph_.executionPlan[compiledIt->second];
            compiledNode.accumulatedLatencySamples = state.accumulatedLatencySamples;
            compiledNode.compensationDelaySamples = state.compensationDelaySamples;
        }
    }

    metrics_.currentLatencySamples = maxLatency;
}

void AudioEngine::alignBranchesForMix(AudioBuffer& ioBuffer)
{
    for (std::uint32_t frame = 0; frame < ioBuffer.frameCount; ++frame)
    {
        ioBuffer.left[frame] = clampSample(ioBuffer.left[frame]);
        ioBuffer.right[frame] = clampSample(ioBuffer.right[frame]);
    }
}

bool AudioEngine::addPluginStub(const PluginDescriptor& descriptor, std::uint32_t ownerNodeId)
{
    PluginInstance instance{};
    instance.instanceId = nextPluginInstanceId();
    instance.descriptor = descriptor;
    instance.ownerNodeId = ownerNodeId;
    instance.active = true;
    instance.sandboxed = descriptor.processMode != PluginProcessMode::InProcess && config_.enablePluginSandbox;

    if (instance.sandboxed)
    {
        spawnPluginSandbox(instance);
    }

    {
        std::lock_guard<std::mutex> pluginLock(pluginMutex_);
        loadedPlugins_.push_back(instance);
    }

    for (auto& node : editableGraph_.nodes)
    {
        if (node.id == ownerNodeId)
        {
            node.pluginInstanceId = instance.instanceId;
            node.supportsDoublePrecision = descriptor.supportsDoublePrecision;
            node.latencySamples = std::max(node.latencySamples, descriptor.reportedLatencySamples);
            break;
        }
    }

    requestGraphRebuild();
    return true;
}

std::vector<AudioEngine::PluginDescriptor> AudioEngine::getLoadedPluginDescriptors() const
{
    std::vector<PluginDescriptor> descriptors;
    std::lock_guard<std::mutex> pluginLock(pluginMutex_);
    descriptors.reserve(loadedPlugins_.size());

    for (const auto& plugin : loadedPlugins_)
    {
        descriptors.push_back(plugin.descriptor);
    }

    return descriptors;
}

bool AudioEngine::renderOffline(const OfflineRenderRequest& request)
{
    if (!config_.enableOfflineRender)
    {
        setError(kErrorOfflineRender, "Offline render subsystem is disabled.");
        return false;
    }

    if (request.endTimeSeconds <= request.startTimeSeconds)
    {
        setError(kErrorOfflineRender, "Offline render time range is invalid.");
        return false;
    }

    lastOfflineRenderRequest_ = request;

    const std::uint64_t startSample = static_cast<std::uint64_t>(
        request.startTimeSeconds * static_cast<double>(request.sampleRate));
    const std::uint64_t endSample = static_cast<std::uint64_t>(
        request.endTimeSeconds * static_cast<double>(request.sampleRate));
    const std::uint64_t totalFrames = endSample - startSample;
    const std::uint32_t blockSize = std::max<std::uint32_t>(1u, deviceState_.blockSize);

    AudioBuffer aggregateBuffer{};
    aggregateBuffer.resize(static_cast<std::uint32_t>(totalFrames));
    aggregateBuffer.clear();

    AudioBuffer blockBuffer{};
    blockBuffer.resize(blockSize);

    const std::uint64_t totalBlocks = (totalFrames + blockSize - 1) / blockSize;

    for (std::uint64_t blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
    {
        const std::uint64_t absoluteBlockStart = startSample + (blockIndex * blockSize);
        const std::uint32_t framesThisBlock = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(blockSize, totalFrames - (blockIndex * blockSize)));

        RenderContext renderContext{};
        renderContext.isOffline = true;
        renderContext.isLivePath = false;
        renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;
        renderContext.callback.sampleRate = request.sampleRate;
        renderContext.callback.blockSize = framesThisBlock;
        renderContext.callback.callbackIndex = blockIndex;
        renderContext.callback.transportSampleStart = absoluteBlockStart;

        blockBuffer.resize(framesThisBlock);
        blockBuffer.clear();

        if (graphSwapPending_)
        {
            swapCompiledGraphAtSafePoint();
        }

        applyAutomationForBlock(renderContext);
        processLiveBlock(renderContext, blockBuffer);
        if (config_.enableAnticipativeProcessing)
        {
            AudioBuffer anticipative{};
            anticipative.resize(framesThisBlock);
            renderGraphBlock(renderContext, anticipative, true);
            mixBuffer(blockBuffer, anticipative);
        }
        alignBranchesForMix(blockBuffer);

        for (std::uint32_t frame = 0; frame < framesThisBlock; ++frame)
        {
            const std::size_t destinationIndex = static_cast<std::size_t>((blockIndex * blockSize) + frame);
            aggregateBuffer.left[destinationIndex] = blockBuffer.left[frame];
            aggregateBuffer.right[destinationIndex] = blockBuffer.right[frame];
        }
    }

    metrics_.renderedOfflineFrames += totalFrames;

    if (!writeWavFile(request.outputPath, request.sampleRate, aggregateBuffer))
    {
        setError(kErrorOfflineRender, "Failed to write offline render to disk.");
        return false;
    }

    publishSnapshot();
    return true;
}

bool AudioEngine::freezeTrack(std::uint32_t trackNodeId)
{
    OfflineRenderRequest request{};
    request.startTimeSeconds = 0.0;
    request.endTimeSeconds = 8.0;
    request.sampleRate = deviceState_.sampleRate;
    request.outputPath = "freeze_track_" + std::to_string(trackNodeId) + ".wav";
    request.highQuality = true;
    return renderOffline(request);
}

bool AudioEngine::addTrack(const std::string& name)
{
    const ProjectState beforeState = projectState_;

    TrackState track{};
    track.trackId = nextTrackId();
    track.busId = projectState_.buses.empty() ? nextBusId() : projectState_.buses.front().busId;
    track.name = name.empty() ? ("Track " + std::to_string(track.trackId)) : name;

    if (projectState_.buses.empty())
    {
        BusState bus{};
        bus.busId = nextBusId();
        bus.name = "Main Bus";
        projectState_.buses.push_back(bus);
        track.busId = bus.busId;
    }

    projectState_.tracks.push_back(track);

    auto busIt = std::find_if(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [&](const BusState& bus) { return bus.busId == track.busId; });

    if (busIt != projectState_.buses.end())
    {
        busIt->inputTrackIds.push_back(track.trackId);
    }

    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add track", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::addBus(const std::string& name)
{
    const ProjectState beforeState = projectState_;

    BusState bus{};
    bus.busId = nextBusId();
    bus.name = name.empty() ? ("Bus " + std::to_string(bus.busId)) : name;
    projectState_.buses.push_back(bus);
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add bus", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::addClipToTrack(std::uint32_t trackId, const std::string& clipName)
{
    auto trackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == trackId; });

    if (trackIt == projectState_.tracks.end())
    {
        setError(kErrorProjectIo, "Track not found while adding clip.");
        return false;
    }

    const ProjectState beforeState = projectState_;

    ClipState clip{};
    clip.clipId = nextClipId();
    clip.trackId = trackId;
    clip.name = clipName.empty() ? ("Clip " + std::to_string(clip.clipId)) : clipName;
    clip.sourceType = ClipSourceType::GeneratedTone;
    clip.startTimeSeconds = 0.0;
    clip.durationSeconds = 4.0;
    clip.gain = 0.8;

    trackIt->clipIds.push_back(clip.clipId);
    projectState_.clips.push_back(clip);
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add clip", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::moveClip(std::uint32_t clipId, std::uint32_t targetTrackId, double startTimeSeconds)
{
    auto clipIt = std::find_if(
        projectState_.clips.begin(),
        projectState_.clips.end(),
        [&](const ClipState& clip) { return clip.clipId == clipId; });

    if (clipIt == projectState_.clips.end())
    {
        setError(kErrorProjectIo, "Clip not found while moving clip.");
        return false;
    }

    auto targetTrackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == targetTrackId; });

    if (targetTrackIt == projectState_.tracks.end())
    {
        setError(kErrorProjectIo, "Target track not found while moving clip.");
        return false;
    }

    const ProjectState beforeState = projectState_;
    const std::uint32_t previousTrackId = clipIt->trackId;

    if (previousTrackId != targetTrackId)
    {
        auto previousTrackIt = std::find_if(
            projectState_.tracks.begin(),
            projectState_.tracks.end(),
            [&](const TrackState& track) { return track.trackId == previousTrackId; });

        if (previousTrackIt != projectState_.tracks.end())
        {
            previousTrackIt->clipIds.erase(
                std::remove(previousTrackIt->clipIds.begin(), previousTrackIt->clipIds.end(), clipId),
                previousTrackIt->clipIds.end());
        }

        targetTrackIt->clipIds.push_back(clipId);
        clipIt->trackId = targetTrackId;
    }

    clipIt->startTimeSeconds = std::max(0.0, startTimeSeconds);
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Move clip", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::newProject(const std::string& name)
{
    clearError();

    config_.projectName = name.empty() ? "Untitled Project" : name;

    editableGraph_ = {};
    pendingGraph_ = {};
    compiledGraph_ = {};
    graphSwapPending_ = false;

    automationLanes_.clear();
    latencyStates_.clear();
    delayLines_.clear();

    {
        std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
        clipCache_.clear();
    }
    metrics_.cachedClipCount = 0;

    {
        std::lock_guard<std::mutex> diskLock(diskQueueMutex_);
        diskReadQueue_.clear();
    }

    undoStack_.clear();
    redoStack_.clear();
    lastOfflineRenderRequest_.reset();

    if (!initializeProjectState())
    {
        setError(kErrorProjectIo, "Failed to create a new project state.");
        return false;
    }

    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;

    if (!buildInitialGraph() || !compileGraph())
    {
        setError(kErrorGraphBuild, "Failed to initialize graph for new project.");
        return false;
    }

    updateStatusFromState();
    publishSnapshot();
    return true;
}

bool AudioEngine::saveProject(const std::string& path)
{
    const std::string resolvedPath = path.empty() ? config_.sessionPath : path;
    std::filesystem::path sessionPath = resolvedPath;

    if (sessionPath.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(sessionPath.parent_path(), ec);
    }

    std::ofstream stream(resolvedPath, std::ios::binary);
    if (!stream.is_open())
    {
        setError(kErrorProjectIo, "Failed to open session file for writing.");
        return false;
    }

    stream << "PROJECT|" << projectState_.projectName << '|' << resolvedPath << '|' << projectState_.revision << '\n';

    for (const auto& bus : projectState_.buses)
    {
        stream << "BUS|" << bus.busId << '|' << bus.name << '\n';
    }

    for (const auto& track : projectState_.tracks)
    {
        stream
            << "TRACK|" << track.trackId << '|' << track.busId << '|'
            << track.name << '|'
            << (track.armed ? 1 : 0) << '|'
            << (track.muted ? 1 : 0) << '|'
            << (track.solo ? 1 : 0) << '\n';
    }

    for (const auto& clip : projectState_.clips)
    {
        stream
            << "CLIP|" << clip.clipId << '|' << clip.trackId << '|'
            << clip.name << '|'
            << static_cast<int>(clip.sourceType) << '|'
            << clip.filePath << '|'
            << clip.startTimeSeconds << '|'
            << clip.durationSeconds << '|'
            << clip.gain << '|'
            << (clip.muted ? 1 : 0) << '\n';
    }

    for (const auto& marker : projectState_.markers)
    {
        stream << "MARKER|" << marker.markerId << '|' << marker.name << '|' << marker.timeSeconds << '\n';
    }

    projectState_.sessionPath = resolvedPath;
    projectState_.dirty = false;
    config_.sessionPath = resolvedPath;
    publishSnapshot();
    return true;
}

bool AudioEngine::loadProject(const std::string& path)
{
    const std::string resolvedPath = path.empty() ? config_.sessionPath : path;
    std::ifstream stream(resolvedPath, std::ios::binary);
    if (!stream.is_open())
    {
        setError(kErrorProjectIo, "Failed to open session file for reading.");
        return false;
    }

    ProjectState loadedProject{};
    loadedProject.sessionPath = resolvedPath;

    try
    {
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty())
            {
                continue;
            }

            const std::vector<std::string> parts = splitString(line, '|');
            if (parts.empty())
            {
                continue;
            }

            if (parts[0] == "PROJECT" && parts.size() >= 4)
            {
                loadedProject.projectName = parts[1];
                loadedProject.sessionPath = parts[2];
                loadedProject.revision = static_cast<std::uint64_t>(std::stoull(parts[3]));
            }
            else if (parts[0] == "BUS" && parts.size() >= 3)
            {
                loadedProject.buses.push_back(BusState{
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    parts[2],
                    {}});
            }
            else if (parts[0] == "TRACK" && parts.size() >= 7)
            {
                loadedProject.tracks.push_back(TrackState{
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    static_cast<std::uint32_t>(std::stoul(parts[2])),
                    parts[3],
                    parts[4] == "1",
                    parts[5] == "1",
                    parts[6] == "1",
                    {},
                    {}});
            }
            else if (parts[0] == "CLIP" && parts.size() >= 10)
            {
                ClipState clip{};
                clip.clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                clip.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                clip.name = parts[3];
                clip.sourceType = static_cast<ClipSourceType>(std::stoi(parts[4]));
                clip.filePath = parts[5];
                clip.startTimeSeconds = std::stod(parts[6]);
                clip.durationSeconds = std::stod(parts[7]);
                clip.gain = std::stod(parts[8]);
                clip.muted = parts[9] == "1";
                loadedProject.clips.push_back(clip);
            }
            else if (parts[0] == "MARKER" && parts.size() >= 4)
            {
                loadedProject.markers.push_back(MarkerState{
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    parts[2],
                    std::stod(parts[3])});
            }
        }
    }
    catch (const std::exception&)
    {
        setError(kErrorProjectIo, "Session file is malformed or contains unsupported values.");
        return false;
    }

    if (loadedProject.projectName.empty())
    {
        loadedProject.projectName = std::filesystem::path(resolvedPath).stem().string();
    }

    if (loadedProject.buses.empty())
    {
        loadedProject.buses.push_back(BusState{1, "Main Bus", {}});
    }

    if (loadedProject.tracks.empty())
    {
        loadedProject.tracks.push_back(TrackState{
            1,
            loadedProject.buses.front().busId,
            "Track 1",
            false,
            false,
            false,
            {},
            {}});
    }

    for (auto& track : loadedProject.tracks)
    {
        if (track.busId == 0)
        {
            track.busId = loadedProject.buses.front().busId;
        }

        for (const auto& clip : loadedProject.clips)
        {
            if (clip.trackId == track.trackId)
            {
                track.clipIds.push_back(clip.clipId);
            }
        }

        auto busIt = std::find_if(
            loadedProject.buses.begin(),
            loadedProject.buses.end(),
            [&](const BusState& bus) { return bus.busId == track.busId; });

        if (busIt != loadedProject.buses.end())
        {
            busIt->inputTrackIds.push_back(track.trackId);
        }
    }

    projectState_ = std::move(loadedProject);
    projectState_.dirty = false;
    projectState_.revision = std::max<std::uint64_t>(1, projectState_.revision + 1);
    undoStack_.clear();
    redoStack_.clear();

    config_.projectName = projectState_.projectName;
    config_.sessionPath = projectState_.sessionPath;

    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;

    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::undoLastEdit()
{
    if (undoStack_.empty())
    {
        return false;
    }

    const ProjectAction action = undoStack_.back();
    undoStack_.pop_back();
    redoStack_.push_back(action);
    projectState_ = action.before;
    projectState_.dirty = true;
    ++projectState_.revision;
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::redoLastEdit()
{
    if (redoStack_.empty())
    {
        return false;
    }

    const ProjectAction action = redoStack_.back();
    redoStack_.pop_back();
    undoStack_.push_back(action);
    projectState_ = action.after;
    projectState_.dirty = true;
    ++projectState_.revision;
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::startTelemetry()
{
    if (telemetryRunning_.load())
    {
        return true;
    }

    telemetryRunning_.store(true);
    audioThreadState_.maintenanceRunning.store(true);
    maintenanceThread_ = std::thread(&AudioEngine::maintenanceThreadMain, this);
    return true;
}

void AudioEngine::stopTelemetry()
{
    telemetryRunning_.store(false);
    audioThreadState_.maintenanceRunning.store(false);

    if (maintenanceThread_.joinable())
    {
        maintenanceThread_.join();
    }
}

void AudioEngine::postCommand(const EngineCommand& command)
{
    if (!commandQueue_.push(command))
    {
        ++metrics_.commandQueueDrops;
    }
}

bool AudioEngine::consumeNextCommand(EngineCommand& outCommand)
{
    return commandQueue_.pop(outCommand);
}

void AudioEngine::setError(int errorCode, const std::string& message)
{
    lastErrorCode_ = errorCode;
    lastErrorMessage_ = message;
    state_.store(EngineState::Error);
    updateStatusFromState();
    publishSnapshot();
}

void AudioEngine::clearError()
{
    lastErrorCode_ = 0;
    lastErrorMessage_.clear();
}

void AudioEngine::resetMetrics()
{
    metrics_ = {};
}

void AudioEngine::updateStatusFromState()
{
    if (state_.load() != EngineState::Error && !initialized_.load())
    {
        state_.store(EngineState::Uninitialized);
    }
}

void AudioEngine::publishSnapshot()
{
    std::lock_guard<std::mutex> snapshotLock(snapshotWriteMutex_);
    const int currentIndex = activeSnapshotIndex_.load(std::memory_order_acquire);
    const int nextIndex = 1 - currentIndex;

    EngineSnapshot snapshot{};
    snapshot.engineState = state_.load();
    snapshot.config = config_;
    snapshot.metrics = metrics_;
    snapshot.transport = transportInfo_;
    snapshot.device = deviceState_;
    snapshot.project.state = projectState_;
    snapshot.project.undoDepth = static_cast<std::uint32_t>(undoStack_.size());
    snapshot.project.redoDepth = static_cast<std::uint32_t>(redoStack_.size());
    snapshot.project.graphVersion = metrics_.activeGraphVersion;
    snapshot.lastErrorCode = lastErrorCode_;
    snapshot.lastErrorMessage = lastErrorMessage_;

    {
        std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
        snapshot.metrics.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
    }

    std::ostringstream status;
    status
        << "Audio Engine: " << toString(snapshot.engineState)
        << " | Backend: " << snapshot.device.backendName
        << " | Device: " << snapshot.device.deviceName
        << " | SR: " << snapshot.device.sampleRate
        << " | Block: " << snapshot.device.blockSize
        << " | Graph v" << snapshot.metrics.activeGraphVersion
        << " | Project: " << snapshot.project.state.projectName;
    snapshot.statusText = status.str();

    uiSnapshots_[nextIndex] = std::move(snapshot);
    activeSnapshotIndex_.store(nextIndex, std::memory_order_release);
}

bool AudioEngine::initializeProjectState()
{
    projectState_ = {};
    projectState_.projectName = config_.projectName;
    projectState_.sessionPath = config_.sessionPath;
    projectState_.revision = 1;
    projectState_.dirty = false;

    BusState mainBus{};
    mainBus.busId = 1;
    mainBus.name = "Main Bus";

    TrackState track{};
    track.trackId = 1;
    track.busId = mainBus.busId;
    track.name = "Track 1";

    ClipState clip{};
    clip.clipId = 1;
    clip.trackId = track.trackId;
    clip.name = "Demo Clip";
    clip.sourceType = ClipSourceType::GeneratedTone;
    clip.startTimeSeconds = 0.0;
    clip.durationSeconds = 8.0;
    clip.gain = 0.7;

    track.clipIds.push_back(clip.clipId);
    mainBus.inputTrackIds.push_back(track.trackId);

    projectState_.buses.push_back(mainBus);
    projectState_.tracks.push_back(track);
    projectState_.clips.push_back(clip);
    projectState_.markers.push_back(MarkerState{1, "Start", 0.0});
    return true;
}

AudioEngine::GraphSnapshot AudioEngine::buildGraphFromProjectState() const
{
    GraphSnapshot graph{};
    graph.version = std::max<std::uint64_t>(1, projectState_.revision);

    graph.nodes.push_back(GraphNode{
        kInputNodeId,
        NodeType::Input,
        "Hardware Input",
        0,
        0,
        0,
        0,
        1.0,
        true,
        false,
        false,
        true});

    for (const auto& bus : projectState_.buses)
    {
        graph.nodes.push_back(GraphNode{
            busNodeId(bus.busId),
            NodeType::Bus,
            bus.name,
            0,
            0,
            bus.busId,
            0,
            1.0,
            false,
            true,
            false,
            true});
    }

    for (const auto& track : projectState_.tracks)
    {
        const bool liveTrack = track.armed;
        graph.nodes.push_back(GraphNode{
            trackNodeId(track.trackId),
            NodeType::Track,
            track.name,
            0,
            track.trackId,
            track.busId,
            0,
            1.0,
            liveTrack,
            !liveTrack,
            track.muted,
            true});

        graph.nodes.push_back(GraphNode{
            pluginNodeId(track.trackId),
            NodeType::Plugin,
            track.name + " FX",
            32,
            track.trackId,
            track.busId,
            0,
            1.0,
            liveTrack,
            !liveTrack,
            false,
            true});

        graph.edges.push_back(GraphEdge{kInputNodeId, trackNodeId(track.trackId)});
        graph.edges.push_back(GraphEdge{trackNodeId(track.trackId), pluginNodeId(track.trackId)});
        graph.edges.push_back(GraphEdge{pluginNodeId(track.trackId), busNodeId(track.busId)});
    }

    graph.nodes.push_back(GraphNode{
        kOutputNodeId,
        NodeType::Output,
        "Hardware Output",
        0,
        0,
        0,
        0,
        1.0,
        true,
        true,
        false,
        true});

    for (const auto& bus : projectState_.buses)
    {
        graph.edges.push_back(GraphEdge{busNodeId(bus.busId), kOutputNodeId});
    }

    return graph;
}

std::optional<AudioEngine::GraphNode> AudioEngine::findGraphNode(std::uint32_t nodeId, const GraphSnapshot& graph) const
{
    const auto it = std::find_if(
        graph.nodes.begin(),
        graph.nodes.end(),
        [&](const GraphNode& node) { return node.id == nodeId; });

    if (it == graph.nodes.end())
    {
        return std::nullopt;
    }

    return *it;
}

const AudioEngine::CompiledNode* AudioEngine::findCompiledNode(std::uint32_t nodeId) const
{
    const auto it = compiledGraph_.nodeLookup.find(nodeId);
    if (it == compiledGraph_.nodeLookup.end())
    {
        return nullptr;
    }

    return &compiledGraph_.executionPlan[it->second];
}

const AudioEngine::AutomationLane* AudioEngine::findAutomationLane(std::uint32_t nodeId) const
{
    const auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == nodeId; });

    return it == automationLanes_.end() ? nullptr : &(*it);
}

AudioEngine::AutomationLane* AudioEngine::findAutomationLane(std::uint32_t nodeId)
{
    const auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == nodeId; });

    return it == automationLanes_.end() ? nullptr : &(*it);
}

double AudioEngine::getNodeAutomationValue(std::uint32_t nodeId, std::uint32_t sampleOffset) const
{
    const AutomationLane* lane = findAutomationLane(nodeId);
    if (lane == nullptr)
    {
        return 1.0;
    }

    for (const auto& segment : lane->activeSegments)
    {
        if (segment.parameterId != kDefaultAutomationParameter)
        {
            continue;
        }

        if (sampleOffset < segment.startSample || sampleOffset > segment.endSample)
        {
            continue;
        }

        const std::uint32_t segmentLength = std::max<std::uint32_t>(1, segment.endSample - segment.startSample);
        const double position = static_cast<double>(sampleOffset - segment.startSample) /
            static_cast<double>(segmentLength);
        return interpolateValue(segment.startValue, segment.endValue, position, segment.mode);
    }

    if (!lane->parameterStates.empty())
    {
        return lane->parameterStates.front().currentValue;
    }

    return 1.0;
}

bool AudioEngine::openWasapiBackend()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE)
    {
        backendComInitialized_ = true;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));

    if (FAILED(hr) || enumerator == nullptr)
    {
        setError(kErrorBackendInit, "Failed to create IMMDeviceEnumerator.");
        return false;
    }

    IMMDevice* device = nullptr;
    std::string resolvedDeviceName = "Default WASAPI Device";

    if (!config_.preferredDeviceName.empty())
    {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);

        if (SUCCEEDED(hr) && collection != nullptr)
        {
            UINT count = 0;
            collection->GetCount(&count);

            const std::string wantedName = toLowerCopy(config_.preferredDeviceName);
            for (UINT index = 0; index < count && device == nullptr; ++index)
            {
                IMMDevice* candidate = nullptr;
                if (FAILED(collection->Item(index, &candidate)) || candidate == nullptr)
                {
                    continue;
                }

                IPropertyStore* propertyStore = nullptr;
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);

                if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &propertyStore)) &&
                    propertyStore != nullptr &&
                    SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName)) &&
                    friendlyName.vt == VT_LPWSTR)
                {
                    char convertedName[512]{};
                    WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        friendlyName.pwszVal,
                        -1,
                        convertedName,
                        static_cast<int>(sizeof(convertedName)),
                        nullptr,
                        nullptr);

                    const std::string candidateName = convertedName;
                    if (toLowerCopy(candidateName).find(wantedName) != std::string::npos)
                    {
                        device = candidate;
                        resolvedDeviceName = candidateName;
                        candidate = nullptr;
                    }
                }

                PropVariantClear(&friendlyName);
                safeRelease(propertyStore);
                safeRelease(candidate);
            }

            safeRelease(collection);
        }
    }

    if (device == nullptr)
    {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || device == nullptr)
        {
            safeRelease(enumerator);
            setError(kErrorBackendInit, "Failed to resolve the default WASAPI render device.");
            return false;
        }
    }

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr) || audioClient == nullptr)
    {
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to activate IAudioClient.");
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr)
    {
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to query WASAPI mix format.");
        return false;
    }

    REFERENCE_TIME requestedDuration = static_cast<REFERENCE_TIME>(
        (10'000'000.0 * static_cast<double>(config_.preferredBlockSize)) /
        static_cast<double>(std::max<DWORD>(1u, mixFormat->nSamplesPerSec)));

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        requestedDuration,
        0,
        mixFormat,
        nullptr);

    if (FAILED(hr))
    {
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to initialize WASAPI shared-mode stream.");
        return false;
    }

    HANDLE bufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (bufferEvent == nullptr)
    {
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to create WASAPI event handle.");
        return false;
    }

    hr = audioClient->SetEventHandle(bufferEvent);
    if (FAILED(hr))
    {
        CloseHandle(bufferEvent);
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to bind the WASAPI event handle.");
        return false;
    }

    UINT32 bufferFrames = 0;
    audioClient->GetBufferSize(&bufferFrames);

    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr) || renderClient == nullptr)
    {
        CloseHandle(bufferEvent);
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to acquire IAudioRenderClient.");
        return false;
    }

    deviceState_.backendType = BackendType::Wasapi;
    deviceState_.backendName = "WASAPI Shared/Event";
    deviceState_.deviceName = resolvedDeviceName;
    deviceState_.sampleRate = mixFormat->nSamplesPerSec;
    deviceState_.blockSize = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(config_.preferredBlockSize),
        std::max<std::uint32_t>(1, bufferFrames));
    deviceState_.deviceBufferFrames = bufferFrames;
    deviceState_.inputChannels = static_cast<std::uint32_t>(config_.inputChannelCount);
    deviceState_.outputChannels = mixFormat->nChannels;
    deviceState_.sharedMode = true;
    deviceState_.eventDriven = true;
    deviceState_.usingPreferredFormat = mixFormat->nSamplesPerSec == static_cast<std::uint32_t>(config_.preferredSampleRate);
    deviceState_.format.sampleRate = mixFormat->nSamplesPerSec;
    deviceState_.format.channelCount = mixFormat->nChannels;
    deviceState_.format.bitsPerSample = mixFormat->wBitsPerSample;
    deviceState_.format.floatingPoint = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    deviceState_.format.extensible = mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    deviceState_.mixFormatBlob.assign(
        reinterpret_cast<const std::uint8_t*>(mixFormat),
        reinterpret_cast<const std::uint8_t*>(mixFormat) + (sizeof(WAVEFORMATEX) + mixFormat->cbSize));
    deviceState_.eventHandle = reinterpret_cast<std::uintptr_t>(bufferEvent);
    deviceState_.enumeratorHandle = reinterpret_cast<std::uintptr_t>(enumerator);
    deviceState_.deviceHandle = reinterpret_cast<std::uintptr_t>(device);
    deviceState_.audioClientHandle = reinterpret_cast<std::uintptr_t>(audioClient);
    deviceState_.renderClientHandle = reinterpret_cast<std::uintptr_t>(renderClient);
    deviceState_.captureClientHandle = 0;
    deviceState_.initialized = true;
    deviceState_.started = false;

    realtimeBuffer_.resize(deviceState_.blockSize);
    anticipativeBuffer_.resize(deviceState_.blockSize);

    CoTaskMemFree(mixFormat);
    return true;
#else
    return false;
#endif
}

void AudioEngine::closeAudioBackend()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    safeCloseHandle(deviceState_.eventHandle);

    if (deviceState_.renderClientHandle != 0)
    {
        IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
        safeRelease(renderClient);
        deviceState_.renderClientHandle = 0;
    }

    if (deviceState_.captureClientHandle != 0)
    {
        IAudioCaptureClient* captureClient = reinterpret_cast<IAudioCaptureClient*>(deviceState_.captureClientHandle);
        safeRelease(captureClient);
        deviceState_.captureClientHandle = 0;
    }

    if (deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        safeRelease(audioClient);
        deviceState_.audioClientHandle = 0;
    }

    if (deviceState_.deviceHandle != 0)
    {
        IMMDevice* device = reinterpret_cast<IMMDevice*>(deviceState_.deviceHandle);
        safeRelease(device);
        deviceState_.deviceHandle = 0;
    }

    if (deviceState_.enumeratorHandle != 0)
    {
        IMMDeviceEnumerator* enumerator = reinterpret_cast<IMMDeviceEnumerator*>(deviceState_.enumeratorHandle);
        safeRelease(enumerator);
        deviceState_.enumeratorHandle = 0;
    }
#endif

    if (backendComInitialized_)
    {
        CoUninitialize();
        backendComInitialized_ = false;
    }

    deviceState_.initialized = false;
    deviceState_.started = false;
    deviceState_.mixFormatBlob.clear();
}

bool AudioEngine::startAudioStream()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (deviceState_.backendType == BackendType::Wasapi && deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
        if (audioClient == nullptr || renderClient == nullptr)
        {
            setError(kErrorBackendInit, "WASAPI client is not ready.");
            return false;
        }

        UINT32 bufferFrames = 0;
        audioClient->GetBufferSize(&bufferFrames);

        BYTE* data = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &data)) && data != nullptr)
        {
            std::memset(data, 0, static_cast<std::size_t>(bufferFrames) * deviceState_.format.channelCount * (deviceState_.format.bitsPerSample / 8));
            renderClient->ReleaseBuffer(bufferFrames, 0);
        }

        const HRESULT hr = audioClient->Start();
        if (FAILED(hr))
        {
            setError(kErrorBackendInit, "Failed to start the WASAPI stream.");
            return false;
        }
    }
#endif

    deviceState_.started = true;
    return true;
}

void AudioEngine::stopAudioStream()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        if (audioClient != nullptr)
        {
            audioClient->Stop();
            audioClient->Reset();
        }
    }
#endif

    deviceState_.started = false;
}

bool AudioEngine::waitForNextAudioBuffer(std::uint32_t& outFramesToRender)
{
    outFramesToRender = deviceState_.blockSize;

    if (deviceState_.backendType == BackendType::Dummy)
    {
        const auto sleepDuration = std::chrono::microseconds(
            static_cast<long long>(
                (static_cast<double>(deviceState_.blockSize) / static_cast<double>(std::max(1u, deviceState_.sampleRate))) *
                1'000'000.0));
        std::this_thread::sleep_for(sleepDuration);
        return true;
    }

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    HANDLE bufferEvent = reinterpret_cast<HANDLE>(deviceState_.eventHandle);
    if (bufferEvent == nullptr)
    {
        setError(kErrorAudioRuntime, "Missing WASAPI event handle.");
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(bufferEvent, kBackendWaitTimeoutMs);
    if (waitResult == WAIT_TIMEOUT)
    {
        deviceState_.recovery.deviceLost = true;
        deviceState_.recovery.restartPending = true;
        deviceState_.recovery.lastFailure = "WASAPI event wait timed out.";
        setError(kErrorAudioRuntime, deviceState_.recovery.lastFailure);
        return false;
    }

    if (waitResult != WAIT_OBJECT_0)
    {
        deviceState_.recovery.deviceLost = true;
        deviceState_.recovery.restartPending = true;
        deviceState_.recovery.lastFailure = "WASAPI event wait failed.";
        setError(kErrorAudioRuntime, deviceState_.recovery.lastFailure);
        return false;
    }

    IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
    if (audioClient == nullptr)
    {
        setError(kErrorAudioRuntime, "IAudioClient is not available.");
        return false;
    }

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
    {
        setError(kErrorAudioRuntime, "Failed to query current WASAPI padding.");
        return false;
    }

    const UINT32 availableFrames = deviceState_.deviceBufferFrames > padding
        ? deviceState_.deviceBufferFrames - padding
        : 0;

    outFramesToRender = std::min<std::uint32_t>(
        std::max<std::uint32_t>(1, availableFrames),
        std::max<std::uint32_t>(1, deviceState_.blockSize));
    return true;
#else
    return true;
#endif
}

bool AudioEngine::writeBufferToBackend(const AudioBuffer& buffer)
{
    if (deviceState_.backendType == BackendType::Dummy)
    {
        return true;
    }

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
    IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
    if (audioClient == nullptr || renderClient == nullptr)
    {
        setError(kErrorAudioRuntime, "WASAPI render path is not available.");
        return false;
    }

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
    {
        setError(kErrorAudioRuntime, "Failed to query padding before writing audio.");
        return false;
    }

    const UINT32 availableFrames = deviceState_.deviceBufferFrames > padding
        ? deviceState_.deviceBufferFrames - padding
        : 0;
    const UINT32 framesToWrite = std::min<UINT32>(buffer.frameCount, std::max<UINT32>(1, availableFrames));

    BYTE* rawBuffer = nullptr;
    const HRESULT hr = renderClient->GetBuffer(framesToWrite, &rawBuffer);
    if (FAILED(hr) || rawBuffer == nullptr)
    {
        setError(kErrorAudioRuntime, "Failed to lock the WASAPI render buffer.");
        return false;
    }

    const std::uint32_t channelCount = std::max<std::uint32_t>(1, deviceState_.format.channelCount);
    const std::uint32_t bitsPerSample = std::max<std::uint32_t>(8, deviceState_.format.bitsPerSample);

    if (deviceState_.format.floatingPoint && bitsPerSample == 32)
    {
        float* out = reinterpret_cast<float*>(rawBuffer);
        for (UINT32 frame = 0; frame < framesToWrite; ++frame)
        {
            const float left = static_cast<float>(clampSample(buffer.left[frame]));
            const float right = static_cast<float>(clampSample(buffer.right[frame]));
            out[(frame * channelCount) + 0] = left;
            if (channelCount > 1)
            {
                out[(frame * channelCount) + 1] = right;
            }
            for (std::uint32_t channel = 2; channel < channelCount; ++channel)
            {
                out[(frame * channelCount) + channel] = 0.0f;
            }
        }
    }
    else if (bitsPerSample == 16)
    {
        auto* out = reinterpret_cast<std::int16_t*>(rawBuffer);
        for (UINT32 frame = 0; frame < framesToWrite; ++frame)
        {
            const std::int16_t left = static_cast<std::int16_t>(clampSample(buffer.left[frame]) * 32767.0);
            const std::int16_t right = static_cast<std::int16_t>(clampSample(buffer.right[frame]) * 32767.0);
            out[(frame * channelCount) + 0] = left;
            if (channelCount > 1)
            {
                out[(frame * channelCount) + 1] = right;
            }
            for (std::uint32_t channel = 2; channel < channelCount; ++channel)
            {
                out[(frame * channelCount) + channel] = 0;
            }
        }
    }
    else
    {
        std::memset(rawBuffer, 0, static_cast<std::size_t>(framesToWrite) * channelCount * (bitsPerSample / 8));
    }

    renderClient->ReleaseBuffer(framesToWrite, 0);
    return true;
#else
    return false;
#endif
}

void AudioEngine::audioThreadMain()
{
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audioThreadState_.comInitialized = SUCCEEDED(comHr) || comHr == S_FALSE;

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    audioThreadState_.mmcssRegistered = mmcssHandle != nullptr;
#endif

    if (!startAudioStream())
    {
        running_.store(false);
        audioThreadState_.stopRequested.store(true);
        return;
    }

    audioThreadState_.running.store(true);
    state_.store(EngineState::Running);
    updateStatusFromState();
    publishSnapshot();

    while (!audioThreadState_.stopRequested.load() && running_.load())
    {
        std::uint32_t framesToRender = deviceState_.blockSize;
        if (!waitForNextAudioBuffer(framesToRender))
        {
            if (audioThreadState_.stopRequested.load())
            {
                break;
            }

            if (!recoverAudioDevice())
            {
                break;
            }
            continue;
        }

        if (framesToRender == 0)
        {
            continue;
        }

        realtimeBuffer_.resize(framesToRender);
        anticipativeBuffer_.resize(framesToRender);

        RenderContext renderContext{};
        renderContext.callback.sampleRate = deviceState_.sampleRate;
        renderContext.callback.blockSize = framesToRender;
        renderContext.callback.deviceBufferFrames = deviceState_.deviceBufferFrames;
        renderContext.callback.callbackIndex = metrics_.callbackCount;
        renderContext.callback.transportSampleStart = transportInfo_.samplePosition;
        renderContext.callback.blockStartTimeSeconds = transportInfo_.timelineSeconds;
        renderContext.isOffline = false;
        renderContext.isLivePath = true;
        renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;

        const auto blockStart = Clock::now();

        processPendingCommands();

        if (graphSwapPending_)
        {
            swapCompiledGraphAtSafePoint();
        }

        applyAutomationForBlock(renderContext);
        processLiveBlock(renderContext, realtimeBuffer_);
        processAnticipativeWork(renderContext);
        mergeLiveAndAnticipativeResults(renderContext, realtimeBuffer_);
        alignBranchesForMix(realtimeBuffer_);
        queueClipReadAhead(renderContext);

        if (!writeBufferToBackend(realtimeBuffer_))
        {
            if (!recoverAudioDevice())
            {
                break;
            }
            continue;
        }

        if (transportInfo_.state == TransportState::Playing)
        {
            transportInfo_.samplePosition += framesToRender;
            transportInfo_.timelineSeconds =
                static_cast<double>(transportInfo_.samplePosition) /
                static_cast<double>(std::max(1u, deviceState_.sampleRate));
        }

        const auto blockEnd = Clock::now();
        const double elapsedUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(blockEnd - blockStart).count());
        const double deadlineUs =
            (static_cast<double>(framesToRender) / static_cast<double>(std::max(1u, deviceState_.sampleRate))) * 1'000'000.0;

        ++metrics_.callbackCount;
        metrics_.averageBlockTimeUs =
            metrics_.callbackCount == 1
            ? elapsedUs
            : ((metrics_.averageBlockTimeUs * static_cast<double>(metrics_.callbackCount - 1)) + elapsedUs) /
              static_cast<double>(metrics_.callbackCount);
        metrics_.peakBlockTimeUs = std::max(metrics_.peakBlockTimeUs, elapsedUs);
        metrics_.cpuLoadApprox = std::min(1.0, elapsedUs / std::max(1.0, deadlineUs));

        if (elapsedUs > deadlineUs)
        {
            ++metrics_.deadlineMisses;
            ++metrics_.xruns;
        }

        audioThreadState_.lastCallbackIndex = metrics_.callbackCount;
        audioThreadState_.lastBlockDurationUs = elapsedUs;
        publishSnapshot();
    }

    stopAudioStream();
    audioThreadState_.running.store(false);

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (audioThreadState_.mmcssRegistered)
    {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
#endif

    if (audioThreadState_.comInitialized)
    {
        CoUninitialize();
        audioThreadState_.comInitialized = false;
    }
}

void AudioEngine::maintenanceThreadMain()
{
    while (telemetryRunning_.load())
    {
        serviceSandboxWatchdogs();
        serviceClipCacheEviction();
        std::this_thread::sleep_for(std::chrono::milliseconds(kMaintenanceTickMs));
    }
}

void AudioEngine::scheduleAnticipativeBlock(std::uint64_t liveBlockIndex)
{
    if (!config_.enableAnticipativeProcessing)
    {
        return;
    }

    const std::uint64_t targetBlock = liveBlockIndex + static_cast<std::uint64_t>(std::max(1, config_.anticipativePrefetchBlocks));

    {
        std::lock_guard<std::mutex> lock(anticipativeMutex_);
        if (targetBlock <= requestedAnticipativeBlock_)
        {
            return;
        }

        requestedAnticipativeBlock_ = targetBlock;
        anticipativeWorkPending_ = true;
    }

    anticipativeCv_.notify_one();
}

void AudioEngine::computeAnticipativeBlock(std::uint64_t targetBlockIndex)
{
    RenderContext renderContext{};
    renderContext.isOffline = false;
    renderContext.isLivePath = false;
    renderContext.isAnticipativePass = true;
    renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;
    renderContext.futureBlockOffset = static_cast<std::uint32_t>(std::max(1, config_.anticipativePrefetchBlocks));
    renderContext.callback.sampleRate = deviceState_.sampleRate;
    renderContext.callback.blockSize = deviceState_.blockSize;
    renderContext.callback.callbackIndex = targetBlockIndex;
    renderContext.callback.transportSampleStart =
        transportInfo_.samplePosition + (targetBlockIndex * static_cast<std::uint64_t>(deviceState_.blockSize));

    AudioBuffer blockBuffer{};
    blockBuffer.resize(deviceState_.blockSize);
    blockBuffer.clear();

    renderGraphBlock(renderContext, blockBuffer, true);
    alignBranchesForMix(blockBuffer);

    {
        std::lock_guard<std::mutex> lock(anticipativeMutex_);
        anticipativeResult_.blockIndex = targetBlockIndex;
        anticipativeResult_.graphVersion = compiledGraph_.sourceVersion;
        anticipativeResult_.valid = true;
        anticipativeResult_.buffer = std::move(blockBuffer);
        completedAnticipativeBlock_ = targetBlockIndex;
    }
}

bool AudioEngine::consumeAnticipativeBlock(std::uint64_t blockIndex, AudioBuffer& outBuffer)
{
    std::lock_guard<std::mutex> lock(anticipativeMutex_);
    if (!anticipativeResult_.valid || anticipativeResult_.blockIndex != blockIndex)
    {
        return false;
    }

    outBuffer = anticipativeResult_.buffer;
    anticipativeResult_.valid = false;
    return true;
}

bool AudioEngine::renderGraphBlock(RenderContext& renderContext, AudioBuffer& outputBuffer, bool includeAnticipativeNodes)
{
    outputBuffer.resize(renderContext.callback.blockSize);
    outputBuffer.clear();

    if (!compiledGraph_.valid)
    {
        return false;
    }

    std::unordered_map<std::uint32_t, AudioBuffer> nodeOutputs;
    const GraphSnapshot& graph = editableGraph_;

    for (const auto& stage : compiledGraph_.stages)
    {
        for (const std::uint32_t nodeId : stage.nodeIds)
        {
            const CompiledNode* compiledNode = findCompiledNode(nodeId);
            if (compiledNode == nullptr)
            {
                continue;
            }

            const bool processThisPass = includeAnticipativeNodes
                ? compiledNode->anticipativeLane
                : compiledNode->liveLane;

            if (!processThisPass)
            {
                continue;
            }

            const auto graphNodeOpt = findGraphNode(nodeId, graph);
            if (!graphNodeOpt.has_value())
            {
                continue;
            }

            const GraphNode graphNode = graphNodeOpt.value();
            AudioBuffer nodeBuffer{};
            nodeBuffer.resize(outputBuffer.frameCount);
            nodeBuffer.clear();

            for (const std::uint32_t upstreamId : compiledNode->upstreamNodeIds)
            {
                const auto upstreamIt = nodeOutputs.find(upstreamId);
                if (upstreamIt != nodeOutputs.end())
                {
                    mixBuffer(nodeBuffer, upstreamIt->second);
                }
            }

            if (graphNode.type == NodeType::Input && !includeAnticipativeNodes)
            {
                for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                {
                    const double phase = static_cast<double>(
                        renderContext.callback.transportSampleStart + frame) /
                        static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
                    const double inputSignal = transportInfo_.monitoringEnabled ? (0.05 * std::sin(kTwoPi * 220.0 * phase)) : 0.0;
                    nodeBuffer.left[frame] += inputSignal;
                    nodeBuffer.right[frame] += inputSignal;
                }
            }

            if (graphNode.type == NodeType::Track || graphNode.type == NodeType::Instrument)
            {
                renderTrackClips(graphNode.trackId, renderContext, nodeBuffer);
            }

            const double nodeGain = graphNode.baseGain;

            for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
            {
                const double automationValue = getNodeAutomationValue(nodeId, frame);
                const double totalGain = nodeGain * automationValue;
                nodeBuffer.left[frame] *= totalGain;
                nodeBuffer.right[frame] *= totalGain;
            }

            if (graphNode.type == NodeType::Plugin)
            {
                bool bypassPlugin = graphNode.bypassed;

                std::lock_guard<std::mutex> pluginLock(pluginMutex_);
                const auto pluginIt = std::find_if(
                    loadedPlugins_.begin(),
                    loadedPlugins_.end(),
                    [&](const PluginInstance& plugin) { return plugin.ownerNodeId == graphNode.id; });

                if (pluginIt != loadedPlugins_.end())
                {
                    bypassPlugin = bypassPlugin || pluginIt->autoBypassed;

                    if (!bypassPlugin)
                    {
                        const double precisionGain = pluginIt->descriptor.supportsDoublePrecision ? 1.08 : 1.03;
                        for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                        {
                            nodeBuffer.left[frame] = std::tanh(nodeBuffer.left[frame] * precisionGain);
                            nodeBuffer.right[frame] = std::tanh(nodeBuffer.right[frame] * precisionGain);
                        }
                    }
                }
                else
                {
                    for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                    {
                        nodeBuffer.left[frame] = std::tanh(nodeBuffer.left[frame] * 1.02);
                        nodeBuffer.right[frame] = std::tanh(nodeBuffer.right[frame] * 1.02);
                    }
                }
            }

            applyNodeDelayCompensation(nodeId, nodeBuffer);
            nodeOutputs[nodeId] = std::move(nodeBuffer);
        }
    }

    for (const auto& compiledNode : compiledGraph_.executionPlan)
    {
        if (compiledNode.type != NodeType::Output && compiledNode.type != NodeType::RenderSink)
        {
            continue;
        }

        const auto nodeIt = nodeOutputs.find(compiledNode.nodeId);
        if (nodeIt != nodeOutputs.end())
        {
            mixBuffer(outputBuffer, nodeIt->second);
        }
    }

    if (outputBuffer.frameCount == 0 && !nodeOutputs.empty())
    {
        outputBuffer.resize(renderContext.callback.blockSize);
        outputBuffer.clear();
        for (const auto& [_, buffer] : nodeOutputs)
        {
            mixBuffer(outputBuffer, buffer);
        }
    }

    return true;
}

void AudioEngine::renderTrackClips(std::uint32_t trackId, const RenderContext& renderContext, AudioBuffer& destination)
{
    const auto trackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == trackId; });

    if (trackIt == projectState_.tracks.end() || trackIt->muted)
    {
        return;
    }

    const std::uint64_t blockStartSample = renderContext.callback.transportSampleStart;
    const double sampleRate = static_cast<double>(std::max(1u, renderContext.callback.sampleRate));

    for (const std::uint32_t clipId : trackIt->clipIds)
    {
        const auto clipIt = std::find_if(
            projectState_.clips.begin(),
            projectState_.clips.end(),
            [&](const ClipState& clip) { return clip.clipId == clipId; });

        if (clipIt == projectState_.clips.end() || clipIt->muted)
        {
            continue;
        }

        const std::uint64_t clipStartSample = static_cast<std::uint64_t>(clipIt->startTimeSeconds * sampleRate);
        const std::uint64_t clipEndSample = clipStartSample + static_cast<std::uint64_t>(clipIt->durationSeconds * sampleRate);

        for (std::uint32_t frame = 0; frame < destination.frameCount; ++frame)
        {
            const std::uint64_t projectSample = blockStartSample + frame;
            if (projectSample < clipStartSample || projectSample >= clipEndSample)
            {
                continue;
            }

            double sampleValue = 0.0;

            if (clipIt->sourceType == ClipSourceType::GeneratedTone)
            {
                const double localTime = static_cast<double>(projectSample - clipStartSample) / sampleRate;
                sampleValue = std::sin(kTwoPi * 110.0 * localTime) * clipIt->gain;
            }
            else
            {
                std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
                const auto cacheIt = std::find_if(
                    clipCache_.begin(),
                    clipCache_.end(),
                    [&](const ClipCacheEntry& entry)
                    {
                        return entry.clipId == clipIt->clipId && entry.complete;
                    });

                if (cacheIt != clipCache_.end())
                {
                    const std::size_t clipSampleIndex = static_cast<std::size_t>(projectSample - clipStartSample);
                    if (clipSampleIndex < cacheIt->left.size() && clipSampleIndex < cacheIt->right.size())
                    {
                        destination.left[frame] += cacheIt->left[clipSampleIndex] * clipIt->gain;
                        destination.right[frame] += cacheIt->right[clipSampleIndex] * clipIt->gain;
                        continue;
                    }
                }
            }

            destination.left[frame] += sampleValue;
            destination.right[frame] += sampleValue;
        }
    }
}

void AudioEngine::applyNodeDelayCompensation(std::uint32_t nodeId, AudioBuffer& buffer)
{
    if (!config_.enablePdc)
    {
        return;
    }

    auto delayIt = delayLines_.find(nodeId);
    if (delayIt == delayLines_.end())
    {
        return;
    }

    DelayLine& delayLine = delayIt->second;
    if (delayLine.delaySamples == 0 || delayLine.left.empty())
    {
        return;
    }

    const std::uint32_t capacity = static_cast<std::uint32_t>(delayLine.left.size());
    for (std::uint32_t frame = 0; frame < buffer.frameCount; ++frame)
    {
        const std::uint32_t readIndex =
            (delayLine.writeIndex + capacity - delayLine.delaySamples) % capacity;

        const double delayedLeft = delayLine.left[readIndex];
        const double delayedRight = delayLine.right[readIndex];

        delayLine.left[delayLine.writeIndex] = buffer.left[frame];
        delayLine.right[delayLine.writeIndex] = buffer.right[frame];
        delayLine.writeIndex = (delayLine.writeIndex + 1) % capacity;

        buffer.left[frame] = delayedLeft;
        buffer.right[frame] = delayedRight;
    }
}

void AudioEngine::mixBuffer(AudioBuffer& destination, const AudioBuffer& source, double gain)
{
    if (destination.frameCount == 0)
    {
        destination.resize(source.frameCount);
    }

    const std::uint32_t frameCount = std::min(destination.frameCount, source.frameCount);
    for (std::uint32_t frame = 0; frame < frameCount; ++frame)
    {
        destination.left[frame] += source.left[frame] * gain;
        destination.right[frame] += source.right[frame] * gain;
    }
}

void AudioEngine::mixTrackToBus(AudioBuffer& destination, std::uint32_t trackId, const RenderContext& renderContext)
{
    renderTrackClips(trackId, renderContext, destination);
}

void AudioEngine::queueClipReadAhead(const RenderContext& renderContext)
{
    const double blockStartSeconds =
        static_cast<double>(renderContext.callback.transportSampleStart) /
        static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double lookAheadSeconds =
        blockStartSeconds +
        (static_cast<double>(renderContext.callback.blockSize * std::max(1, config_.anticipativePrefetchBlocks)) /
         static_cast<double>(std::max(1u, renderContext.callback.sampleRate)));

    for (const auto& clip : projectState_.clips)
    {
        if (clip.sourceType != ClipSourceType::AudioFile || clip.filePath.empty())
        {
            continue;
        }

        if (clip.startTimeSeconds > lookAheadSeconds ||
            (clip.startTimeSeconds + clip.durationSeconds) < blockStartSeconds)
        {
            continue;
        }

        bool alreadyCached = false;
        {
            std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
            alreadyCached = std::any_of(
                clipCache_.begin(),
                clipCache_.end(),
                [&](const ClipCacheEntry& entry)
                {
                    return entry.clipId == clip.clipId && entry.complete;
                });
        }

        if (alreadyCached)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> queueLock(diskQueueMutex_);
            const bool alreadyQueued = std::any_of(
                diskReadQueue_.begin(),
                diskReadQueue_.end(),
                [&](const DiskReadRequest& request)
                {
                    return request.clipId == clip.clipId;
                });

            if (!alreadyQueued)
            {
                diskReadQueue_.push_back(DiskReadRequest{
                    clip.clipId,
                    clip.filePath,
                    0,
                    static_cast<std::uint32_t>(clip.durationSeconds * static_cast<double>(renderContext.callback.sampleRate))});
            }
        }

        diskQueueCv_.notify_one();
    }
}

void AudioEngine::diskWorkerMain()
{
    while (diskWorkerRunning_.load())
    {
        DiskReadRequest request{};

        {
            std::unique_lock<std::mutex> lock(diskQueueMutex_);
            diskQueueCv_.wait(
                lock,
                [this]()
                {
                    return !diskWorkerRunning_.load() || !diskReadQueue_.empty();
                });

            if (!diskWorkerRunning_.load() && diskReadQueue_.empty())
            {
                return;
            }

            request = diskReadQueue_.front();
            diskReadQueue_.pop_front();
        }

        loadClipIntoCache(request);
    }
}

bool AudioEngine::loadClipIntoCache(const DiskReadRequest& request)
{
    if (request.filePath.empty())
    {
        return false;
    }

    DecodedWavData decoded{};
    if (!decodeWavFile(request.filePath, decoded))
    {
        return false;
    }

    ClipCacheEntry entry{};
    entry.clipId = request.clipId;
    entry.filePath = request.filePath;
    entry.cachedOffsetFrames = 0;
    entry.sampleRate = decoded.sampleRate;
    entry.left = std::move(decoded.left);
    entry.right = std::move(decoded.right);
    entry.complete = true;
    entry.lastAccess = Clock::now();

    std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
    auto existing = std::find_if(
        clipCache_.begin(),
        clipCache_.end(),
        [&](const ClipCacheEntry& candidate) { return candidate.clipId == request.clipId; });

    if (existing == clipCache_.end())
    {
        clipCache_.push_back(std::move(entry));
    }
    else
    {
        *existing = std::move(entry);
    }

    metrics_.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
    return true;
}

bool AudioEngine::writeWavFile(const std::string& path, std::uint32_t sampleRate, const AudioBuffer& buffer)
{
    std::filesystem::path outputPath = path;
    if (outputPath.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    const std::uint16_t channelCount = 2;
    const std::uint16_t bitsPerSample = 32;
    const std::uint32_t byteRate = sampleRate * channelCount * (bitsPerSample / 8);
    const std::uint16_t blockAlign = channelCount * (bitsPerSample / 8);
    const std::uint32_t dataSize = buffer.frameCount * blockAlign;
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint16_t formatTag = WAVE_FORMAT_IEEE_FLOAT;

    stream.write("RIFF", 4);
    stream.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);

    const std::uint32_t fmtSize = 16;
    stream.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    stream.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    stream.write(reinterpret_cast<const char*>(&channelCount), sizeof(channelCount));
    stream.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    stream.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    stream.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    stream.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    stream.write("data", 4);
    stream.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));

    for (std::uint32_t frame = 0; frame < buffer.frameCount; ++frame)
    {
        const float left = static_cast<float>(clampSample(buffer.left[frame]));
        const float right = static_cast<float>(clampSample(buffer.right[frame]));
        stream.write(reinterpret_cast<const char*>(&left), sizeof(left));
        stream.write(reinterpret_cast<const char*>(&right), sizeof(right));
    }

    return stream.good();
}

bool AudioEngine::spawnPluginSandbox(PluginInstance& instance)
{
    if (!config_.enablePluginSandbox || config_.safeMode)
    {
        return false;
    }

    char currentExePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, currentExePath, MAX_PATH);

    std::string pluginHostPath = currentExePath;
    std::filesystem::path siblingHost = std::filesystem::path(currentExePath).parent_path() / "DAWCloudPluginHost.exe";
    if (std::filesystem::exists(siblingHost))
    {
        pluginHostPath = siblingHost.string();
    }

    const std::string mappingName =
        "DAWCloudSandbox_" +
        std::to_string(instance.instanceId) +
        "_" +
        std::to_string(GetTickCount64());

    HANDLE mappingHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SandboxSharedState),
        mappingName.c_str());

    if (mappingHandle == nullptr)
    {
        return false;
    }

    void* mappingView = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxSharedState));
    if (mappingView == nullptr)
    {
        CloseHandle(mappingHandle);
        return false;
    }

    auto* sharedState = reinterpret_cast<SandboxSharedState*>(mappingView);
    *sharedState = SandboxSharedState{};
    UnmapViewOfFile(mappingView);

    std::string commandLine =
        quoteArgument(pluginHostPath) +
        " --plugin-host --mapping=" + quoteArgument(mappingName) +
        " --plugin=" + quoteArgument(instance.descriptor.name);

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    const BOOL created = CreateProcessA(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created)
    {
        CloseHandle(mappingHandle);
        return false;
    }

    instance.sandbox.enabled = true;
    instance.sandbox.alive = true;
    instance.sandbox.executablePath = pluginHostPath;
    instance.sandbox.sharedMemoryName = mappingName;
    instance.sandbox.mappingHandle = reinterpret_cast<std::uintptr_t>(mappingHandle);
    instance.sandbox.processHandle = reinterpret_cast<std::uintptr_t>(processInfo.hProcess);
    instance.sandbox.threadHandle = reinterpret_cast<std::uintptr_t>(processInfo.hThread);
    instance.sandbox.lastHeartbeatTime = Clock::now();
    return true;
}

void AudioEngine::shutdownPluginSandbox(PluginInstance& instance)
{
    if (!instance.sandbox.enabled)
    {
        return;
    }

    if (instance.sandbox.mappingHandle != 0)
    {
        HANDLE mappingHandle = reinterpret_cast<HANDLE>(instance.sandbox.mappingHandle);
        void* view = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxSharedState));
        if (view != nullptr)
        {
            auto* shared = reinterpret_cast<SandboxSharedState*>(view);
            shared->stopRequested = 1;
            UnmapViewOfFile(view);
        }
    }

    if (instance.sandbox.processHandle != 0)
    {
        HANDLE processHandle = reinterpret_cast<HANDLE>(instance.sandbox.processHandle);
        WaitForSingleObject(processHandle, 1000);
        safeCloseHandle(instance.sandbox.processHandle);
    }

    safeCloseHandle(instance.sandbox.threadHandle);
    safeCloseHandle(instance.sandbox.mappingHandle);
    instance.sandbox.enabled = false;
    instance.sandbox.alive = false;
}

void AudioEngine::serviceSandboxWatchdogs()
{
    std::lock_guard<std::mutex> pluginLock(pluginMutex_);
    for (auto& plugin : loadedPlugins_)
    {
        if (!plugin.sandbox.enabled || plugin.sandbox.mappingHandle == 0)
        {
            continue;
        }

        HANDLE processHandle = reinterpret_cast<HANDLE>(plugin.sandbox.processHandle);
        if (processHandle != nullptr && WaitForSingleObject(processHandle, 0) == WAIT_OBJECT_0)
        {
            plugin.active = false;
            plugin.autoBypassed = true;
            plugin.sandbox.alive = false;
            plugin.sandbox.crashRecovered = true;
            continue;
        }

        HANDLE mappingHandle = reinterpret_cast<HANDLE>(plugin.sandbox.mappingHandle);
        void* view = MapViewOfFile(mappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SandboxSharedState));
        if (view == nullptr)
        {
            continue;
        }

        auto* shared = reinterpret_cast<SandboxSharedState*>(view);
        const std::int32_t heartbeat = shared->heartbeat;
        if (heartbeat != plugin.sandbox.lastHeartbeat)
        {
            plugin.sandbox.lastHeartbeat = heartbeat;
            plugin.sandbox.watchdogMisses = 0;
            plugin.sandbox.lastHeartbeatTime = Clock::now();
        }
        else
        {
            ++plugin.sandbox.watchdogMisses;
        }

        if (plugin.sandbox.watchdogMisses >= kSandboxWatchdogThreshold)
        {
            plugin.active = false;
            plugin.autoBypassed = true;
            plugin.sandbox.alive = false;
            plugin.sandbox.crashRecovered = true;
            shared->stopRequested = 1;
        }

        UnmapViewOfFile(view);
    }
}

void AudioEngine::serviceClipCacheEviction()
{
    std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
    if (clipCache_.size() <= 8)
    {
        return;
    }

    const auto now = Clock::now();
    clipCache_.erase(
        std::remove_if(
            clipCache_.begin(),
            clipCache_.end(),
            [&](const ClipCacheEntry& entry)
            {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.lastAccess).count();
                return age > 30;
            }),
        clipCache_.end());

    metrics_.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
}

void AudioEngine::pushUndoState(const std::string& description, const ProjectState& beforeState)
{
    undoStack_.push_back(ProjectAction{description, beforeState, projectState_});
    redoStack_.clear();
}

std::uint32_t AudioEngine::nextTrackId() const
{
    std::uint32_t maxId = 0;
    for (const auto& track : projectState_.tracks)
    {
        maxId = std::max(maxId, track.trackId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextBusId() const
{
    std::uint32_t maxId = 0;
    for (const auto& bus : projectState_.buses)
    {
        maxId = std::max(maxId, bus.busId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextClipId() const
{
    std::uint32_t maxId = 0;
    for (const auto& clip : projectState_.clips)
    {
        maxId = std::max(maxId, clip.clipId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextPluginInstanceId() const
{
    std::uint32_t maxId = 0;
    std::lock_guard<std::mutex> pluginLock(pluginMutex_);
    for (const auto& plugin : loadedPlugins_)
    {
        maxId = std::max(maxId, plugin.instanceId);
    }
    return maxId + 1;
}

void AudioEngine::processPendingCommands()
{
    EngineCommand command{};
    while (consumeNextCommand(command))
    {
        switch (command.type)
        {
        case CommandType::NewProject:
            newProject(command.textValue);
            break;

        case CommandType::StartEngine:
            start();
            break;

        case CommandType::StopEngine:
            stop();
            break;

        case CommandType::RecoverAudioDevice:
            recoverAudioDevice();
            break;

        case CommandType::PlayTransport:
            play();
            break;

        case CommandType::PauseTransport:
            pause();
            break;

        case CommandType::StopTransport:
            stopTransport();
            break;

        case CommandType::RebuildGraph:
            requestGraphRebuild();
            break;

        case CommandType::RecompileGraph:
            compileGraph();
            break;

        case CommandType::SetTempo:
            setTempo(command.doubleValue);
            break;

        case CommandType::SetTimelinePosition:
            setTimelinePosition(command.doubleValue);
            break;

        case CommandType::SetSamplePosition:
            setSamplePosition(command.uintValue);
            break;

        case CommandType::ToggleAnticipativeProcessing:
            config_.enableAnticipativeProcessing = !config_.enableAnticipativeProcessing;
            publishSnapshot();
            break;

        case CommandType::ToggleAutomation:
            config_.enableSampleAccurateAutomation = !config_.enableSampleAccurateAutomation;
            publishSnapshot();
            break;

        case CommandType::TogglePdc:
            config_.enablePdc = !config_.enablePdc;
            recalculateLatencyModel();
            publishSnapshot();
            break;

        case CommandType::RenderOffline:
        {
            OfflineRenderRequest request{};
            request.startTimeSeconds = 0.0;
            request.endTimeSeconds = 12.0;
            request.sampleRate = deviceState_.sampleRate;
            request.outputPath = command.textValue.empty() ? "offline_render.wav" : command.textValue;
            renderOffline(request);
            break;
        }

        case CommandType::FreezeTrack:
            freezeTrack(static_cast<std::uint32_t>(command.uintValue));
            break;

        case CommandType::AddTrack:
            addTrack(command.textValue);
            break;

        case CommandType::AddBus:
            addBus(command.textValue);
            break;

        case CommandType::AddClipToTrack:
            addClipToTrack(static_cast<std::uint32_t>(command.uintValue), command.textValue);
            break;

        case CommandType::MoveClip:
            moveClip(
                static_cast<std::uint32_t>(command.uintValue),
                static_cast<std::uint32_t>(command.secondaryUintValue),
                command.doubleValue);
            break;

        case CommandType::SaveProject:
            saveProject(command.textValue.empty() ? config_.sessionPath : command.textValue);
            break;

        case CommandType::LoadProject:
            loadProject(command.textValue.empty() ? config_.sessionPath : command.textValue);
            break;

        case CommandType::UndoEdit:
            undoLastEdit();
            break;

        case CommandType::RedoEdit:
            redoLastEdit();
            break;

        case CommandType::None:
        default:
            break;
        }
    }
}
