#include "PAudioEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
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
    constexpr int kDefaultStepCount = 16;
    constexpr int kDefaultPatternCount = 4;
    constexpr int kDefaultPlaylistCellCount = 32;
    constexpr int kDefaultPianoLaneCount = 24;
    constexpr double kPlaylistCellSeconds = 0.5;

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

    std::string joinString(const std::vector<std::string>& values, char delimiter)
    {
        std::ostringstream stream;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index > 0)
            {
                stream << delimiter;
            }
            stream << values[index];
        }
        return stream.str();
    }

    bool parseBoolToken(const std::string& text)
    {
        return text == "1" || toLowerCopy(text) == "true" || toLowerCopy(text) == "yes";
    }

    bool hasFileExtension(const std::string& path, const std::string& extension)
    {
        return toLowerCopy(std::filesystem::path(path).extension().string()) == toLowerCopy(extension);
    }

    std::string fileStemFromPath(const std::string& path, const std::string& fallback)
    {
        const std::filesystem::path filePath(path);
        const std::string stem = filePath.stem().string();
        return stem.empty() ? fallback : stem;
    }

    double clampSample(double value)
    {
        return std::clamp(value, -1.0, 1.0);
    }

    double sanitizeTempoBpm(double bpm)
    {
        const double clamped = std::clamp(bpm, 10.0, 522.0);
        return std::round(clamped * 1000.0) / 1000.0;
    }

    int extractPatternNumberFromClipName(const std::string& clipName)
    {
        for (std::size_t index = 0; index < clipName.size(); ++index)
        {
            if (std::isdigit(static_cast<unsigned char>(clipName[index])) != 0)
            {
                return std::max(1, std::atoi(clipName.substr(index).c_str()));
            }
        }
        return 1;
    }

    double midiNoteToFrequency(int midiNote)
    {
        return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
    }

    double linearSampleAt(const std::vector<double>& samples, double index)
    {
        if (samples.empty())
        {
            return 0.0;
        }

        const double clampedIndex = std::clamp(index, 0.0, static_cast<double>(samples.size() - 1));
        const std::size_t leftIndex = static_cast<std::size_t>(clampedIndex);
        const std::size_t rightIndex = std::min(leftIndex + 1, samples.size() - 1);
        const double blend = clampedIndex - static_cast<double>(leftIndex);
        return samples[leftIndex] + ((samples[rightIndex] - samples[leftIndex]) * blend);
    }

    double computeEnvelopeGain(
        double elapsedSeconds,
        double noteBodyDurationSeconds,
        const AudioEngine::ChannelSettingsState& settings)
    {
        const double safeElapsed = std::max(0.0, elapsedSeconds);
        const double attackSeconds = std::max(0.0, settings.attackMs / 1000.0);
        const double decaySeconds = std::max(0.0, settings.decayMs / 1000.0);
        const double releaseSeconds = std::max(0.0, settings.releaseMs / 1000.0);
        const double sustainLevel = std::clamp(settings.sustainLevel, 0.0, 1.0);

        if (attackSeconds > 0.0 && safeElapsed < attackSeconds)
        {
            return safeElapsed / attackSeconds;
        }

        const double decayStart = attackSeconds;
        const double decayEnd = decayStart + decaySeconds;
        if (decaySeconds > 0.0 && safeElapsed < decayEnd)
        {
            const double decayT = (safeElapsed - decayStart) / decaySeconds;
            return 1.0 + ((sustainLevel - 1.0) * decayT);
        }

        if (safeElapsed <= noteBodyDurationSeconds)
        {
            return sustainLevel;
        }

        if (releaseSeconds <= 0.0)
        {
            return 0.0;
        }

        const double releaseT = (safeElapsed - noteBodyDurationSeconds) / releaseSeconds;
        if (releaseT >= 1.0)
        {
            return 0.0;
        }

        return sustainLevel * (1.0 - releaseT);
    }

    double computeFadeGain(
        double localTimeSeconds,
        double totalDurationSeconds,
        double fadeInSeconds,
        double fadeOutSeconds)
    {
        double gain = 1.0;

        if (fadeInSeconds > 0.0)
        {
            gain *= std::clamp(localTimeSeconds / fadeInSeconds, 0.0, 1.0);
        }

        if (fadeOutSeconds > 0.0)
        {
            const double tailSeconds = std::max(0.0, totalDurationSeconds - localTimeSeconds);
            gain *= std::clamp(tailSeconds / fadeOutSeconds, 0.0, 1.0);
        }

        return gain;
    }

    std::pair<double, double> stereoPanGains(double pan)
    {
        const double clampedPan = std::clamp(pan, -1.0, 1.0);
        const double angle = (clampedPan + 1.0) * (kTwoPi / 8.0);
        return {std::cos(angle), std::sin(angle)};
    }

    std::string makeAutosavePathForSession(const std::string& sessionPath)
    {
        std::filesystem::path path(sessionPath.empty() ? "session.dawproject" : sessionPath);
        if (!path.has_extension())
        {
            path += ".dawproject";
        }

        const std::string stem = path.stem().string();
        path.replace_filename(stem + ".autosave" + path.extension().string());
        return path.string();
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

    std::uint32_t sendNodeId(std::uint32_t trackId)
    {
        return kPluginNodeBase + (trackId * 10) + 4;
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

    std::shared_ptr<const DecodedWavData> getSamplerSourceData(const std::string& path)
    {
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, std::shared_ptr<DecodedWavData>> cache;

        if (path.empty())
        {
            return nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const auto it = cache.find(path);
            if (it != cache.end())
            {
                return it->second;
            }
        }

        auto decoded = std::make_shared<DecodedWavData>();
        if (!decodeWavFile(path, *decoded))
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[path] = decoded;
        return decoded;
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

    audioThreadState_.helpersRunning.store(false);
    diskWorkerRunning_.store(false);
    anticipativeCv_.notify_all();
    diskQueueCv_.notify_all();

    if (audioThread_.joinable() && !audioThreadState_.running.load())
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

    const bool calledFromAudioThread =
        audioThread_.joinable() && std::this_thread::get_id() == audioThread_.get_id();

    state_.store(EngineState::Stopping);
    updateStatusFromState();
    transportInfo_.state = TransportState::Stopped;

    running_.store(false);
    audioThreadState_.stopRequested.store(true);
    audioThreadState_.helpersRunning.store(false);
    diskWorkerRunning_.store(false);
    anticipativeCv_.notify_all();
    diskQueueCv_.notify_all();

    publishSnapshot();

    if (calledFromAudioThread)
    {
        return true;
    }

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
    transportInfo_.loopEnabled = false;
    transportInfo_.loopStartSample = 0;
    transportInfo_.loopEndSample = 0;
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
    transportInfo_.tempoBpm = sanitizeTempoBpm(bpm);
    publishSnapshot();
}

void AudioEngine::setSamplePosition(std::uint64_t samplePosition)
{
    transportInfo_.samplePosition = samplePosition;
    transportInfo_.timelineSeconds =
        static_cast<double>(samplePosition) / static_cast<double>(std::max(1u, deviceState_.sampleRate));
    publishSnapshot();
}

void AudioEngine::configureTransportLoop(std::uint64_t startSample, std::uint64_t endSample, bool enabled)
{
    transportInfo_.loopStartSample = startSample;
    transportInfo_.loopEndSample = endSample > startSample ? endSample : startSample;
    transportInfo_.loopEnabled =
        enabled && transportInfo_.loopEndSample > transportInfo_.loopStartSample;

    if (transportInfo_.loopEnabled && transportInfo_.samplePosition >= transportInfo_.loopEndSample)
    {
        const std::uint64_t loopLength = transportInfo_.loopEndSample - transportInfo_.loopStartSample;
        const std::uint64_t offset = loopLength == 0
            ? 0
            : ((transportInfo_.samplePosition - transportInfo_.loopStartSample) % loopLength);
        transportInfo_.samplePosition = transportInfo_.loopStartSample + offset;
        transportInfo_.timelineSeconds =
            static_cast<double>(transportInfo_.samplePosition) /
            static_cast<double>(std::max(1u, deviceState_.sampleRate));
    }

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

    const ProjectState originalProjectState = projectState_;
    const bool isolateTrack = request.targetTrackId != 0;
    if (isolateTrack)
    {
        for (auto& track : projectState_.tracks)
        {
            track.muted = track.trackId != request.targetTrackId;
        }
    }

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
        if (isolateTrack)
        {
            projectState_ = originalProjectState;
        }
        setError(kErrorOfflineRender, "Failed to write offline render to disk.");
        return false;
    }

    if (isolateTrack)
    {
        projectState_ = originalProjectState;
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
    request.targetTrackId =
        trackNodeId >= kTrackNodeBase
            ? static_cast<std::uint32_t>((trackNodeId - kTrackNodeBase) / 10)
            : trackNodeId;
    return renderOffline(request);
}

bool AudioEngine::addPattern(const std::string& name)
{
    const ProjectState beforeState = projectState_;

    int nextPatternNumber = 1;
    for (const auto& pattern : projectState_.patterns)
    {
        nextPatternNumber = std::max(nextPatternNumber, pattern.patternNumber + 1);
    }

    PatternState pattern = makeDefaultPatternState(nextPatternNumber);
    pattern.name = name.empty() ? ("Pattern " + std::to_string(nextPatternNumber)) : name;
    projectState_.patterns.push_back(std::move(pattern));
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add pattern", beforeState);
    publishSnapshot();
    return true;
}

bool AudioEngine::addTrack(const std::string& name)
{
    const ProjectState beforeState = projectState_;

    TrackState track{};
    track.trackId = nextTrackId();
    track.busId = projectState_.buses.empty() ? nextBusId() : projectState_.buses.front().busId;
    track.name = name.empty() ? ("Track " + std::to_string(track.trackId)) : name;
    track.gain = 0.92;
    track.pan = 0.0;
    track.sendAmount = 0.0;

    if (projectState_.buses.empty())
    {
        BusState bus{};
        bus.busId = nextBusId();
        bus.name = "Main Bus";
        bus.gain = 1.0;
        bus.pan = 0.0;
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

    synchronizeProjectMusicalState();
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
    bus.gain = 1.0;
    bus.pan = 0.0;
    projectState_.buses.push_back(bus);
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add bus", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::togglePatternStep(int patternNumber, std::uint32_t trackId, int stepIndex)
{
    PatternState* pattern = findPatternState(patternNumber);
    if (pattern == nullptr)
    {
        setError(kErrorProjectIo, "Pattern not found while toggling step.");
        return false;
    }

    PatternLaneState* lane = findPatternLane(*pattern, trackId);
    if (lane == nullptr)
    {
        setError(kErrorProjectIo, "Pattern lane not found while toggling step.");
        return false;
    }

    if (stepIndex < 0 || stepIndex >= static_cast<int>(lane->steps.size()))
    {
        setError(kErrorProjectIo, "Step index out of range while toggling step.");
        return false;
    }

    const ProjectState beforeState = projectState_;
    StepState& step = lane->steps[static_cast<std::size_t>(stepIndex)];
    step.enabled = !step.enabled;
    step.velocity = step.enabled ? std::max(96, step.velocity) : 0;
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Toggle step", beforeState);
    publishSnapshot();
    return true;
}

bool AudioEngine::upsertMidiNote(
    int patternNumber,
    std::uint32_t trackId,
    std::size_t noteIndex,
    const MidiNoteState& note,
    bool createIfMissing)
{
    PatternState* pattern = findPatternState(patternNumber);
    if (pattern == nullptr)
    {
        setError(kErrorProjectIo, "Pattern not found while editing MIDI note.");
        return false;
    }

    PatternLaneState* lane = findPatternLane(*pattern, trackId);
    if (lane == nullptr)
    {
        setError(kErrorProjectIo, "Pattern lane not found while editing MIDI note.");
        return false;
    }

    const ProjectState beforeState = projectState_;
    const bool editingExisting = noteIndex < lane->notes.size();
    MidiNoteState sanitizedNote = note;
    sanitizedNote.lane = std::clamp(sanitizedNote.lane, 0, kDefaultPianoLaneCount - 1);
    sanitizedNote.step = std::clamp(sanitizedNote.step, 0, kDefaultPlaylistCellCount - 1);
    sanitizedNote.length = std::max(1, sanitizedNote.length);
    sanitizedNote.velocity = std::clamp(sanitizedNote.velocity, 1, 127);

    if (editingExisting)
    {
        lane->notes[noteIndex] = sanitizedNote;
    }
    else if (createIfMissing)
    {
        lane->notes.push_back(sanitizedNote);
    }
    else
    {
        setError(kErrorProjectIo, "MIDI note index out of range.");
        return false;
    }

    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState(editingExisting ? "Edit MIDI note" : "Add MIDI note", beforeState);
    publishSnapshot();
    return true;
}

bool AudioEngine::setAutomationPoint(std::uint32_t clipId, int pointIndex, int cell, int value)
{
    AutomationClipState* automationClip = findAutomationClip(clipId);
    if (automationClip == nullptr)
    {
        setError(kErrorProjectIo, "Automation clip not found while editing point.");
        return false;
    }

    const ProjectState beforeState = projectState_;
    AutomationPointState point{};
    point.cell = std::max(0, cell);
    point.value = std::clamp(value, 0, 100);

    if (pointIndex < 0)
    {
        automationClip->points.push_back(point);
    }
    else if (pointIndex >= static_cast<int>(automationClip->points.size()))
    {
        automationClip->points.resize(static_cast<std::size_t>(pointIndex + 1));
        automationClip->points[static_cast<std::size_t>(pointIndex)] = point;
    }
    else
    {
        automationClip->points[static_cast<std::size_t>(pointIndex)] = point;
    }

    std::sort(
        automationClip->points.begin(),
        automationClip->points.end(),
        [](const AutomationPointState& left, const AutomationPointState& right)
        {
            return left.cell < right.cell;
        });

    if (!automationClip->points.empty())
    {
        automationClip->startCell = automationClip->points.front().cell;
        automationClip->lengthCells =
            std::max(2, automationClip->points.back().cell - automationClip->startCell + 1);
    }

    if (PlaylistItemState* playlistItem = findPlaylistItem(automationClip->clipId); playlistItem != nullptr)
    {
        playlistItem->trackId = automationClip->trackId;
        playlistItem->startTimeSeconds =
            static_cast<double>(std::max(0, automationClip->startCell)) * kPlaylistCellSeconds;
        playlistItem->durationSeconds =
            static_cast<double>(std::max(1, automationClip->lengthCells)) * kPlaylistCellSeconds;
        playlistItem->label = automationClip->target;
    }

    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Edit automation point", beforeState);
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
    const bool audioClipRequest =
        hasFileExtension(clipName, ".wav") ||
        hasFileExtension(clipName, ".wave") ||
        hasFileExtension(clipName, ".aif") ||
        hasFileExtension(clipName, ".aiff");
    const int patternNumber = audioClipRequest ? 0 : extractPatternNumberFromClipName(clipName);
    const std::string label = clipName.empty()
        ? (audioClipRequest ? ("Audio " + std::to_string(nextClipId())) : ("Pattern " + std::to_string(std::max(1, patternNumber))))
        : clipName;

    ClipState clip{};
    clip.clipId = nextClipId();
    clip.trackId = trackId;
    clip.name = audioClipRequest ? fileStemFromPath(label, "Audio " + std::to_string(clip.clipId)) : label;
    clip.sourceType = audioClipRequest ? ClipSourceType::AudioFile : ClipSourceType::GeneratedTone;
    clip.filePath = audioClipRequest ? clipName : "";
    clip.patternNumber = patternNumber;
    clip.startTimeSeconds = 0.0;
    clip.durationSeconds = 4.0;
    clip.gain = 0.8;
    clip.loopEnabled = !audioClipRequest;
    clip.timeStretchEnabled = true;

    if (audioClipRequest)
    {
        DecodedWavData wavData{};
        if (decodeWavFile(clip.filePath, wavData) && wavData.sampleRate != 0)
        {
            clip.durationSeconds = static_cast<double>(wavData.left.size()) / static_cast<double>(wavData.sampleRate);
        }
    }

    PlaylistItemState item{};
    item.itemId = nextPlaylistItemId();
    item.trackId = trackId;
    item.type = audioClipRequest ? PlaylistItemType::AudioClip : PlaylistItemType::PatternClip;
    item.sourceClipId = clip.clipId;
    item.patternNumber = patternNumber;
    item.label = audioClipRequest ? clip.name : ("Pattern " + std::to_string(std::max(1, patternNumber)));
    item.startTimeSeconds = 0.0;
    item.durationSeconds = std::max(2.0, clip.durationSeconds);
    item.gain = clip.gain;
    item.pan = clip.pan;
    item.pitchSemitones = clip.pitchSemitones;
    item.stretchRatio = clip.stretchRatio;
    item.muted = clip.muted;
    item.loopEnabled = clip.loopEnabled;
    item.timeStretchEnabled = clip.timeStretchEnabled;

    trackIt->clipIds.push_back(item.itemId);
    projectState_.clips.push_back(clip);
    projectState_.playlistItems.push_back(item);
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Add clip", beforeState);
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::moveClip(
    std::uint32_t clipId,
    std::uint32_t targetTrackId,
    double startTimeSeconds,
    double durationSeconds)
{
    PlaylistItemState* clipIt = findPlaylistItem(clipId);

    if (clipIt == nullptr)
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

        if (clipIt->type != PlaylistItemType::AutomationClip)
        {
            targetTrackIt->clipIds.push_back(clipId);
        }
        clipIt->trackId = targetTrackId;
    }

    clipIt->startTimeSeconds = std::max(0.0, startTimeSeconds);
    if (durationSeconds > 0.0)
    {
        clipIt->durationSeconds = std::max(0.125, durationSeconds);
    }
    if (clipIt->type == PlaylistItemType::AutomationClip)
    {
        AutomationClipState* automationClip = findAutomationClip(clipIt->automationClipId);
        if (automationClip != nullptr)
        {
            automationClip->trackId = clipIt->trackId;
            automationClip->startCell =
                std::max(0, static_cast<int>(clipIt->startTimeSeconds / kPlaylistCellSeconds + 0.5));
            if (durationSeconds > 0.0)
            {
                automationClip->lengthCells =
                    std::max(2, static_cast<int>(clipIt->durationSeconds / kPlaylistCellSeconds + 0.5));
            }
        }
    }
    if (ClipState* sourceClip = findClipState(clipIt->sourceClipId); sourceClip != nullptr)
    {
        sourceClip->trackId = clipIt->trackId;
    }
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

bool AudioEngine::writeProjectFile(const std::string& path, bool updateSessionPath, bool clearDirtyState)
{
    const std::string resolvedPath = path.empty()
        ? (updateSessionPath ? config_.sessionPath : projectState_.autosavePath)
        : path;
    const std::string sessionPathForFile = updateSessionPath ? resolvedPath : projectState_.sessionPath;
    const std::string autosavePathForFile = updateSessionPath
        ? makeAutosavePathForSession(resolvedPath)
        : (projectState_.autosavePath.empty() ? makeAutosavePathForSession(projectState_.sessionPath) : projectState_.autosavePath);
    const std::string recoveryPathForFile = updateSessionPath
        ? autosavePathForFile
        : (projectState_.recoveryPath.empty() ? autosavePathForFile : projectState_.recoveryPath);
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

    stream << "PROJECT|" << projectState_.projectName << '|' << sessionPathForFile << '|' << projectState_.revision << '\n';
    stream
        << "PROJECTMETA|" << (projectState_.autosaveEnabled ? 1 : 0) << '|'
        << autosavePathForFile << '|'
        << recoveryPathForFile << '|'
        << projectState_.lastSavedRevision << '|'
        << projectState_.lastAutosavedRevision << '|'
        << (projectState_.recoveryAvailable ? 1 : 0) << '\n';

    for (const auto& bus : projectState_.buses)
    {
        stream
            << "BUS|" << bus.busId << '|'
            << bus.name << '|'
            << bus.gain << '|'
            << bus.pan << '|'
            << (bus.muted ? 1 : 0) << '|'
            << (bus.solo ? 1 : 0) << '|'
            << bus.outputBusId << '\n';
    }

    for (const auto& track : projectState_.tracks)
    {
        stream
            << "TRACK|" << track.trackId << '|'
            << track.busId << '|'
            << track.name << '|'
            << (track.armed ? 1 : 0) << '|'
            << (track.muted ? 1 : 0) << '|'
            << (track.solo ? 1 : 0) << '|'
            << (track.recordEnabled ? 1 : 0) << '|'
            << track.gain << '|'
            << track.pan << '|'
            << track.routeTargetBusId << '|'
            << track.sendBusId << '|'
            << track.sendAmount << '\n';
    }

    for (const auto& clip : projectState_.clips)
    {
        stream
            << "CLIP|" << clip.clipId << '|'
            << clip.trackId << '|'
            << clip.name << '|'
            << static_cast<int>(clip.sourceType) << '|'
            << clip.filePath << '|'
            << clip.startTimeSeconds << '|'
            << clip.durationSeconds << '|'
            << clip.gain << '|'
            << (clip.muted ? 1 : 0) << '|'
            << clip.patternNumber << '|'
            << clip.trimStartSeconds << '|'
            << clip.trimEndSeconds << '|'
            << clip.fadeInSeconds << '|'
            << clip.fadeOutSeconds << '|'
            << clip.pan << '|'
            << clip.pitchSemitones << '|'
            << clip.stretchRatio << '|'
            << (clip.loopEnabled ? 1 : 0) << '|'
            << (clip.timeStretchEnabled ? 1 : 0) << '\n';
    }

    for (const auto& item : projectState_.playlistItems)
    {
        stream
            << "PLAYLIST|" << item.itemId << '|'
            << item.trackId << '|'
            << static_cast<int>(item.type) << '|'
            << item.sourceClipId << '|'
            << item.automationClipId << '|'
            << item.patternNumber << '|'
            << item.label << '|'
            << item.startTimeSeconds << '|'
            << item.durationSeconds << '|'
            << item.trimStartSeconds << '|'
            << item.trimEndSeconds << '|'
            << item.fadeInSeconds << '|'
            << item.fadeOutSeconds << '|'
            << item.gain << '|'
            << item.pan << '|'
            << item.pitchSemitones << '|'
            << item.stretchRatio << '|'
            << (item.muted ? 1 : 0) << '|'
            << (item.loopEnabled ? 1 : 0) << '|'
            << (item.timeStretchEnabled ? 1 : 0) << '\n';
    }

    for (const auto& marker : projectState_.markers)
    {
        stream << "MARKER|" << marker.markerId << '|' << marker.name << '|' << marker.timeSeconds << '\n';
    }

    for (const auto& pattern : projectState_.patterns)
    {
        stream
            << "PATTERN|" << pattern.patternNumber << '|'
            << pattern.name << '|'
            << pattern.lengthInBars << '|'
            << pattern.accentAmount << '\n';

        for (const auto& lane : pattern.lanes)
        {
            stream
                << "PATTERNLANE|" << pattern.patternNumber << '|'
                << lane.trackId << '|'
                << lane.swing << '|'
                << lane.shuffle << '\n';

            for (std::size_t stepIndex = 0; stepIndex < lane.steps.size(); ++stepIndex)
            {
                const StepState& step = lane.steps[stepIndex];
                stream
                    << "STEP|" << pattern.patternNumber << '|'
                    << lane.trackId << '|'
                    << stepIndex << '|'
                    << (step.enabled ? 1 : 0) << '|'
                    << step.velocity << '\n';
            }

            for (const auto& note : lane.notes)
            {
                stream
                    << "NOTE|" << pattern.patternNumber << '|'
                    << lane.trackId << '|'
                    << note.lane << '|'
                    << note.step << '|'
                    << note.length << '|'
                    << note.velocity << '|'
                    << (note.accent ? 1 : 0) << '|'
                    << (note.slide ? 1 : 0) << '\n';
            }
        }
    }

    for (const auto& settings : projectState_.channelSettings)
    {
        stream
            << "CHANNEL|" << settings.trackId << '|'
            << settings.name << '|'
            << static_cast<int>(settings.generatorType) << '|'
            << settings.sampleFilePath << '|'
            << settings.instrumentPluginName << '|'
            << settings.gain << '|'
            << settings.pan << '|'
            << settings.pitchSemitones << '|'
            << settings.attackMs << '|'
            << settings.decayMs << '|'
            << settings.sustainLevel << '|'
            << settings.releaseMs << '|'
            << settings.filterCutoffHz << '|'
            << settings.resonance << '|'
            << settings.mixerInsert << '|'
            << settings.routeTarget << '|'
            << (settings.reverse ? 1 : 0) << '|'
            << (settings.timeStretch ? 1 : 0) << '|'
            << joinString(settings.pluginRack, ',') << '|'
            << joinString(settings.presets, ',') << '\n';
    }

    for (const auto& automationClip : projectState_.automationClips)
    {
        stream
            << "AUTOCLIP|" << automationClip.clipId << '|'
            << automationClip.target << '|'
            << automationClip.trackId << '|'
            << automationClip.lane << '|'
            << automationClip.startCell << '|'
            << automationClip.lengthCells << '\n';

        for (const auto& point : automationClip.points)
        {
            stream
                << "AUTOPOINT|" << automationClip.clipId << '|'
                << point.cell << '|'
                << point.value << '|'
                << point.curve << '\n';
        }
    }

    if (updateSessionPath)
    {
        projectState_.sessionPath = resolvedPath;
        projectState_.autosavePath = makeAutosavePathForSession(resolvedPath);
        projectState_.recoveryPath = projectState_.autosavePath;
        config_.sessionPath = resolvedPath;
    }

    if (clearDirtyState)
    {
        projectState_.dirty = false;
        projectState_.lastSavedRevision = projectState_.revision;
        projectState_.recoveryAvailable = false;
    }
    else
    {
        projectState_.lastAutosavedRevision = projectState_.revision;
        projectState_.recoveryPath = resolvedPath;
        projectState_.recoveryAvailable = true;
    }

    publishSnapshot();
    return true;
}

void AudioEngine::maybeAutosaveProject()
{
    if (!projectState_.autosaveEnabled || !projectState_.dirty)
    {
        return;
    }

    if (projectState_.autosavePath.empty())
    {
        projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    }

    if (projectState_.lastAutosavedRevision == projectState_.revision)
    {
        return;
    }

    writeProjectFile(projectState_.autosavePath, false, false);
}

bool AudioEngine::saveProject(const std::string& path)
{
    const std::string resolvedPath = path.empty() ? config_.sessionPath : path;
    return writeProjectFile(resolvedPath, true, true);
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
    loadedProject.autosaveEnabled = true;
    loadedProject.autosavePath = makeAutosavePathForSession(resolvedPath);
    loadedProject.recoveryPath = loadedProject.autosavePath;

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
            else if (parts[0] == "PROJECTMETA" && parts.size() >= 7)
            {
                loadedProject.autosaveEnabled = parseBoolToken(parts[1]);
                loadedProject.autosavePath = parts[2];
                loadedProject.recoveryPath = parts[3];
                loadedProject.lastSavedRevision = static_cast<std::uint64_t>(std::stoull(parts[4]));
                loadedProject.lastAutosavedRevision = static_cast<std::uint64_t>(std::stoull(parts[5]));
                loadedProject.recoveryAvailable = parseBoolToken(parts[6]);
            }
            else if (parts[0] == "BUS" && parts.size() >= 3)
            {
                BusState bus{};
                bus.busId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                bus.name = parts[2];
                if (parts.size() >= 8)
                {
                    bus.gain = std::stod(parts[3]);
                    bus.pan = std::stod(parts[4]);
                    bus.muted = parseBoolToken(parts[5]);
                    bus.solo = parseBoolToken(parts[6]);
                    bus.outputBusId = static_cast<std::uint32_t>(std::stoul(parts[7]));
                }
                loadedProject.buses.push_back(std::move(bus));
            }
            else if (parts[0] == "TRACK" && parts.size() >= 7)
            {
                TrackState track{};
                track.trackId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                track.busId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                track.name = parts[3];
                track.armed = parseBoolToken(parts[4]);
                track.muted = parseBoolToken(parts[5]);
                track.solo = parseBoolToken(parts[6]);
                if (parts.size() >= 13)
                {
                    track.recordEnabled = parseBoolToken(parts[7]);
                    track.gain = std::stod(parts[8]);
                    track.pan = std::stod(parts[9]);
                    track.routeTargetBusId = static_cast<std::uint32_t>(std::stoul(parts[10]));
                    track.sendBusId = static_cast<std::uint32_t>(std::stoul(parts[11]));
                    track.sendAmount = std::stod(parts[12]);
                }
                loadedProject.tracks.push_back(std::move(track));
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
                clip.muted = parseBoolToken(parts[9]);
                if (parts.size() >= 19)
                {
                    clip.patternNumber = std::stoi(parts[10]);
                    clip.trimStartSeconds = std::stod(parts[11]);
                    clip.trimEndSeconds = std::stod(parts[12]);
                    clip.fadeInSeconds = std::stod(parts[13]);
                    clip.fadeOutSeconds = std::stod(parts[14]);
                    clip.pan = std::stod(parts[15]);
                    clip.pitchSemitones = std::stod(parts[16]);
                    clip.stretchRatio = std::stod(parts[17]);
                    clip.loopEnabled = parseBoolToken(parts[18]);
                    clip.timeStretchEnabled = parts.size() >= 20 ? parseBoolToken(parts[19]) : false;
                }
                loadedProject.clips.push_back(std::move(clip));
            }
            else if (parts[0] == "PLAYLIST" && parts.size() >= 20)
            {
                PlaylistItemState item{};
                item.itemId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                item.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                item.type = static_cast<PlaylistItemType>(std::stoi(parts[3]));
                item.sourceClipId = static_cast<std::uint32_t>(std::stoul(parts[4]));
                item.automationClipId = static_cast<std::uint32_t>(std::stoul(parts[5]));
                item.patternNumber = std::stoi(parts[6]);
                item.label = parts[7];
                item.startTimeSeconds = std::stod(parts[8]);
                item.durationSeconds = std::stod(parts[9]);
                item.trimStartSeconds = std::stod(parts[10]);
                item.trimEndSeconds = std::stod(parts[11]);
                item.fadeInSeconds = std::stod(parts[12]);
                item.fadeOutSeconds = std::stod(parts[13]);
                item.gain = std::stod(parts[14]);
                item.pan = std::stod(parts[15]);
                item.pitchSemitones = std::stod(parts[16]);
                item.stretchRatio = std::stod(parts[17]);
                item.muted = parseBoolToken(parts[18]);
                item.loopEnabled = parseBoolToken(parts[19]);
                item.timeStretchEnabled = parts.size() >= 21 ? parseBoolToken(parts[20]) : false;
                loadedProject.playlistItems.push_back(std::move(item));
            }
            else if (parts[0] == "MARKER" && parts.size() >= 4)
            {
                loadedProject.markers.push_back(MarkerState{
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    parts[2],
                    std::stod(parts[3])});
            }
            else if (parts[0] == "PATTERN" && parts.size() >= 5)
            {
                PatternState pattern{};
                pattern.patternNumber = std::stoi(parts[1]);
                pattern.name = parts[2];
                pattern.lengthInBars = std::stoi(parts[3]);
                pattern.accentAmount = std::stoi(parts[4]);
                loadedProject.patterns.push_back(std::move(pattern));
            }
            else if (parts[0] == "PATTERNLANE" && parts.size() >= 5)
            {
                const int patternNumber = std::stoi(parts[1]);
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt == loadedProject.patterns.end())
                {
                    PatternState pattern{};
                    pattern.patternNumber = patternNumber;
                    pattern.name = "Pattern " + std::to_string(patternNumber);
                    pattern.lengthInBars = 2;
                    pattern.accentAmount = 0;
                    loadedProject.patterns.push_back(std::move(pattern));
                    patternIt = loadedProject.patterns.end() - 1;
                }

                PatternLaneState lane{};
                lane.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                lane.swing = std::stoi(parts[3]);
                lane.shuffle = std::stoi(parts[4]);
                patternIt->lanes.push_back(std::move(lane));
            }
            else if (parts[0] == "STEP" && parts.size() >= 6)
            {
                const int patternNumber = std::stoi(parts[1]);
                const std::uint32_t trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt != loadedProject.patterns.end())
                {
                    auto laneIt = std::find_if(
                        patternIt->lanes.begin(),
                        patternIt->lanes.end(),
                        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
                    if (laneIt != patternIt->lanes.end())
                    {
                        const std::size_t stepIndex = static_cast<std::size_t>(std::stoul(parts[3]));
                        if (laneIt->steps.size() <= stepIndex)
                        {
                            laneIt->steps.resize(stepIndex + 1);
                        }
                        laneIt->steps[stepIndex] = StepState{parseBoolToken(parts[4]), std::stoi(parts[5])};
                    }
                }
            }
            else if (parts[0] == "NOTE" && parts.size() >= 9)
            {
                const int patternNumber = std::stoi(parts[1]);
                const std::uint32_t trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt != loadedProject.patterns.end())
                {
                    auto laneIt = std::find_if(
                        patternIt->lanes.begin(),
                        patternIt->lanes.end(),
                        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
                    if (laneIt != patternIt->lanes.end())
                    {
                        laneIt->notes.push_back(MidiNoteState{
                            std::stoi(parts[3]),
                            std::stoi(parts[4]),
                            std::stoi(parts[5]),
                            std::stoi(parts[6]),
                            parseBoolToken(parts[7]),
                            parseBoolToken(parts[8]),
                            false});
                    }
                }
            }
            else if (parts[0] == "CHANNEL" && parts.size() >= 15)
            {
                ChannelSettingsState settings{};
                settings.trackId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                settings.name = parts[2];

                if (parts.size() >= 20)
                {
                    settings.generatorType = static_cast<GeneratorType>(std::stoi(parts[3]));
                    settings.sampleFilePath = parts[4];
                    settings.instrumentPluginName = parts[5];
                    settings.gain = std::stod(parts[6]);
                    settings.pan = std::stod(parts[7]);
                    settings.pitchSemitones = std::stod(parts[8]);
                    settings.attackMs = std::stod(parts[9]);
                    settings.decayMs = std::stod(parts[10]);
                    settings.sustainLevel = std::stod(parts[11]);
                    settings.releaseMs = std::stod(parts[12]);
                    settings.filterCutoffHz = std::stod(parts[13]);
                    settings.resonance = std::stod(parts[14]);
                    settings.mixerInsert = std::stoi(parts[15]);
                    settings.routeTarget = std::stoi(parts[16]);
                    settings.reverse = parseBoolToken(parts[17]);
                    settings.timeStretch = parseBoolToken(parts[18]);
                    settings.pluginRack = parts[19].empty() ? std::vector<std::string>{} : splitString(parts[19], ',');
                    settings.presets = parts.size() >= 21 && !parts[20].empty() ? splitString(parts[20], ',') : std::vector<std::string>{};
                }
                else
                {
                    settings.gain = std::stod(parts[3]);
                    settings.pan = std::stod(parts[4]);
                    settings.pitchSemitones = std::stod(parts[5]);
                    settings.attackMs = std::stod(parts[6]);
                    settings.releaseMs = std::stod(parts[7]);
                    settings.filterCutoffHz = std::stod(parts[8]);
                    settings.resonance = std::stod(parts[9]);
                    settings.mixerInsert = std::stoi(parts[10]);
                    settings.routeTarget = std::stoi(parts[11]);
                    settings.reverse = parseBoolToken(parts[12]);
                    settings.timeStretch = parseBoolToken(parts[13]);
                    settings.pluginRack = parts[14].empty() ? std::vector<std::string>{} : splitString(parts[14], ',');
                    settings.presets = parts.size() >= 16 && !parts[15].empty() ? splitString(parts[15], ',') : std::vector<std::string>{};
                }

                loadedProject.channelSettings.push_back(std::move(settings));
            }
            else if (parts[0] == "AUTOCLIP" && parts.size() >= 6)
            {
                AutomationClipState clip{};
                clip.clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                clip.target = parts[2];
                if (parts.size() >= 7)
                {
                    clip.trackId = static_cast<std::uint32_t>(std::stoul(parts[3]));
                    clip.lane = std::stoi(parts[4]);
                    clip.startCell = std::stoi(parts[5]);
                    clip.lengthCells = std::stoi(parts[6]);
                }
                else
                {
                    clip.lane = std::stoi(parts[3]);
                    clip.startCell = std::stoi(parts[4]);
                    clip.lengthCells = std::stoi(parts[5]);
                }
                loadedProject.automationClips.push_back(std::move(clip));
            }
            else if (parts[0] == "AUTOPOINT" && parts.size() >= 4)
            {
                const std::uint32_t clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                auto automationIt = std::find_if(
                    loadedProject.automationClips.begin(),
                    loadedProject.automationClips.end(),
                    [&](const AutomationClipState& clip) { return clip.clipId == clipId; });
                if (automationIt != loadedProject.automationClips.end())
                {
                    AutomationPointState point{};
                    point.cell = std::stoi(parts[2]);
                    point.value = std::stoi(parts[3]);
                    point.curve = parts.size() >= 5 ? std::stod(parts[4]) : 0.0;
                    automationIt->points.push_back(point);
                }
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
        BusState mainBus{};
        mainBus.busId = 1;
        mainBus.name = "Main Bus";
        loadedProject.buses.push_back(std::move(mainBus));
    }

    if (loadedProject.tracks.empty())
    {
        TrackState track{};
        track.trackId = 1;
        track.busId = loadedProject.buses.front().busId;
        track.name = "Track 1";
        loadedProject.tracks.push_back(std::move(track));
    }

    if (loadedProject.autosavePath.empty())
    {
        loadedProject.autosavePath = makeAutosavePathForSession(resolvedPath);
    }
    if (loadedProject.recoveryPath.empty())
    {
        loadedProject.recoveryPath = loadedProject.autosavePath;
    }

    for (auto& bus : loadedProject.buses)
    {
        bus.inputTrackIds.clear();
    }

    for (auto& track : loadedProject.tracks)
    {
        if (track.busId == 0)
        {
            track.busId = loadedProject.buses.front().busId;
        }

        track.clipIds.clear();

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
    synchronizeProjectMusicalState();
    projectState_.dirty = false;
    projectState_.revision = std::max<std::uint64_t>(1, projectState_.revision + 1);
    projectState_.lastSavedRevision = std::max(projectState_.lastSavedRevision, projectState_.revision);
    projectState_.recoveryAvailable =
        projectState_.autosaveEnabled &&
        !projectState_.autosavePath.empty() &&
        std::filesystem::exists(projectState_.autosavePath);
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
    synchronizeProjectMusicalState();
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
    synchronizeProjectMusicalState();
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
    projectState_.autosaveEnabled = true;
    projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    projectState_.recoveryPath = projectState_.autosavePath;
    projectState_.lastSavedRevision = projectState_.revision;

    BusState mainBus{};
    mainBus.busId = 1;
    mainBus.name = "Main Bus";
    mainBus.gain = 1.0;
    mainBus.pan = 0.0;

    TrackState track{};
    track.trackId = 1;
    track.busId = mainBus.busId;
    track.name = "Track 1";
    track.gain = 0.92;
    track.pan = 0.0;
    track.recordEnabled = false;

    ClipState clip{};
    clip.clipId = 1;
    clip.trackId = track.trackId;
    clip.name = "Demo Clip";
    clip.sourceType = ClipSourceType::GeneratedTone;
    clip.patternNumber = 1;
    clip.startTimeSeconds = 0.0;
    clip.durationSeconds = 8.0;
    clip.gain = 0.7;
    clip.loopEnabled = true;
    clip.timeStretchEnabled = true;

    PlaylistItemState item{};
    item.itemId = 1;
    item.trackId = track.trackId;
    item.type = PlaylistItemType::PatternClip;
    item.sourceClipId = clip.clipId;
    item.patternNumber = clip.patternNumber;
    item.label = "Pattern 1";
    item.startTimeSeconds = 0.0;
    item.durationSeconds = 8.0;
    item.gain = 0.84;
    item.loopEnabled = true;
    item.timeStretchEnabled = true;

    track.clipIds.push_back(item.itemId);
    mainBus.inputTrackIds.push_back(track.trackId);

    projectState_.buses.push_back(mainBus);
    projectState_.tracks.push_back(track);
    projectState_.clips.push_back(clip);
    projectState_.playlistItems.push_back(item);
    projectState_.markers.push_back(MarkerState{1, "Start", 0.0});
    synchronizeProjectMusicalState();
    return true;
}

void AudioEngine::synchronizeProjectMusicalState()
{
    if (projectState_.autosavePath.empty())
    {
        projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    }
    if (projectState_.recoveryPath.empty())
    {
        projectState_.recoveryPath = projectState_.autosavePath;
    }

    if (projectState_.playlistItems.empty())
    {
        for (const auto& clip : projectState_.clips)
        {
            PlaylistItemState item{};
            item.itemId = nextPlaylistItemId();
            item.trackId = clip.trackId;
            item.type = clip.sourceType == ClipSourceType::AudioFile
                ? PlaylistItemType::AudioClip
                : PlaylistItemType::PatternClip;
            item.sourceClipId = clip.clipId;
            item.patternNumber = clip.patternNumber > 0 ? clip.patternNumber : extractPatternNumberFromClipName(clip.name);
            item.label = clip.name;
            item.startTimeSeconds = clip.startTimeSeconds;
            item.durationSeconds = clip.durationSeconds;
            item.trimStartSeconds = clip.trimStartSeconds;
            item.trimEndSeconds = clip.trimEndSeconds;
            item.fadeInSeconds = clip.fadeInSeconds;
            item.fadeOutSeconds = clip.fadeOutSeconds;
            item.gain = clip.gain;
            item.pan = clip.pan;
            item.pitchSemitones = clip.pitchSemitones;
            item.stretchRatio = clip.stretchRatio <= 0.0 ? 1.0 : clip.stretchRatio;
            item.muted = clip.muted;
            item.loopEnabled = clip.loopEnabled;
            item.timeStretchEnabled = clip.timeStretchEnabled;
            projectState_.playlistItems.push_back(std::move(item));
        }
    }

    for (auto& item : projectState_.playlistItems)
    {
        item.durationSeconds = std::max(0.125, item.durationSeconds);
        item.trimStartSeconds = std::max(0.0, item.trimStartSeconds);
        item.trimEndSeconds = std::max(0.0, item.trimEndSeconds);
        item.fadeInSeconds = std::max(0.0, item.fadeInSeconds);
        item.fadeOutSeconds = std::max(0.0, item.fadeOutSeconds);
        item.gain = std::clamp(item.gain, 0.0, 4.0);
        item.pan = std::clamp(item.pan, -1.0, 1.0);
        item.stretchRatio = item.stretchRatio <= 0.0 ? 1.0 : item.stretchRatio;

        if ((item.type == PlaylistItemType::PatternClip || item.type == PlaylistItemType::AudioClip) && item.sourceClipId != 0)
        {
            if (const ClipState* sourceClip = findClipState(item.sourceClipId); sourceClip != nullptr)
            {
                if (item.label.empty())
                {
                    item.label = sourceClip->name;
                }
                if (item.patternNumber == 0)
                {
                    item.patternNumber = sourceClip->patternNumber > 0
                        ? sourceClip->patternNumber
                        : extractPatternNumberFromClipName(sourceClip->name);
                }
            }
        }
    }

    for (auto& automationClip : projectState_.automationClips)
    {
        automationClip.trackId = automationClip.trackId == 0 && !projectState_.tracks.empty()
            ? projectState_.tracks.front().trackId
            : automationClip.trackId;

        auto playlistAutomationIt = std::find_if(
            projectState_.playlistItems.begin(),
            projectState_.playlistItems.end(),
            [&](const PlaylistItemState& item)
            {
                return item.type == PlaylistItemType::AutomationClip &&
                    item.automationClipId == automationClip.clipId;
            });

        if (playlistAutomationIt == projectState_.playlistItems.end())
        {
            PlaylistItemState item{};
            item.itemId = automationClip.clipId;
            item.trackId = automationClip.trackId;
            item.type = PlaylistItemType::AutomationClip;
            item.automationClipId = automationClip.clipId;
            item.label = automationClip.target;
            item.startTimeSeconds = static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds;
            item.durationSeconds = static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds;
            item.gain = 1.0;
            projectState_.playlistItems.push_back(std::move(item));
        }
        else
        {
            playlistAutomationIt->itemId = automationClip.clipId;
            playlistAutomationIt->trackId = automationClip.trackId;
            playlistAutomationIt->label = automationClip.target;
            playlistAutomationIt->startTimeSeconds =
                static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds;
            playlistAutomationIt->durationSeconds =
                static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds;
        }
    }

    for (auto& clip : projectState_.clips)
    {
        clip.patternNumber = clip.patternNumber > 0 ? clip.patternNumber : extractPatternNumberFromClipName(clip.name);
        clip.trimStartSeconds = std::max(0.0, clip.trimStartSeconds);
        clip.trimEndSeconds = std::max(0.0, clip.trimEndSeconds);
        clip.fadeInSeconds = std::max(0.0, clip.fadeInSeconds);
        clip.fadeOutSeconds = std::max(0.0, clip.fadeOutSeconds);
        clip.gain = std::clamp(clip.gain, 0.0, 4.0);
        clip.pan = std::clamp(clip.pan, -1.0, 1.0);
        clip.stretchRatio = clip.stretchRatio <= 0.0 ? 1.0 : clip.stretchRatio;
    }

    if (projectState_.patterns.empty())
    {
        for (int patternNumber = 1; patternNumber <= kDefaultPatternCount; ++patternNumber)
        {
            projectState_.patterns.push_back(makeDefaultPatternState(patternNumber));
        }
    }

    for (auto& pattern : projectState_.patterns)
    {
        std::vector<PatternLaneState> synchronizedLanes;
        synchronizedLanes.reserve(projectState_.tracks.size());

        for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
        {
            const TrackState& track = projectState_.tracks[trackIndex];
            PatternLaneState* existingLane = findPatternLane(pattern, track.trackId);
            if (existingLane != nullptr)
            {
                if (existingLane->steps.size() != static_cast<std::size_t>(kDefaultStepCount))
                {
                    existingLane->steps.resize(static_cast<std::size_t>(kDefaultStepCount));
                }
                synchronizedLanes.push_back(*existingLane);
            }
            else
            {
                synchronizedLanes.push_back(makeDefaultPatternLane(track.trackId, pattern.patternNumber, trackIndex));
            }
        }

        pattern.lanes = std::move(synchronizedLanes);
    }

    std::vector<ChannelSettingsState> synchronizedSettings;
    synchronizedSettings.reserve(projectState_.tracks.size());
    for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
    {
        const TrackState& track = projectState_.tracks[trackIndex];
        ChannelSettingsState* settings = findChannelSettings(track.trackId);
        if (settings != nullptr)
        {
            settings->name = track.name;
            synchronizedSettings.push_back(*settings);
        }
        else
        {
            synchronizedSettings.push_back(makeDefaultChannelSettings(track.trackId, trackIndex));
        }
    }
    projectState_.channelSettings = std::move(synchronizedSettings);

    for (auto& track : projectState_.tracks)
    {
        track.gain = std::clamp(track.gain, 0.0, 4.0);
        track.pan = std::clamp(track.pan, -1.0, 1.0);
        track.sendAmount = std::clamp(track.sendAmount, 0.0, 1.0);
        track.clipIds.clear();
        for (const auto& item : projectState_.playlistItems)
        {
            if (item.trackId == track.trackId && item.type != PlaylistItemType::AutomationClip)
            {
                track.clipIds.push_back(item.itemId);
            }
        }
    }

    if (projectState_.automationClips.empty())
    {
        projectState_.automationClips.push_back(AutomationClipState{
            nextAutomationClipId(),
            "Bus:1:gain",
            {{0, 84, 0.0}, {8, 78, 0.0}, {16, 90, 0.0}, {24, 82, 0.0}},
            projectState_.tracks.empty() ? 0u : projectState_.tracks.front().trackId,
            static_cast<int>(projectState_.tracks.size()),
            0,
            8});

        const AutomationClipState& automationClip = projectState_.automationClips.back();
        projectState_.playlistItems.push_back(PlaylistItemState{
            automationClip.clipId,
            automationClip.trackId,
            PlaylistItemType::AutomationClip,
            0,
            automationClip.clipId,
            0,
            automationClip.target,
            static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds,
            static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            1.0,
            false,
            false,
            false});
    }
}

AudioEngine::PatternState* AudioEngine::findPatternState(int patternNumber)
{
    auto it = std::find_if(
        projectState_.patterns.begin(),
        projectState_.patterns.end(),
        [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
    return it == projectState_.patterns.end() ? nullptr : &(*it);
}

const AudioEngine::PatternState* AudioEngine::findPatternState(int patternNumber) const
{
    auto it = std::find_if(
        projectState_.patterns.begin(),
        projectState_.patterns.end(),
        [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
    return it == projectState_.patterns.end() ? nullptr : &(*it);
}

AudioEngine::PatternLaneState* AudioEngine::findPatternLane(PatternState& pattern, std::uint32_t trackId)
{
    auto it = std::find_if(
        pattern.lanes.begin(),
        pattern.lanes.end(),
        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
    return it == pattern.lanes.end() ? nullptr : &(*it);
}

const AudioEngine::PatternLaneState* AudioEngine::findPatternLane(const PatternState& pattern, std::uint32_t trackId) const
{
    auto it = std::find_if(
        pattern.lanes.begin(),
        pattern.lanes.end(),
        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
    return it == pattern.lanes.end() ? nullptr : &(*it);
}

AudioEngine::ClipState* AudioEngine::findClipState(std::uint32_t clipId)
{
    auto it = std::find_if(
        projectState_.clips.begin(),
        projectState_.clips.end(),
        [&](const ClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.clips.end() ? nullptr : &(*it);
}

const AudioEngine::ClipState* AudioEngine::findClipState(std::uint32_t clipId) const
{
    auto it = std::find_if(
        projectState_.clips.begin(),
        projectState_.clips.end(),
        [&](const ClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.clips.end() ? nullptr : &(*it);
}

AudioEngine::PlaylistItemState* AudioEngine::findPlaylistItem(std::uint32_t itemId)
{
    auto it = std::find_if(
        projectState_.playlistItems.begin(),
        projectState_.playlistItems.end(),
        [&](const PlaylistItemState& item) { return item.itemId == itemId; });
    return it == projectState_.playlistItems.end() ? nullptr : &(*it);
}

const AudioEngine::PlaylistItemState* AudioEngine::findPlaylistItem(std::uint32_t itemId) const
{
    auto it = std::find_if(
        projectState_.playlistItems.begin(),
        projectState_.playlistItems.end(),
        [&](const PlaylistItemState& item) { return item.itemId == itemId; });
    return it == projectState_.playlistItems.end() ? nullptr : &(*it);
}

AudioEngine::BusState* AudioEngine::findBusState(std::uint32_t busId)
{
    auto it = std::find_if(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [&](const BusState& bus) { return bus.busId == busId; });
    return it == projectState_.buses.end() ? nullptr : &(*it);
}

const AudioEngine::BusState* AudioEngine::findBusState(std::uint32_t busId) const
{
    auto it = std::find_if(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [&](const BusState& bus) { return bus.busId == busId; });
    return it == projectState_.buses.end() ? nullptr : &(*it);
}

AudioEngine::ChannelSettingsState* AudioEngine::findChannelSettings(std::uint32_t trackId)
{
    auto it = std::find_if(
        projectState_.channelSettings.begin(),
        projectState_.channelSettings.end(),
        [&](const ChannelSettingsState& settings) { return settings.trackId == trackId; });
    return it == projectState_.channelSettings.end() ? nullptr : &(*it);
}

const AudioEngine::ChannelSettingsState* AudioEngine::findChannelSettings(std::uint32_t trackId) const
{
    auto it = std::find_if(
        projectState_.channelSettings.begin(),
        projectState_.channelSettings.end(),
        [&](const ChannelSettingsState& settings) { return settings.trackId == trackId; });
    return it == projectState_.channelSettings.end() ? nullptr : &(*it);
}

AudioEngine::AutomationClipState* AudioEngine::findAutomationClip(std::uint32_t clipId)
{
    auto it = std::find_if(
        projectState_.automationClips.begin(),
        projectState_.automationClips.end(),
        [&](const AutomationClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.automationClips.end() ? nullptr : &(*it);
}

double AudioEngine::evaluateAutomationTarget(const std::string& target, double timelineSeconds, double defaultValue) const
{
    if (target.empty())
    {
        return defaultValue;
    }

    const std::string desiredTarget = toLowerCopy(target);
    const double timelineCell = timelineSeconds / kPlaylistCellSeconds;

    auto convertValue =
        [&](double normalizedValue, const std::string& normalizedTarget) -> double
    {
        const double clamped = std::clamp(normalizedValue, 0.0, 1.0);
        if (normalizedTarget.find(":pan") != std::string::npos)
        {
            return (clamped * 2.0) - 1.0;
        }
        if (normalizedTarget.find(":cutoff") != std::string::npos)
        {
            return 200.0 + (clamped * 15800.0);
        }
        if (normalizedTarget.find(":gain") != std::string::npos || normalizedTarget == "master volume")
        {
            return clamped;
        }
        return clamped;
    };

    for (const auto& automationClip : projectState_.automationClips)
    {
        const std::string clipTarget = toLowerCopy(automationClip.target);
        const bool targetMatches =
            clipTarget == desiredTarget ||
            (desiredTarget == "master:gain" && (clipTarget == "master volume" || clipTarget == "bus:1:gain")) ||
            (desiredTarget == "bus:1:gain" && clipTarget == "master volume");

        if (!targetMatches || automationClip.points.empty())
        {
            continue;
        }

        const double clipStartCell = static_cast<double>(std::max(0, automationClip.startCell));
        const double clipEndCell = clipStartCell + static_cast<double>(std::max(1, automationClip.lengthCells));
        if (timelineCell < clipStartCell || timelineCell > clipEndCell)
        {
            continue;
        }

        const AutomationPointState* previousPoint = &automationClip.points.front();
        const AutomationPointState* nextPoint = previousPoint;

        for (const auto& point : automationClip.points)
        {
            if (point.cell <= timelineCell)
            {
                previousPoint = &point;
            }
            if (point.cell >= timelineCell)
            {
                nextPoint = &point;
                break;
            }
        }

        if (nextPoint == nullptr)
        {
            nextPoint = previousPoint;
        }

        if (previousPoint == nullptr || nextPoint == nullptr)
        {
            continue;
        }

        if (previousPoint == nextPoint || nextPoint->cell == previousPoint->cell)
        {
            return convertValue(static_cast<double>(previousPoint->value) / 100.0, desiredTarget);
        }

        const double t =
            (timelineCell - static_cast<double>(previousPoint->cell)) /
            static_cast<double>(std::max(1, nextPoint->cell - previousPoint->cell));
        const double interpolatedValue =
            static_cast<double>(previousPoint->value) +
            ((static_cast<double>(nextPoint->value) - static_cast<double>(previousPoint->value)) * t);
        return convertValue(interpolatedValue / 100.0, desiredTarget);
    }

    return defaultValue;
}

bool AudioEngine::hasSoloedTracks() const
{
    return std::any_of(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [](const TrackState& track) { return track.solo; });
}

bool AudioEngine::hasSoloedBuses() const
{
    return std::any_of(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [](const BusState& bus) { return bus.solo; });
}

AudioEngine::PatternLaneState AudioEngine::makeDefaultPatternLane(std::uint32_t trackId, int patternNumber, std::size_t laneIndex) const
{
    PatternLaneState lane{};
    lane.trackId = trackId;
    lane.steps.resize(static_cast<std::size_t>(kDefaultStepCount));

    for (int stepIndex = 0; stepIndex < kDefaultStepCount; ++stepIndex)
    {
        const int patternOffset = patternNumber - 1;
        const bool onQuarter = ((stepIndex + patternOffset) % 4) == 0;
        const bool grooveHit =
            ((stepIndex + static_cast<int>(laneIndex) + patternOffset) % (patternNumber + 3)) == 0;
        lane.steps[static_cast<std::size_t>(stepIndex)].enabled = onQuarter || grooveHit;
        lane.steps[static_cast<std::size_t>(stepIndex)].velocity =
            68 + ((stepIndex * (patternNumber + 6) + static_cast<int>(laneIndex) * 11) % 56);
    }

    lane.swing = static_cast<int>((laneIndex * 9 + static_cast<std::size_t>(patternNumber) * 5) % 48);
    lane.shuffle = static_cast<int>((laneIndex * 5 + static_cast<std::size_t>(patternNumber) * 7) % 32);

    const int rootLane = 12 + static_cast<int>((laneIndex * 3) % 7);
    lane.notes.push_back(MidiNoteState{rootLane, 0, 3, 98, true, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 4, 4, 2, 84, false, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 7, 8, 4, 102, false, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 12, 14, 2, 92, false, (laneIndex % 2) == 0, false});
    return lane;
}

AudioEngine::PatternState AudioEngine::makeDefaultPatternState(int patternNumber) const
{
    PatternState pattern{};
    pattern.patternNumber = patternNumber;
    pattern.name = "Pattern " + std::to_string(patternNumber);
    pattern.lengthInBars = 2 + ((patternNumber - 1) % 3);
    pattern.accentAmount = 8 * patternNumber;

    for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
    {
        pattern.lanes.push_back(
            makeDefaultPatternLane(
                projectState_.tracks[trackIndex].trackId,
                patternNumber,
                trackIndex + static_cast<std::size_t>(patternNumber - 1)));
    }

    return pattern;
}

AudioEngine::ChannelSettingsState AudioEngine::makeDefaultChannelSettings(std::uint32_t trackId, std::size_t laneIndex) const
{
    ChannelSettingsState settings{};
    settings.trackId = trackId;

    auto trackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == trackId; });
    settings.name = trackIt == projectState_.tracks.end() ? ("Channel " + std::to_string(trackId)) : trackIt->name;
    settings.generatorType = laneIndex % 3 == 2 ? GeneratorType::PluginInstrument : (laneIndex % 2 == 0 ? GeneratorType::Sampler : GeneratorType::TestSynth);
    settings.gain = 0.78 + (0.03 * static_cast<double>(laneIndex % 3));
    settings.pan = laneIndex % 3 == 0 ? -0.08 : (laneIndex % 4 == 0 ? 0.10 : 0.0);
    settings.pitchSemitones = static_cast<double>((laneIndex % 5) - 2);
    settings.attackMs = 10.0 + static_cast<double>(laneIndex * 2);
    settings.decayMs = 64.0 + static_cast<double>(laneIndex * 8);
    settings.sustainLevel = 0.72 + (0.04 * static_cast<double>(laneIndex % 3));
    settings.releaseMs = 160.0 + static_cast<double>(laneIndex * 18);
    settings.filterCutoffHz = 7600.0 + static_cast<double>((laneIndex % 4) * 900);
    settings.resonance = 0.18 + (0.04 * static_cast<double>(laneIndex % 3));
    settings.mixerInsert = static_cast<int>(laneIndex + 1);
    settings.routeTarget = 0;
    settings.reverse = false;
    settings.timeStretch = true;
    settings.sampleFilePath.clear();
    settings.instrumentPluginName = settings.generatorType == GeneratorType::PluginInstrument ? "Future VSTi Rack" : "";
    settings.pluginRack = {"Sampler", laneIndex % 2 == 0 ? "Transient" : "EQ"};
    settings.presets = {"Init", "Tight", "Wide"};
    return settings;
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
            bus.gain,
            false,
            true,
            bus.muted,
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
            track.gain,
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

        if (track.sendBusId != 0 && track.sendAmount > 0.0 && findBusState(track.sendBusId) != nullptr)
        {
            graph.nodes.push_back(GraphNode{
                sendNodeId(track.trackId),
                NodeType::Send,
                track.name + " Send",
                0,
                track.trackId,
                track.sendBusId,
                0,
                std::clamp(track.sendAmount, 0.0, 1.0),
                liveTrack,
                !liveTrack,
                false,
                true});
        }

        graph.edges.push_back(GraphEdge{kInputNodeId, trackNodeId(track.trackId)});
        graph.edges.push_back(GraphEdge{trackNodeId(track.trackId), pluginNodeId(track.trackId)});

        const std::uint32_t routeBusId =
            track.routeTargetBusId != 0 && findBusState(track.routeTargetBusId) != nullptr
                ? track.routeTargetBusId
                : track.busId;
        graph.edges.push_back(GraphEdge{pluginNodeId(track.trackId), busNodeId(routeBusId)});

        if (track.sendBusId != 0 && track.sendBusId != routeBusId && findBusState(track.sendBusId) != nullptr)
        {
            graph.edges.push_back(GraphEdge{pluginNodeId(track.trackId), sendNodeId(track.trackId)});
            graph.edges.push_back(GraphEdge{sendNodeId(track.trackId), busNodeId(track.sendBusId)});
        }
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
        if (bus.outputBusId != 0 && bus.outputBusId != bus.busId && findBusState(bus.outputBusId) != nullptr)
        {
            graph.edges.push_back(GraphEdge{busNodeId(bus.busId), busNodeId(bus.outputBusId)});
        }
        else
        {
            graph.edges.push_back(GraphEdge{busNodeId(bus.busId), kOutputNodeId});
        }
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
        if (!running_.load() || audioThreadState_.stopRequested.load())
        {
            break;
        }

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

            if (transportInfo_.loopEnabled && transportInfo_.loopEndSample > transportInfo_.loopStartSample)
            {
                const std::uint64_t loopLength = transportInfo_.loopEndSample - transportInfo_.loopStartSample;
                if (transportInfo_.samplePosition >= transportInfo_.loopEndSample && loopLength > 0)
                {
                    const std::uint64_t wrappedOffset =
                        (transportInfo_.samplePosition - transportInfo_.loopStartSample) % loopLength;
                    transportInfo_.samplePosition = transportInfo_.loopStartSample + wrappedOffset;
                }
            }

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

    if (state_.load() != EngineState::Error)
    {
        state_.store(EngineState::Stopped);
    }

    updateStatusFromState();
    publishSnapshot();

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
        if (!running_.load())
        {
            if (audioThread_.joinable() && !audioThreadState_.running.load())
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

            processPendingCommands();

            if (!running_.load() && graphSwapPending_)
            {
                swapCompiledGraphAtSafePoint();
            }
        }

        serviceSandboxWatchdogs();
        serviceClipCacheEviction();
        maybeAutosaveProject();
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
    const double sampleRate = static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double blockStartSeconds =
        static_cast<double>(renderContext.callback.transportSampleStart) / sampleRate;

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

            if (graphNode.type == NodeType::Bus)
            {
                const BusState* bus = findBusState(graphNode.busId);
                if (bus == nullptr || bus->muted || (hasSoloedBuses() && !bus->solo))
                {
                    nodeBuffer.clear();
                }
                else
                {
                    const double busGain = evaluateAutomationTarget(
                        "Bus:" + std::to_string(bus->busId) + ":gain",
                        blockStartSeconds,
                        1.0);
                    const double busPan = evaluateAutomationTarget(
                        "Bus:" + std::to_string(bus->busId) + ":pan",
                        blockStartSeconds,
                        bus->pan);
                    const auto [leftPan, rightPan] = stereoPanGains(busPan);
                    for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                    {
                        nodeBuffer.left[frame] *= leftPan * busGain;
                        nodeBuffer.right[frame] *= rightPan * busGain;
                    }
                }
            }
            else if (graphNode.type == NodeType::Output || graphNode.type == NodeType::RenderSink)
            {
                const double masterGain = evaluateAutomationTarget("Master:gain", blockStartSeconds, 1.0);
                for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                {
                    nodeBuffer.left[frame] *= masterGain;
                    nodeBuffer.right[frame] *= masterGain;
                }
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

    if (trackIt == projectState_.tracks.end() || trackIt->muted || (hasSoloedTracks() && !trackIt->solo))
    {
        return;
    }

    const std::uint64_t blockStartSample = renderContext.callback.transportSampleStart;
    const double sampleRate = static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double blockStartSeconds = static_cast<double>(blockStartSample) / sampleRate;
    const ChannelSettingsState* channelSettings = findChannelSettings(trackId);

    ChannelSettingsState defaultChannelSettings{};
    defaultChannelSettings.trackId = trackId;
    defaultChannelSettings.name = trackIt->name;
    const ChannelSettingsState& settings = channelSettings == nullptr ? defaultChannelSettings : *channelSettings;
    const double trackGainAutomation = evaluateAutomationTarget(
        "Track:" + std::to_string(trackId) + ":gain",
        blockStartSeconds,
        1.0);
    const double trackPan = evaluateAutomationTarget(
        "Track:" + std::to_string(trackId) + ":pan",
        blockStartSeconds,
        trackIt->pan);
    const double channelGain = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":gain",
        blockStartSeconds,
        settings.gain);
    const double channelPan = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":pan",
        blockStartSeconds,
        settings.pan);
    const double cutoffHz = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":cutoff",
        blockStartSeconds,
        settings.filterCutoffHz);

    for (const std::uint32_t itemId : trackIt->clipIds)
    {
        const PlaylistItemState* item = findPlaylistItem(itemId);
        if (item == nullptr || item->muted || item->type == PlaylistItemType::AutomationClip)
        {
            continue;
        }

        const ClipState* sourceClip = item->sourceClipId == 0 ? nullptr : findClipState(item->sourceClipId);
        if (sourceClip == nullptr && item->type == PlaylistItemType::AudioClip)
        {
            continue;
        }

        const std::uint64_t clipStartSample = static_cast<std::uint64_t>(item->startTimeSeconds * sampleRate);
        const std::uint64_t clipEndSample = clipStartSample + static_cast<std::uint64_t>(item->durationSeconds * sampleRate);
        const double trimStartSeconds = item->trimStartSeconds > 0.0
            ? item->trimStartSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->trimStartSeconds);
        const double trimEndSeconds = item->trimEndSeconds > 0.0
            ? item->trimEndSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->trimEndSeconds);
        const double fadeInSeconds = item->fadeInSeconds > 0.0
            ? item->fadeInSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->fadeInSeconds);
        const double fadeOutSeconds = item->fadeOutSeconds > 0.0
            ? item->fadeOutSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->fadeOutSeconds);

        for (std::uint32_t frame = 0; frame < destination.frameCount; ++frame)
        {
            const std::uint64_t projectSample = blockStartSample + frame;
            if (projectSample < clipStartSample || projectSample >= clipEndSample)
            {
                continue;
            }

            const double localTimeSeconds = static_cast<double>(projectSample - clipStartSample) / sampleRate;
            const double fadeGain = computeFadeGain(
                localTimeSeconds,
                item->durationSeconds,
                fadeInSeconds,
                fadeOutSeconds);
            const double totalPan = std::clamp(trackPan + channelPan + item->pan, -1.0, 1.0);
            const auto [leftPan, rightPan] = stereoPanGains(totalPan);
            const double totalGain =
                std::clamp(item->gain, 0.0, 4.0) *
                std::clamp(trackGainAutomation, 0.0, 2.0) *
                std::clamp(channelGain, 0.0, 2.0) *
                fadeGain;
            double leftSample = 0.0;
            double rightSample = 0.0;

            if (item->type == PlaylistItemType::PatternClip)
            {
                const int patternNumber = item->patternNumber != 0
                    ? item->patternNumber
                    : (sourceClip == nullptr ? 1 : sourceClip->patternNumber);
                const PatternState* pattern = findPatternState(patternNumber);
                const PatternLaneState* patternLane =
                    pattern == nullptr ? nullptr : findPatternLane(*pattern, trackId);

                if (patternLane != nullptr)
                {
                    const double secondsPerBeat = 60.0 / std::max(1.0, transportInfo_.tempoBpm);
                    const double patternDurationSeconds =
                        static_cast<double>(std::max(1, pattern->lengthInBars)) * 4.0 * secondsPerBeat;
                    double patternPlaybackSeconds = localTimeSeconds;

                    if (item->timeStretchEnabled && item->durationSeconds > 0.0 && patternDurationSeconds > 0.0)
                    {
                        patternPlaybackSeconds =
                            (localTimeSeconds / item->durationSeconds) *
                            patternDurationSeconds *
                            std::max(0.125, item->stretchRatio);
                    }

                    if (!item->timeStretchEnabled)
                    {
                        patternPlaybackSeconds = localTimeSeconds * std::max(0.125, item->stretchRatio);
                    }

                    if (item->loopEnabled && patternDurationSeconds > 0.0)
                    {
                        patternPlaybackSeconds = std::fmod(patternPlaybackSeconds, patternDurationSeconds);
                        if (patternPlaybackSeconds < 0.0)
                        {
                            patternPlaybackSeconds += patternDurationSeconds;
                        }
                    }

                    const double cellDurationSeconds =
                        patternDurationSeconds / static_cast<double>(std::max(1, kDefaultPlaylistCellCount));

                    auto renderVoice =
                        [&](int laneIndex, int stepIndex, int lengthCells, int velocity, bool accent, bool slide)
                    {
                        const double noteStartSeconds = static_cast<double>(std::max(0, stepIndex)) * cellDurationSeconds;
                        const double noteBodyDurationSeconds =
                            static_cast<double>(std::max(1, lengthCells)) * cellDurationSeconds;
                        const double noteEndSeconds =
                            noteStartSeconds +
                            noteBodyDurationSeconds +
                            std::max(0.0, settings.releaseMs / 1000.0);

                        if (patternPlaybackSeconds < noteStartSeconds || patternPlaybackSeconds >= noteEndSeconds)
                        {
                            return;
                        }

                        const double noteElapsedSeconds = patternPlaybackSeconds - noteStartSeconds;
                        const double envelopeGain =
                            computeEnvelopeGain(noteElapsedSeconds, noteBodyDurationSeconds, settings);
                        if (envelopeGain <= 0.0)
                        {
                            return;
                        }

                        const double velocityGain =
                            std::clamp(static_cast<double>(velocity) / 127.0, 0.05, 1.0);
                        const double accentGain = accent ? 1.12 : 1.0;
                        const double pitchSemitones =
                            settings.pitchSemitones +
                            item->pitchSemitones +
                            (sourceClip == nullptr ? 0.0 : sourceClip->pitchSemitones);
                        const double midiNote =
                            static_cast<double>(36 + std::clamp(laneIndex, 0, kDefaultPianoLaneCount - 1)) +
                            pitchSemitones;
                        const double frequency =
                            440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
                        const double cutoffNorm = std::clamp(cutoffHz / 16000.0, 0.08, 1.0);

                        double voiceLeft = 0.0;
                        double voiceRight = 0.0;
                        bool renderedFromSamplerSource = false;

                        if (settings.generatorType == GeneratorType::Sampler && !settings.sampleFilePath.empty())
                        {
                            const std::shared_ptr<const DecodedWavData> samplerData =
                                getSamplerSourceData(settings.sampleFilePath);
                            if (samplerData != nullptr && !samplerData->left.empty())
                            {
                                const double pitchRatio = std::pow(2.0, ((midiNote - 60.0) / 12.0));
                                double sampleIndex =
                                    noteElapsedSeconds *
                                    static_cast<double>(samplerData->sampleRate) *
                                    pitchRatio;
                                if (settings.reverse)
                                {
                                    sampleIndex = static_cast<double>(samplerData->left.size() - 1) - sampleIndex;
                                }
                                voiceLeft = linearSampleAt(samplerData->left, sampleIndex);
                                voiceRight = linearSampleAt(samplerData->right, sampleIndex);
                                renderedFromSamplerSource = true;
                            }
                        }

                        if (!renderedFromSamplerSource)
                        {
                            const double phase = kTwoPi * frequency * noteElapsedSeconds;

                            if (settings.generatorType == GeneratorType::Sampler)
                            {
                                const double transient = std::exp(-noteElapsedSeconds * 10.0);
                                const double body = std::sin(phase) * 0.68;
                                const double overtone = std::sin(phase * 2.0) * 0.18 * cutoffNorm;
                                const double low = std::sin(phase * 0.5) * 0.14;
                                const double wave = (body + overtone + low) * transient;
                                voiceLeft = wave;
                                voiceRight = wave;
                            }
                            else if (settings.generatorType == GeneratorType::PluginInstrument)
                            {
                                const double wave =
                                    (0.42 * std::sin(phase)) +
                                    (0.28 * std::asin(std::sin(phase * 1.01))) * cutoffNorm +
                                    (0.20 * std::sin(phase * 2.0)) +
                                    (slide ? 0.08 * std::sin(phase * 0.5) : 0.0);
                                voiceLeft = wave;
                                voiceRight = wave * (0.96 + (0.04 * cutoffNorm));
                            }
                            else
                            {
                                const double wave =
                                    (0.54 * std::sin(phase)) +
                                    (0.24 * std::sin(phase * 2.0) * cutoffNorm) +
                                    (0.14 * std::sin(phase * 3.0) * settings.resonance) +
                                    (0.12 * std::sin(phase * 0.5)) +
                                    (slide ? 0.05 * std::sin(phase * 1.5) : 0.0);
                                voiceLeft = wave;
                                voiceRight = wave;
                            }
                        }

                        const double voiceGain = envelopeGain * velocityGain * accentGain * 0.75;
                        leftSample += voiceLeft * voiceGain;
                        rightSample += voiceRight * voiceGain;
                    };

                    for (const auto& note : patternLane->notes)
                    {
                        renderVoice(
                            note.lane,
                            note.step,
                            note.length,
                            note.velocity,
                            note.accent,
                            note.slide);
                    }

                    for (std::size_t stepIndex = 0; stepIndex < patternLane->steps.size(); ++stepIndex)
                    {
                        const StepState& step = patternLane->steps[stepIndex];
                        if (!step.enabled)
                        {
                            continue;
                        }

                        const int rootLane = 8 + static_cast<int>((trackId + patternNumber) % 10);
                        renderVoice(
                            rootLane,
                            static_cast<int>(stepIndex),
                            1,
                            std::max(1, step.velocity),
                            step.velocity >= 112,
                            false);
                    }
                }
                else
                {
                    const double phase = kTwoPi * 110.0 * localTimeSeconds;
                    leftSample = std::sin(phase) * 0.35;
                    rightSample = leftSample;
                }
            }
            else if (sourceClip != nullptr && sourceClip->sourceType == ClipSourceType::AudioFile)
            {
                const std::shared_ptr<const DecodedWavData> audioData = getSamplerSourceData(sourceClip->filePath);
                if (audioData != nullptr && !audioData->left.empty())
                {
                    const double rawSourceDurationSeconds =
                        static_cast<double>(audioData->left.size()) /
                        static_cast<double>(std::max(1u, audioData->sampleRate));
                    const double sourcePlayableDurationSeconds =
                        std::max(0.001, rawSourceDurationSeconds - trimStartSeconds - trimEndSeconds);
                    double sourcePlaybackSeconds = localTimeSeconds;

                    if (item->timeStretchEnabled && item->durationSeconds > 0.0)
                    {
                        sourcePlaybackSeconds =
                            (localTimeSeconds / item->durationSeconds) *
                            sourcePlayableDurationSeconds;
                    }
                    else
                    {
                        sourcePlaybackSeconds = localTimeSeconds * std::max(0.125, item->stretchRatio);
                    }

                    const double pitchRatio = std::pow(2.0, item->pitchSemitones / 12.0);
                    sourcePlaybackSeconds *= std::max(0.125, pitchRatio);

                    if (!item->loopEnabled && sourcePlaybackSeconds >= sourcePlayableDurationSeconds)
                    {
                        continue;
                    }

                    if (item->loopEnabled && sourcePlayableDurationSeconds > 0.0)
                    {
                        sourcePlaybackSeconds = std::fmod(sourcePlaybackSeconds, sourcePlayableDurationSeconds);
                        if (sourcePlaybackSeconds < 0.0)
                        {
                            sourcePlaybackSeconds += sourcePlayableDurationSeconds;
                        }
                    }

                    const double sourceSampleIndex =
                        (trimStartSeconds + sourcePlaybackSeconds) *
                        static_cast<double>(audioData->sampleRate);
                    leftSample = linearSampleAt(audioData->left, sourceSampleIndex);
                    rightSample = linearSampleAt(audioData->right, sourceSampleIndex);
                }
            }

            destination.left[frame] += leftSample * leftPan * totalGain;
            destination.right[frame] += rightSample * rightPan * totalGain;
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

    for (const auto& item : projectState_.playlistItems)
    {
        if (item.type != PlaylistItemType::AudioClip || item.sourceClipId == 0)
        {
            continue;
        }

        const ClipState* clip = findClipState(item.sourceClipId);
        if (clip == nullptr || clip->sourceType != ClipSourceType::AudioFile || clip->filePath.empty())
        {
            continue;
        }

        if (item.startTimeSeconds > lookAheadSeconds ||
            (item.startTimeSeconds + item.durationSeconds) < blockStartSeconds)
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
                    return entry.clipId == clip->clipId && entry.complete;
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
                    return request.clipId == clip->clipId;
                });

            if (!alreadyQueued)
            {
                diskReadQueue_.push_back(DiskReadRequest{
                    clip->clipId,
                    clip->filePath,
                    0,
                    static_cast<std::uint32_t>(item.durationSeconds * static_cast<double>(renderContext.callback.sampleRate))});
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

std::uint32_t AudioEngine::nextPlaylistItemId() const
{
    std::uint32_t maxId = 0;
    for (const auto& item : projectState_.playlistItems)
    {
        if (item.type == PlaylistItemType::AutomationClip || item.itemId >= 4000)
        {
            continue;
        }
        maxId = std::max(maxId, item.itemId);
    }
    return std::max<std::uint32_t>(1, maxId + 1);
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

std::uint32_t AudioEngine::nextAutomationClipId() const
{
    std::uint32_t maxId = 4000;
    for (const auto& automationClip : projectState_.automationClips)
    {
        maxId = std::max(maxId, automationClip.clipId);
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

        case CommandType::ConfigureTransportLoop:
            configureTransportLoop(command.uintValue, command.secondaryUintValue, command.boolValue);
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

        case CommandType::AddPattern:
            addPattern(command.textValue);
            break;

        case CommandType::AddClipToTrack:
            addClipToTrack(static_cast<std::uint32_t>(command.uintValue), command.textValue);
            break;

        case CommandType::TogglePatternStep:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 3)
            {
                togglePatternStep(
                    std::stoi(parts[0]),
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    std::stoi(parts[2]));
            }
            break;
        }

        case CommandType::UpsertMidiNote:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 10)
            {
                MidiNoteState note{};
                note.lane = std::stoi(parts[3]);
                note.step = std::stoi(parts[4]);
                note.length = std::stoi(parts[5]);
                note.velocity = std::stoi(parts[6]);
                note.accent = parseBoolToken(parts[7]);
                note.slide = parseBoolToken(parts[8]);
                note.selected = false;
                upsertMidiNote(
                    std::stoi(parts[0]),
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    static_cast<std::size_t>(std::stoull(parts[2])),
                    note,
                    parseBoolToken(parts[9]));
            }
            break;
        }

        case CommandType::SetAutomationPoint:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 4)
            {
                setAutomationPoint(
                    static_cast<std::uint32_t>(std::stoul(parts[0])),
                    std::stoi(parts[1]),
                    std::stoi(parts[2]),
                    std::stoi(parts[3]));
            }
            break;
        }

        case CommandType::MoveClip:
            moveClip(
                static_cast<std::uint32_t>(command.uintValue),
                static_cast<std::uint32_t>(command.secondaryUintValue),
                command.doubleValue,
                command.textValue.empty() ? -1.0 : std::strtod(command.textValue.c_str(), nullptr));
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
