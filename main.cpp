#include <windows.h>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include "AudioEngine.h"
#include "UI.h"

namespace
{
    constexpr const char* kAppName = "DAW Cloud Template";

    enum class AppExitCode : int
    {
        Success = 0,
        ConfigError = 10,
        EngineInitError = 20,
        AudioBackendError = 30,
        UiInitError = 40,
        FatalUnknownError = 100
    };

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

    AudioEngine::EngineConfig buildDefaultEngineConfig()
    {
        AudioEngine::EngineConfig config{};

        config.preferredSampleRate = 48000;
        config.preferredBlockSize = 256;
        config.inputChannelCount = 2;
        config.outputChannelCount = 2;

        config.enableWasapi = true;
        config.enableCompiledGraph = true;
        config.enableAnticipativeProcessing = true;
        config.enableSampleAccurateAutomation = true;
        config.enablePdc = true;
        config.enableOfflineRender = true;
        config.enablePluginHost = false;
        config.enablePluginSandbox = false;
        config.prefer64BitInternalMix = true;

        config.preferredDeviceName = "";
        config.projectName = "Untitled Project";

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
            << ", graph=" << (config.enableCompiledGraph ? "on" : "off")
            << ", anticipative=" << (config.enableAnticipativeProcessing ? "on" : "off")
            << ", automation=" << (config.enableSampleAccurateAutomation ? "on" : "off")
            << ", pdc=" << (config.enablePdc ? "on" : "off")
            << ", offlineRender=" << (config.enableOfflineRender ? "on" : "off")
            << ", pluginHost=" << (config.enablePluginHost ? "on" : "off")
            << ", pluginSandbox=" << (config.enablePluginSandbox ? "on" : "off")
            << ", internalMix64=" << (config.prefer64BitInternalMix ? "on" : "off")
            << ", preferredDevice=" << safeValueOrFallback(config.preferredDeviceName, "<default>");
        return oss.str();
    }

    AppExitCode runApplication(HINSTANCE hInstance, int nCmdShow)
    {
        appendLogLine("Application bootstrap started.");

        AudioEngine::EngineConfig engineConfig = buildDefaultEngineConfig();
        appendLogLine(describeConfig(engineConfig));

        AudioEngine engine;

        appendLogLine("Phase 1/7: bootstrap complete.");
        appendLogLine("Phase 2/7: initializing core engine.");

        if (!engine.initialize(engineConfig))
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Engine initialization failed."
            );

            appendLogLine("Engine initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el core engine.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Core engine initialized successfully.");

        appendLogLine("Phase 3/7: initializing audio backend.");
        if (!engine.initializeAudioBackend())
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Audio backend initialization failed."
            );

            appendLogLine("Audio backend initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el backend de audio.\n\n" + errorText);
            return AppExitCode::AudioBackendError;
        }

        appendLogLine(
            "Audio backend initialized. Backend=" +
            safeValueOrFallback(engine.getBackendName(), "<unknown>") +
            ", device=" + safeValueOrFallback(engine.getCurrentDeviceName(), "<default>") +
            ", sampleRate=" + std::to_string(engine.getCurrentSampleRate()) +
            ", blockSize=" + std::to_string(engine.getCurrentBlockSize())
        );

        appendLogLine("Phase 4/7: building and compiling initial graph.");
        if (!engine.buildInitialGraph())
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Initial graph build failed."
            );

            appendLogLine("Initial graph build failed: " + errorText);
            showErrorBox(kAppName, "No se pudo construir el grafo inicial.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        if (!engine.compileGraph())
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Graph compilation failed."
            );

            appendLogLine("Graph compilation failed: " + errorText);
            showErrorBox(kAppName, "No se pudo compilar el grafo inicial.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine(
            "Initial graph compiled successfully. GraphVersion=" +
            std::to_string(engine.getMetrics().activeGraphVersion)
        );

        appendLogLine("Phase 5/7: initializing transport.");
        if (!engine.initializeTransport())
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Transport initialization failed."
            );

            appendLogLine("Transport initialization failed: " + errorText);
            showErrorBox(kAppName, "No se pudo inicializar el transport.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Transport initialized successfully.");

        appendLogLine("Phase 6/7: initializing UI.");
        UI ui(hInstance, nCmdShow, engine);

        appendLogLine("UI initialized successfully.");

        appendLogLine("Phase 7/7: starting telemetry/polling.");
        if (!engine.startTelemetry())
        {
            const std::string errorText = safeValueOrFallback(
                engine.getLastErrorMessage(),
                "Telemetry startup failed."
            );

            appendLogLine("Telemetry startup failed: " + errorText);
            showErrorBox(kAppName, "No se pudo iniciar la telemetría.\n\n" + errorText);
            return AppExitCode::EngineInitError;
        }

        appendLogLine("Telemetry started successfully.");
        appendLogLine("Application startup completed. Entering UI loop.");

        const int uiExitCode = ui.run();

        appendLogLine("UI loop exited with code " + std::to_string(uiExitCode) + ".");

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
        return static_cast<int>(runApplication(hInstance, nCmdShow));
    }
    catch (const AudioEngine::ConfigurationException& e)
    {
        const std::string message = std::string("Error de configuración:\n\n") + e.what();
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
        const std::string message = std::string("Error de inicialización de UI:\n\n") + e.what();
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
