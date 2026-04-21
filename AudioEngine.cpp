#include "AudioEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int kErrorInvalidConfig = 1001;
    constexpr int kErrorBackendInit = 1002;
    constexpr int kErrorGraphBuild = 1003;
    constexpr int kErrorGraphCompile = 1004;
    constexpr int kErrorTransportInit = 1005;
    constexpr int kErrorOfflineRender = 1006;

    const char* toString(AudioEngine::EngineState state)
    {
        switch (state)
        {
        case AudioEngine::EngineState::Uninitialized: return "Uninitialized";
        case AudioEngine::EngineState::Initializing:  return "Initializing";
        case AudioEngine::EngineState::Stopped:       return "Stopped";
        case AudioEngine::EngineState::Starting:      return "Starting";
        case AudioEngine::EngineState::Running:       return "Running";
        case AudioEngine::EngineState::Stopping:      return "Stopping";
        case AudioEngine::EngineState::Error:         return "Error";
        default:                                      return "Unknown";
        }
    }

    const char* backendName(AudioEngine::BackendType type)
    {
        switch (type)
        {
        case AudioEngine::BackendType::Wasapi: return "WASAPI";
        case AudioEngine::BackendType::Dummy:  return "Dummy";
        default:                               return "None";
        }
    }

    float lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }
}

AudioEngine::AudioEngine()
{
    resetMetrics();
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

    config_ = config;

    deviceState_.sampleRate = static_cast<std::uint32_t>(config_.preferredSampleRate);
    deviceState_.blockSize = static_cast<std::uint32_t>(config_.preferredBlockSize);
    deviceState_.inputChannels = static_cast<std::uint32_t>(config_.inputChannelCount);
    deviceState_.outputChannels = static_cast<std::uint32_t>(config_.outputChannelCount);

    realtimeBuffer_.resize(deviceState_.blockSize);
    anticipativeBuffer_.resize(deviceState_.blockSize);

    editableGraph_ = {};
    pendingGraph_ = {};
    compiledGraph_ = {};
    graphSwapPending_ = false;

    automationLanes_.clear();
    latencyStates_.clear();
    delayLines_.clear();
    loadedPlugins_.clear();
    diskReadQueue_.clear();
    clipCache_.clear();
    lastOfflineRenderRequest_.reset();

    transportInfo_ = {};
    transportInfo_.tempoBpm = 120.0;
    transportInfo_.monitoringEnabled = true;

    {
        std::lock_guard<std::mutex> lock(commandQueueMutex_);
        commandQueue_.clear();
    }

    resetMetrics();

    initialized_.store(true);
    running_.store(false);
    telemetryRunning_.store(false);
    state_.store(EngineState::Stopped);
    updateStatusFromState();

    return true;
}

void AudioEngine::shutdown()
{
    stop();
    initialized_.store(false);
    deviceState_.initialized = false;
    deviceState_.started = false;
    state_.store(EngineState::Uninitialized);
    updateStatusFromState();
}

bool AudioEngine::initializeAudioBackend()
{
    if (!initialized_.load())
    {
        setError(kErrorBackendInit, "Engine must be initialized before backend setup.");
        return false;
    }

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (config_.enableWasapi)
    {
        deviceState_.backendType = BackendType::Wasapi;
        deviceState_.backendName = "WASAPI";
        deviceState_.deviceName = config_.preferredDeviceName.empty() ? "Default WASAPI Device" : config_.preferredDeviceName;
    }
    else
#endif
    {
        deviceState_.backendType = BackendType::Dummy;
        deviceState_.backendName = "Dummy";
        deviceState_.deviceName = "Dummy Audio Device";
    }

    deviceState_.initialized = true;
    return true;
}

bool AudioEngine::start()
{
    if (!initialized_.load())
    {
        setError(kErrorBackendInit, "Engine not initialized.");
        return false;
    }

    if (!deviceState_.initialized)
    {
        setError(kErrorBackendInit, "Audio backend not initialized.");
        return false;
    }

    if (running_.load())
    {
        return true;
    }

    state_.store(EngineState::Starting);
    updateStatusFromState();

    try
    {
        realtimeBuffer_.resize(deviceState_.blockSize);
        anticipativeBuffer_.resize(deviceState_.blockSize);
        deviceState_.started = true;
        running_.store(true);
        state_.store(EngineState::Running);
        updateStatusFromState();
        return true;
    }
    catch (...)
    {
        deviceState_.started = false;
        running_.store(false);
        state_.store(EngineState::Error);
        setError(kErrorBackendInit, "Failed to start backend/device.");
        return false;
    }
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
    deviceState_.started = false;

    if (state_.load() != EngineState::Error)
    {
        state_.store(EngineState::Stopped);
    }

    updateStatusFromState();
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
    std::string text = "Audio Engine: ";
    text += toString(state_.load());
    text += " | Backend: ";
    text += deviceState_.backendName;
    text += " | Device: ";
    text += deviceState_.deviceName;
    text += " | SR: ";
    text += std::to_string(deviceState_.sampleRate);
    text += " | Block: ";
    text += std::to_string(deviceState_.blockSize);
    return text;
}

