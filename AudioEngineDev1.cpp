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

    int hexDigitValue(char character)
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f')
        {
            return 10 + (character - 'a');
        }
        if (character >= 'A' && character <= 'F')
        {
            return 10 + (character - 'A');
        }
        return -1;
    }

    std::vector<std::string> splitString(const std::string& text, char delimiter)
    {
        if (text.empty())
        {
            return {};
        }

        std::vector<std::string> parts;
        std::string item;
        item.reserve(text.size());

        for (const char character : text)
        {
            if (character == delimiter)
            {
                parts.push_back(item);
                item.clear();
            }
            else
            {
                item.push_back(character);
            }
        }

        parts.push_back(item);
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

    std::string encodeProjectToken(const std::string& text, char delimiter)
    {
        static constexpr char kHexDigits[] = "0123456789ABCDEF";

        std::string encoded;
        encoded.reserve(text.size());

        for (const unsigned char value : text)
        {
            const bool mustEscape =
                value == static_cast<unsigned char>('%') ||
                value == static_cast<unsigned char>(delimiter) ||
                value == static_cast<unsigned char>('\r') ||
                value == static_cast<unsigned char>('\n');

            if (!mustEscape)
            {
                encoded.push_back(static_cast<char>(value));
                continue;
            }

            encoded.push_back('%');
            encoded.push_back(kHexDigits[(value >> 4) & 0x0F]);
            encoded.push_back(kHexDigits[value & 0x0F]);
        }

        return encoded;
    }

    std::string decodeProjectToken(const std::string& text)
    {
        std::string decoded;
        decoded.reserve(text.size());

        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] == '%' && (index + 2) < text.size())
            {
                const int highNibble = hexDigitValue(text[index + 1]);
                const int lowNibble = hexDigitValue(text[index + 2]);
                if (highNibble >= 0 && lowNibble >= 0)
                {
                    decoded.push_back(static_cast<char>((highNibble << 4) | lowNibble));
                    index += 2;
                    continue;
                }
            }

            decoded.push_back(text[index]);
        }

        return decoded;
    }

    std::string encodeProjectStringList(const std::vector<std::string>& values, char delimiter)
    {
        std::vector<std::string> encodedValues;
        encodedValues.reserve(values.size());

        for (const auto& value : values)
        {
            encodedValues.push_back(encodeProjectToken(value, delimiter));
        }

        return joinString(encodedValues, delimiter);
    }

    std::vector<std::string> decodeProjectStringList(const std::string& text, char delimiter)
    {
        if (text.empty())
        {
            return {};
        }

        std::vector<std::string> values = splitString(text, delimiter);
        for (auto& value : values)
        {
            value = decodeProjectToken(value);
        }
        return values;
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

bool AudioEngine::clonePattern(int patternNumber)
{
    const PatternState* sourcePattern = findPatternState(patternNumber);
    if (sourcePattern == nullptr)
    {
        setError(kErrorProjectIo, "Pattern not found while cloning pattern.");
        return false;
    }

    const ProjectState beforeState = projectState_;

    int nextPatternNumber = 1;
    for (const auto& pattern : projectState_.patterns)
    {
        nextPatternNumber = std::max(nextPatternNumber, pattern.patternNumber + 1);
    }

    PatternState duplicatedPattern = *sourcePattern;
    duplicatedPattern.patternNumber = nextPatternNumber;
    duplicatedPattern.name =
        sourcePattern->name.empty()
            ? ("Pattern " + std::to_string(nextPatternNumber))
            : (sourcePattern->name + " Copy");
    projectState_.patterns.push_back(std::move(duplicatedPattern));
    synchronizeProjectMusicalState();
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Clone pattern", beforeState);
    publishSnapshot();
    return true;
}

bool AudioEngine::deletePattern(int patternNumber)
{
    auto patternIt = std::find_if(
        projectState_.patterns.begin(),
        projectState_.patterns.end(),
        [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
    if (patternIt == projectState_.patterns.end())
    {
        setError(kErrorProjectIo, "Pattern not found while deleting pattern.");
        return false;
    }

    if (projectState_.patterns.size() <= 1)
    {
        setError(kErrorProjectIo, "Cannot delete the last remaining pattern.");
        return false;
    }

    const ProjectState beforeState = projectState_;
    std::vector<std::uint32_t> removedPlaylistItemIds;

    for (const auto& item : projectState_.playlistItems)
    {
        if (item.type == PlaylistItemType::PatternClip && item.patternNumber == patternNumber)
        {
            removedPlaylistItemIds.push_back(item.itemId);
        }
    }

    for (auto& track : projectState_.tracks)
    {
        track.clipIds.erase(
            std::remove_if(
                track.clipIds.begin(),
                track.clipIds.end(),
                [&](std::uint32_t itemId)
                {
                    return std::find(removedPlaylistItemIds.begin(), removedPlaylistItemIds.end(), itemId) != removedPlaylistItemIds.end();
                }),
            track.clipIds.end());
    }

    projectState_.playlistItems.erase(
        std::remove_if(
            projectState_.playlistItems.begin(),
            projectState_.playlistItems.end(),
            [&](const PlaylistItemState& item)
            {
                return item.type == PlaylistItemType::PatternClip && item.patternNumber == patternNumber;
            }),
        projectState_.playlistItems.end());

    projectState_.clips.erase(
        std::remove_if(
            projectState_.clips.begin(),
            projectState_.clips.end(),
            [&](const ClipState& clip)
            {
                return clip.patternNumber == patternNumber;
            }),
        projectState_.clips.end());

    projectState_.patterns.erase(patternIt);
    synchronizeProjectMusicalState();
    projectState_.dirty = true;
    ++projectState_.revision;
    pushUndoState("Delete pattern", beforeState);
    requestGraphRebuild();
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

bool AudioEngine::addClipToTrack(
    std::uint32_t trackId,
    const std::string& clipName,
    double startTimeSeconds,
    double durationSeconds)
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
    clip.startTimeSeconds = std::max(0.0, startTimeSeconds);
    clip.durationSeconds = 4.0;
    clip.gain = 0.8;
    clip.loopEnabled = !audioClipRequest;
    clip.timeStretchEnabled = true;

    if (!audioClipRequest)
    {
        const PatternState* pattern = findPatternState(patternNumber);
        if (pattern != nullptr)
        {
            const double beats = static_cast<double>(std::max(1, pattern->lengthInBars)) * 4.0;
            clip.durationSeconds = std::max(0.5, (beats * 60.0) / std::max(1.0, transportInfo_.tempoBpm));
        }
    }

    if (audioClipRequest)
    {
        DecodedWavData wavData{};
        if (decodeWavFile(clip.filePath, wavData) && wavData.sampleRate != 0)
        {
            clip.durationSeconds = static_cast<double>(wavData.left.size()) / static_cast<double>(wavData.sampleRate);
        }
    }

    if (durationSeconds > 0.0)
    {
        clip.durationSeconds = std::max(0.125, durationSeconds);
    }

    PlaylistItemState item{};
    item.itemId = nextPlaylistItemId();
    item.trackId = trackId;
    item.type = audioClipRequest ? PlaylistItemType::AudioClip : PlaylistItemType::PatternClip;
    item.sourceClipId = clip.clipId;
    item.patternNumber = patternNumber;
    item.label = audioClipRequest ? clip.name : ("Pattern " + std::to_string(std::max(1, patternNumber)));
    item.startTimeSeconds = clip.startTimeSeconds;
    item.durationSeconds = std::max(0.125, clip.durationSeconds);
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

/*
 * AudioEngineDev1.cpp sigue siendo la puerta de entrada historica del motor.
 * Encadena las demas partes para que el build siga funcionando aunque el index
 * solo incluya este archivo.
 */
#include "AudioEngineDev2.cpp"
#include "AudioEngineDev3.cpp"
