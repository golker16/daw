#pragma once

#include <windows.h>
#include <string>
#include "AudioEngine.h"

class UI
{
public:
    UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void createMainWindow();
    void registerWindowClass();
    void createControls();
    void updateStatusLabel();

    HINSTANCE hInstance_;
    int nCmdShow_;
    AudioEngine& engine_;

    HWND hwnd_;
    HWND startButton_;
    HWND stopButton_;
    HWND statusLabel_;

    static constexpr const char* kWindowClassName = "DAWCloudTemplateWindowClass";
};