std::string AudioEngine::getLastErrorMessage() const
{
    return lastErrorMessage_;
}

int AudioEngine::getLastErrorCode() const noexcept
{
    return lastErrorCode_;
}

const AudioEngine::EngineConfig& AudioEngine::getConfig() const noexcept
{
    return config_;
}

AudioEngine::EngineMetrics AudioEngine::getMetrics() const noexcept
{
    return metrics_;
}

std::string AudioEngine::getBackendName() const
{
    return deviceState_.backendName;
}

std::string AudioEngine::getCurrentDeviceName() const
{
    return deviceState_.deviceName;
}

int AudioEngine::getCurrentSampleRate() const noexcept
{
    return static_cast<int>(deviceState_.sampleRate);
}

int AudioEngine::getCurrentBlockSize() const noexcept
{
    return static_cast<int>(deviceState_.blockSize);
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
    return true;
}

void AudioEngine::play()
{
    transportInfo_.state = TransportState::Playing;
}

void AudioEngine::pause()
{
    transportInfo_.state = TransportState::Paused;
}

void AudioEngine::stopTransport()
{
    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;
}

void AudioEngine::setTimelinePosition(double seconds)
{
    transportInfo_.timelineSeconds = std::max(0.0, seconds);
}

void AudioEngine::setTempo(double bpm)
{
    transportInfo_.tempoBpm = std::max(1.0, bpm);
}

void AudioEngine::setSamplePosition(std::uint64_t samplePosition)
{
    transportInfo_.samplePosition = samplePosition;
}

bool AudioEngine::isPlaying() const noexcept
{
    return transportInfo_.state == TransportState::Playing;
}

bool AudioEngine::isMonitoring() const noexcept
{
    return transportInfo_.monitoringEnabled;
}

AudioEngine::TransportInfo AudioEngine::getTransportInfo() const noexcept
{
    return transportInfo_;
}

bool AudioEngine::buildInitialGraph()
{
    if (!initialized_.load())
    {
        setError(kErrorGraphBuild, "Engine must be initialized before building graph.");
        return false;
    }

    editableGraph_ = {};
    editableGraph_.version = 1;

    editableGraph_.nodes.push_back(GraphNode{1, NodeType::Input, "Input", 0, true, false, false});
    editableGraph_.nodes.push_back(GraphNode{2, NodeType::Track, "Track 1", 0, true, true, false});
    editableGraph_.nodes.push_back(GraphNode{3, NodeType::Plugin, "Plugin Slot Stub", 64, true, true, false});
    editableGraph_.nodes.push_back(GraphNode{4, NodeType::Bus, "Main Bus", 0, true, true, false});
    editableGraph_.nodes.push_back(GraphNode{5, NodeType::Output, "Hardware Output", 0, true, false, false});

    editableGraph_.edges.push_back(GraphEdge{1, 2});
    editableGraph_.edges.push_back(GraphEdge{2, 3});
    editableGraph_.edges.push_back(GraphEdge{3, 4});
    editableGraph_.edges.push_back(GraphEdge{4, 5});

    pendingGraph_ = editableGraph_;
    graphSwapPending_ = true;

    automationLanes_.clear();
    automationLanes_.push_back(AutomationLane{3, config_.enableSampleAccurateAutomation, {}, {}});

    recalculateLatencyModel();
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

    CompiledGraph newCompiledGraph{};
    newCompiledGraph.sourceVersion = sourceGraph.version;
    newCompiledGraph.valid = true;

    std::uint32_t order = 0;
    for (const auto& node : sourceGraph.nodes)
    {
        CompiledNode compiled{};
        compiled.nodeId = node.id;
        compiled.type = node.type;
        compiled.executionOrder = order++;
        compiled.liveLane = node.liveCritical;
        compiled.anticipativeLane = node.canProcessAnticipatively && config_.enableAnticipativeProcessing;
        compiled.accumulatedLatencySamples = node.latencySamples;
        newCompiledGraph.executionPlan.push_back(compiled);
    }

    compiledGraph_ = newCompiledGraph;
    metrics_.activeGraphVersion = compiledGraph_.sourceVersion;
    graphSwapPending_ = false;

    recalculateLatencyModel();
    return true;
}

