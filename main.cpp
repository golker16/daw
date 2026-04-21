#include <windows.h>
#include <string>
#include <exception>

#include "AudioEngine.h"
#include "UI.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    try
    {
        AudioEngine engine;
        engine.initialize();

        UI ui(hInstance, nCmdShow, engine);
        return ui.run();
    }
    catch (const std::exception& e)
    {
        std::string msg = "Error fatal:\n";
        msg += e.what();
        MessageBoxA(nullptr, msg.c_str(), "DAW Cloud Template", MB_ICONERROR | MB_OK);
        return -1;
    }
    catch (...)
    {
        MessageBoxA(nullptr, "Error fatal desconocido.", "DAW Cloud Template", MB_ICONERROR | MB_OK);
        return -1;
    }
}
