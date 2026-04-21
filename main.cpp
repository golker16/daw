#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AudioEngine.h"
#include "UI.h"

namespace
{
    constexpr const char* kAppName = "DAW Cloud Template";

    struct SandboxSharedState
    {
        volatile LONG heartbeat = 0;
        volatile LONG stopRequested = 0;
        volatile LONG lastError = 0;
        volatile LONG reserved = 0;
    };

    enum class AppExitCode : int
    {
        Success = 0,
        ConfigError = 10,
        EngineInitError = 20,
        AudioBackendError = 30,
        UiInitError = 40,
        PluginHostError = 50,
        FatalUnknownError = 100
    };

    struct AppBootstrapOptions
    {
        bool pluginHostMode = false;
        bool disableWasapi = false;
        bool safeMode = false;
        std::string mappingName;
        std::string pluginName;
        std::string sessionPath;
        std::string preferredDeviceName;
        int sampleRate = 0;
        int blockSize = 0;
    };

    std::string narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string output(requiredBytes > 0 ? static_cast<std::size_t>(requiredBytes) : 0, '\0');
        if (requiredBytes > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, output.data(), requiredBytes, nullptr, nullptr);
            if (!output.empty() && output.back() == '\0')
            {
                output.pop_back();
            }
        }
        return output;
    }

    void appendLogLine(const std::string& text)
    {
        std::ofstream logFile("DAWCloudTemplate.log", std::ios::app);
        if (!logFile.is_open())
        {
            return;
        }

        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);

        logFile
            << '['
            << localTime.wYear << '-'
            << (localTime.wMonth < 10 ? "0" : "") << localTime.wMonth << '-'
            << (localTime.wDay < 10 ? "0" : "") << localTime.wDay << ' '
            << (localTime.wHour < 10 ? "0" : "") << localTime.wHour << ':'
            << (localTime.wMinute < 10 ? "0" : "") << localTime.wMinute << ':'
            << (localTime.wSecond < 10 ? "0" : "") << localTime.wSecond
            << "] "
            << text
            << '\n';
    }

    void showErrorBox(const std::string& title, const std::string& body)
    {
        MessageBoxA(nullptr, body.c_str(), title.c_str(), MB_ICONERROR | MB_OK);
    }

    std::string safeValueOrFallback(const std::string& value, const std::string& fallback)
    {
        return value.empty() ? fallback : value;
    }

    std::vector<std::string> getCommandLineArgs()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<std::string> args;

        if (argv == nullptr)
        {
            return args;
        }

        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index)
        {
            args.push_back(narrow(argv[index]));
        }

        LocalFree(argv);
        return args;
    }

    AppBootstrapOptions parseBootstrapOptions()
    {
        AppBootstrapOptions options{};
        const std::vector<std::string> args = getCommandLineArgs();

        for (const auto& arg : args)
        {
            if (arg == "--plugin-host")
            {
                options.pluginHostMode = true;
            }
            else if (arg == "--dummy")
            {
                options.disableWasapi = true;
            }
            else if (arg == "--safe-mode")
            {
                options.safeMode = true;
            }
            else if (arg.rfind("--mapping=", 0) == 0)
            {
                options.mappingName = arg.substr(std::string("--mapping=").size());
            }
            else if (arg.rfind("--plugin=", 0) == 0)
            {
                options.pluginName = arg.substr(std::string("--plugin=").size());
            }
            else if (arg.rfind("--session=", 0) == 0)
            {
                options.sessionPath = arg.substr(std::string("--session=").size());
            }
            else if (arg.rfind("--device=", 0) == 0)
            {
                options.preferredDeviceName = arg.substr(std::string("--device=").size());
            }
            else if (arg.rfind("--sr=", 0) == 0)
            {
                options.sampleRate = std::atoi(arg.substr(std::string("--sr=").size()).c_str());
            }
            else if (arg.rfind("--bs=", 0) == 0)
            {
                options.blockSize = std::atoi(arg.substr(std::string("--bs=").size()).c_str());
            }
        }

        return options;
    }

    AudioEngine::EngineConfig buildDefaultEngineConfig(const AppBootstrapOptions& options)
    {
        AudioEngine::EngineConfig config{};
        config.preferredSampleRate = options.sampleRate > 0 ? options.sampleRate : 48000;
        config.preferredBlockSize = options.blockSize > 0 ? options.blockSize : 256;
        config.inputChannelCount = 2;
        config.outputChannelCount = 2;
        config.anticipativePrefetchBlocks = 2;
        config.helperThreadCount = 2;
        config.diskWorkerCount = 1;
        config.enableWasapi = !options.disableWasapi;
        config.enableCompiledGraph = true;
        config.enableAnticipativeProcessing = true;
        config.enableSampleAccurateAutomation = true;
        config.enablePdc = true;
        config.enableOfflineRender = true;
        config.enablePluginHost = true;
        config.enablePluginSandbox = true;
        config.prefer64BitInternalMix = true;
        config.safeMode = options.safeMode;
        config.preferredDeviceName = options.preferredDeviceName;
        config.projectName = "Untitled Project";
        config.sessionPath = options.sessionPath.empty() ? "session.dawproject" : options.sessionPath;
        return config;
    }

    std::string describeConfig(const AudioEngine::EngineConfig& config)
    {
        std::ostringstream oss;
        oss
            << "Engine config: "
            << "sampleRate=" << config.preferredSampleRate
            << ", blockSize=" << config.preferredBlockSize
            << ", inputs=" << config.inputChannelCount
            << ", outputs=" << config.outputChannelCount
            << ", wasapi=" << (config.enableWasapi ? "on" : "off")
            << ", anticipative=" << (config.enableAnticipativeProcessing ? "on" : "off")
            << ", automation=" << (config.enableSampleAccurateAutomation ? "on" : "off")
            << ", pdc=" << (config.enablePdc ? "on" : "off")
            << ", offlineRender=" << (config.enableOfflineRender ? "on" : "off")
            << ", pluginHost=" << (config.enablePluginHost ? "on" : "off")
            << ", pluginSandbox=" << (config.enablePluginSandbox ? "on" : "off")
            << ", internalMix64=" << (config.prefer64BitInternalMix ? "on" : "off")
            << ", safeMode=" << (config.safeMode ? "on" : "off")
            << ", preferredDevice=" << safeValueOrFallback(config.preferredDeviceName, "<default>")
            << ", sessionPath=" << config.sessionPath;
        return oss.str();
    }

    AppExitCode runPluginHost(const AppBootstrapOptions& options)
    {
        if (options.mappingName.empty())
        {
            appendLogLine("Plugin host mode failed: no mapping name was provided.");
            return AppExitCode::PluginHostError;
        }

        HANDLE mappingHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, options.mappingName.c_str());
        if (mappingHandle == nullptr)
        {
            appendLogLine("Plugin host mode failed: OpenFileMappingA returned null.");
            return AppExitCode::PluginHostError;
        }

        void* view = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxSharedState));
        if (view == nullptr)
        {
            CloseHandle(mappingHandle);
            appendLogLine("Plugin host mode failed: MapViewOfFile returned null.");
            return AppExitCode::PluginHostError;
        }

        auto* shared = reinterpret_cast<SandboxSharedState*>(view);
        appendLogLine("Plugin host mode started for plugin '" + options.pluginName + "'.");

        while (shared->stopRequested == 0)
        {
            ++shared->heartbeat;
            Sleep(100);
        }

        appendLogLine("Plugin host mode stopping cleanly.");
        UnmapViewOfFile(view);
        CloseHandle(mappingHandle);
        return AppExitCode::Success;
    }

    AppExitCode runApplication(HINSTANCE hInstance, int nCmdShow, const AppBootstrapOptions& options)
    {
        appendLogLine("Application bootstrap started.");

        AudioEngine::EngineConfig engineConfig = buildDefaultEngineConfig(options);
        appendLogLine(describeConfig(engineConfig));

        AudioEngine engine;

        appendLogLine("Phase 1/8: initializing core engine.");
        if (!engine.initialize(engineConfig))
        {
            const std::string errorText = safeValueOrFallback(engine.getLastErrorMessage(), "Engine initialization failed.");
            appendLogLine("Engine initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el core engine.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Phase 2/8: restoring session state.");
        if (!options.sessionPath.empty() && std::filesystem::exists(options.sessionPath))
        {
            if (!engine.loadProject(options.sessionPath))
            {
                appendLogLine("Session load failed, continuing with default project: " + engine.getLastErrorMessage());
            }
        }

        appendLogLine("Phase 3/8: initializing audio backend.");
        if (!engine.initializeAudioBackend())
        {
            const std::string errorText = safeValueOrFallback(engine.getLastErrorMessage(), "Audio backend initialization failed.");
            appendLogLine("Audio backend initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el backend de audio.\n\n" + errorText);
            return AppExitCode::AudioBackendError;
        }

        appendLogLine(
            "Audio backend ready. Backend=" +
            safeValueOrFallback(engine.getBackendName(), "<unknown>") +
            ", device=" + safeValueOrFallback(engine.getCurrentDeviceName(), "<default>") +
            ", sampleRate=" + std::to_string(engine.getCurrentSampleRate()) +
            ", blockSize=" + std::to_string(engine.getCurrentBlockSize()));

        appendLogLine("Phase 4/8: building graph.");
        if (!engine.buildInitialGraph() || !engine.compileGraph())
        {
            const std::string errorText = safeValueOrFallback(engine.getLastErrorMessage(), "Graph initialization failed.");
            appendLogLine("Graph initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo construir/compilar el grafo.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Phase 5/8: initializing transport.");
        if (!engine.initializeTransport())
        {
            const std::string errorText = safeValueOrFallback(engine.getLastErrorMessage(), "Transport initialization failed.");
            appendLogLine("Transport initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el transport.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Phase 6/8: starting engine and maintenance threads.");
        if (!engine.start())
        {
            const std::string errorText = safeValueOrFallback(engine.getLastErrorMessage(), "Engine start failed.");
            appendLogLine("Engine start failed: " + errorText);
            showErrorBox(kAppName, "No se pudo arrancar el engine de audio.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        engine.startTelemetry();
        appendLogLine("Realtime engine is running.");

        appendLogLine("Phase 7/8: initializing UI.");
        UI ui(hInstance, nCmdShow, engine);
        appendLogLine("UI initialized successfully.");

        appendLogLine("Phase 8/8: entering UI loop.");
        const int uiExitCode = ui.run();
        appendLogLine("UI loop exited with code " + std::to_string(uiExitCode) + ".");

        appendLogLine("Shutting down audio engine.");
        engine.stopTelemetry();
        engine.shutdown();

        appendLogLine("Application shutdown complete.");
        return AppExitCode::Success;
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    try
    {
        const AppBootstrapOptions options = parseBootstrapOptions();

        if (options.pluginHostMode)
        {
            return static_cast<int>(runPluginHost(options));
        }

        return static_cast<int>(runApplication(hInstance, nCmdShow, options));
    }
    catch (const AudioEngine::ConfigurationException& e)
    {
        const std::string message = std::string("Error de configuracion:\n\n") + e.what();
        appendLogLine(message);
        showErrorBox(kAppName, message);
        return static_cast<int>(AppExitCode::ConfigError);
    }
    catch (const AudioEngine::AudioBackendException& e)
    {
        const std::string message = std::string("Error de backend de audio:\n\n") + e.what();
        appendLogLine(message);
        showErrorBox(kAppName, message);
        return static_cast<int>(AppExitCode::AudioBackendError);
    }
    catch (const UI::UiInitializationException& e)
    {
        const std::string message = std::string("Error de inicializacion de UI:\n\n") + e.what();
        appendLogLine(message);
        showErrorBox(kAppName, message);
        return static_cast<int>(AppExitCode::UiInitError);
    }
    catch (const std::exception& e)
    {
        const std::string message = std::string("Error fatal:\n\n") + e.what();
        appendLogLine(message);
        showErrorBox(kAppName, message);
        return static_cast<int>(AppExitCode::FatalUnknownError);
    }
    catch (...)
    {
        const std::string message = "Error fatal desconocido.";
        appendLogLine(message);
        showErrorBox(kAppName, message);
        return static_cast<int>(AppExitCode::FatalUnknownError);
    }
}