void AudioEngine::requestGraphRebuild()
{
    pendingGraph_ = editableGraph_;
    ++pendingGraph_.version;

    if (!pendingGraph_.nodes.empty())
    {
        pendingGraph_.nodes[0].name = pendingGraph_.nodes[0].name;
    }

    graphSwapPending_ = true;
}

bool AudioEngine::swapCompiledGraphAtSafePoint()
{
    if (!graphSwapPending_)
    {
        return false;
    }

    if (!compileGraph())
    {
        return false;
    }

    return true;
}

std::uint64_t AudioEngine::getActiveGraphVersion() const noexcept
{
    return metrics_.activeGraphVersion;
}

void AudioEngine::processLiveBlock(RenderContext& renderContext, AudioBuffer& ioBuffer)
{
    ioBuffer.clear();

    const float baseGain = config_.prefer64BitInternalMix ? 0.12f : 0.10f;

    for (const auto& node : compiledGraph_.executionPlan)
    {
        if (!node.liveLane)
        {
            continue;
        }

        for (std::uint32_t i = 0; i < ioBuffer.frameCount; ++i)
        {
            const float phase = static_cast<float>((renderContext.callback.callbackIndex * ioBuffer.frameCount + i) % 256) / 255.0f;
            const float signal = baseGain * std::sin(phase * 6.28318530718f);

            ioBuffer.left[i] += signal;
            ioBuffer.right[i] += signal;
        }
    }
}

void AudioEngine::processAnticipativeWork(RenderContext& renderContext)
{
    anticipativeBuffer_.clear();

    if (!config_.enableAnticipativeProcessing)
    {
        return;
    }

    for (const auto& node : compiledGraph_.executionPlan)
    {
        if (!node.anticipativeLane)
        {
            continue;
        }

        for (std::uint32_t i = 0; i < anticipativeBuffer_.frameCount; ++i)
        {
            const float phase = static_cast<float>((renderContext.callback.callbackIndex * anticipativeBuffer_.frameCount + i + 64) % 512) / 511.0f;
            const float signal = 0.04f * std::sin(phase * 6.28318530718f);
            anticipativeBuffer_.left[i] += signal;
            anticipativeBuffer_.right[i] += signal;
        }
    }
}

void AudioEngine::mergeLiveAndAnticipativeResults(RenderContext&, AudioBuffer& ioBuffer)
{
    for (std::uint32_t i = 0; i < ioBuffer.frameCount && i < anticipativeBuffer_.frameCount; ++i)
    {
        ioBuffer.left[i] += anticipativeBuffer_.left[i];
        ioBuffer.right[i] += anticipativeBuffer_.right[i];
    }
}

void AudioEngine::enqueueAutomationEvent(const AutomationEvent& event)
{
    auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == event.nodeId; });

    if (it == automationLanes_.end())
    {
        AutomationLane lane{};
        lane.nodeId = event.nodeId;
        lane.sampleAccurateEnabled = config_.enableSampleAccurateAutomation;
        lane.pendingEvents.push_back(event);
        automationLanes_.push_back(lane);
        return;
    }

    it->pendingEvents.push_back(event);
}

void AudioEngine::applyAutomationForBlock(RenderContext& renderContext)
{
    if (!config_.enableSampleAccurateAutomation)
    {
        return;
    }

    for (auto& lane : automationLanes_)
    {
        if (!lane.sampleAccurateEnabled)
        {
            continue;
        }

        std::sort(
            lane.pendingEvents.begin(),
            lane.pendingEvents.end(),
            [](const AutomationEvent& a, const AutomationEvent& b)
            {
                return a.sampleOffset < b.sampleOffset;
            });

        for (const auto& event : lane.pendingEvents)
        {
            auto paramIt = std::find_if(
                lane.parameterStates.begin(),
                lane.parameterStates.end(),
                [&](const ParameterState& state) { return state.parameterId == event.parameterId; });

            if (paramIt == lane.parameterStates.end())
            {
                ParameterState newState{};
                newState.parameterId = event.parameterId;
                newState.currentValue = event.value;
                newState.targetValue = event.value;
                newState.smoother.currentValue = event.value;
                newState.smoother.targetValue = event.value;
                lane.parameterStates.push_back(newState);
            }
            else
            {
                paramIt->targetValue = event.value;
                paramIt->smoother.targetValue = event.value;

                if (renderContext.supportsSampleAccurateAutomation)
                {
                    const float t = static_cast<float>(event.sampleOffset) /
                                    static_cast<float>(std::max<std::uint32_t>(1, renderContext.callback.blockSize - 1));
                    paramIt->currentValue = lerp(paramIt->currentValue, paramIt->targetValue, t);
                    paramIt->smoother.currentValue = paramIt->currentValue;
                }
                else
                {
                    paramIt->currentValue = event.value;
                    paramIt->smoother.currentValue = event.value;
                }
            }
        }

        lane.pendingEvents.clear();
    }
}

void AudioEngine::flushAutomationToNode(std::uint32_t nodeId)
{
    auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == nodeId; });

    if (it != automationLanes_.end())
    {
        it->pendingEvents.clear();
    }
}

void AudioEngine::recalculateLatencyModel()
{
    latencyStates_.clear();
    delayLines_.clear();

    std::uint32_t maxLatency = 0;

    for (const auto& node : editableGraph_.nodes)
    {
        DelayCompensationState state{};
        state.nodeId = node.id;
        state.localLatencySamples = node.latencySamples;
        state.accumulatedLatencySamples = node.latencySamples;
        latencyStates_.push_back(state);

        maxLatency = std::max(maxLatency, node.latencySamples);
    }

    for (const auto& latencyState : latencyStates_)
    {
        DelayLine delayLine{};
        delayLine.delaySamples = maxLatency > latencyState.accumulatedLatencySamples
            ? (maxLatency - latencyState.accumulatedLatencySamples)
            : 0;
        delayLine.buffer.resize(std::max<std::uint32_t>(1, delayLine.delaySamples + deviceState_.blockSize), 0.0f);
        delayLines_.push_back(delayLine);
    }

    metrics_.currentLatencySamples = maxLatency;
}

void AudioEngine::alignBranchesForMix(AudioBuffer& ioBuffer)
{
    if (!config_.enablePdc || delayLines_.empty())
    {
        return;
    }

    const float compensationGain = 1.0f;
    for (std::uint32_t i = 0; i < ioBuffer.frameCount; ++i)
    {
        ioBuffer.left[i] *= compensationGain;
        ioBuffer.right[i] *= compensationGain;
    }
}

bool AudioEngine::addPluginStub(const PluginDescriptor& descriptor, std::uint32_t ownerNodeId)
{
    PluginInstance instance{};
    instance.instanceId = static_cast<std::uint32_t>(loadedPlugins_.size() + 1);
    instance.descriptor = descriptor;
    instance.ownerNodeId = ownerNodeId;
    instance.active = true;
    instance.sandboxed = descriptor.processMode != PluginProcessMode::InProcess;

    loadedPlugins_.push_back(instance);

    for (auto& node : editableGraph_.nodes)
    {
        if (node.id == ownerNodeId)
        {
            node.latencySamples = std::max(node.latencySamples, descriptor.reportedLatencySamples);
            break;
        }
    }

    recalculateLatencyModel();
    return true;
}

std::vector<AudioEngine::PluginDescriptor> AudioEngine::getLoadedPluginDescriptors() const
{
    std::vector<PluginDescriptor> descriptors;
    descriptors.reserve(loadedPlugins_.size());

    for (const auto& instance : loadedPlugins_)
    {
        descriptors.push_back(instance.descriptor);
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

    RenderContext renderContext{};
    renderContext.isOffline = true;
    renderContext.isLivePath = false;
    renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;
    renderContext.callback.sampleRate = request.sampleRate;
    renderContext.callback.blockSize = deviceState_.blockSize;

    AudioBuffer offlineBuffer{};
    offlineBuffer.resize(deviceState_.blockSize);

    const double durationSeconds = request.endTimeSeconds - request.startTimeSeconds;
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(durationSeconds * static_cast<double>(request.sampleRate));
    const std::uint64_t totalBlocks = (totalFrames + offlineBuffer.frameCount - 1) / offlineBuffer.frameCount;

    for (std::uint64_t blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
    {
        renderContext.callback.callbackIndex = blockIndex;
        processPendingCommands();

        if (graphSwapPending_)
        {
            swapCompiledGraphAtSafePoint();
        }

        applyAutomationForBlock(renderContext);
        processLiveBlock(renderContext, offlineBuffer);
        processAnticipativeWork(renderContext);
        mergeLiveAndAnticipativeResults(renderContext, offlineBuffer);
        alignBranchesForMix(offlineBuffer);
    }

    return true;
}

bool AudioEngine::freezeTrack(std::uint32_t trackNodeId)
{
    OfflineRenderRequest request{};
    request.startTimeSeconds = 0.0;
    request.endTimeSeconds = 5.0;
    request.sampleRate = deviceState_.sampleRate;
    request.outputPath = "freeze_track_" + std::to_string(trackNodeId) + ".wav";
    request.highQuality = true;
    return renderOffline(request);
}

bool AudioEngine::startTelemetry()
{
    if (!initialized_.load())
    {
        return false;
    }

    telemetryRunning_.store(true);

    if (!running_.load())
    {
        return true;
    }

    CallbackContext callback{};
    callback.sampleRate = deviceState_.sampleRate;
    callback.blockSize = deviceState_.blockSize;

    RenderContext renderContext{};
    renderContext.callback = callback;
    renderContext.isOffline = false;
    renderContext.isLivePath = true;
    renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;

    realtimeBuffer_.resize(deviceState_.blockSize);
    anticipativeBuffer_.resize(deviceState_.blockSize);

    for (std::uint32_t iteration = 0; iteration < 4 && telemetryRunning_.load() && running_.load(); ++iteration)
    {
        const auto startTime = Clock::now();

        renderContext.callback.callbackIndex = iteration;

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

        if (transportInfo_.state == TransportState::Playing)
        {
            transportInfo_.samplePosition += realtimeBuffer_.frameCount;
            transportInfo_.timelineSeconds =
                static_cast<double>(transportInfo_.samplePosition) /
                static_cast<double>(std::max(1u, deviceState_.sampleRate));
        }

        const auto endTime = Clock::now();
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
        const double blockDeadlineUs =
            (static_cast<double>(renderContext.callback.blockSize) / static_cast<double>(renderContext.callback.sampleRate)) * 1'000'000.0;

        metrics_.peakBlockTimeUs = std::max(metrics_.peakBlockTimeUs, static_cast<double>(elapsedUs));
        metrics_.cpuLoadApprox = std::min(1.0, static_cast<double>(elapsedUs) / std::max(1.0, blockDeadlineUs));

        if (static_cast<double>(elapsedUs) > blockDeadlineUs)
        {
            ++metrics_.deadlineMisses;
            ++metrics_.xruns;
        }
    }

    return true;
}

void AudioEngine::stopTelemetry()
{
    telemetryRunning_.store(false);
}

void AudioEngine::postCommand(const EngineCommand& command)
{
    std::lock_guard<std::mutex> lock(commandQueueMutex_);
    commandQueue_.push_back(command);
}

bool AudioEngine::consumeNextCommand(EngineCommand& outCommand)
{
    std::lock_guard<std::mutex> lock(commandQueueMutex_);

    if (commandQueue_.empty())
    {
        return false;
    }

    outCommand = commandQueue_.front();
    commandQueue_.pop_front();
    return true;
}

void AudioEngine::setError(int errorCode, const std::string& message)
{
    lastErrorCode_ = errorCode;
    lastErrorMessage_ = message;
    state_.store(EngineState::Error);
    updateStatusFromState();
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

void AudioEngine::processPendingCommands()
{
    EngineCommand command{};

    while (consumeNextCommand(command))
    {
        switch (command.type)
        {
        case CommandType::StartEngine:
            start();
            break;

        case CommandType::StopEngine:
            stop();
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
            break;

        case CommandType::ToggleAutomation:
            config_.enableSampleAccurateAutomation = !config_.enableSampleAccurateAutomation;
            break;

        case CommandType::TogglePdc:
            config_.enablePdc = !config_.enablePdc;
            recalculateLatencyModel();
            break;

        case CommandType::RenderOffline:
        {
            OfflineRenderRequest request{};
            request.startTimeSeconds = 0.0;
            request.endTimeSeconds = 10.0;
            request.sampleRate = deviceState_.sampleRate;
            request.outputPath = command.textValue.empty() ? "offline_render.wav" : command.textValue;
            request.highQuality = true;
            renderOffline(request);
            break;
        }

        case CommandType::FreezeTrack:
            freezeTrack(static_cast<std::uint32_t>(command.uintValue));
            break;

        case CommandType::None:
        default:
            break;
        }
    }
}
