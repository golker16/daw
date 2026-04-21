#include "UI.h"

#include <stdexcept>
#include <string>

#define ID_BTN_START 1001
#define ID_BTN_STOP  1002
#define ID_LBL_STATUS 1003

UI::UI(HINSTANCE hInstance, int nCmdShow, AudioEngine& engine)
    : hInstance_(hInstance),
      nCmdShow_(nCmdShow),
      engine_(engine),
      hwnd_(nullptr),
      startButton_(nullptr),
      stopButton_(nullptr),
      statusLabel_(nullptr)
{
    registerWindowClass();
    createMainWindow();
    createControls();
    updateStatusLabel();
}

int UI::run()
{
    ShowWindow(hwnd_, nCmdShow_);
    UpdateWindow(hwnd_);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}

void UI::registerWindowClass()
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = UI::WindowProc;
    wc.hInstance = hInstance_;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExA(&wc))
    {
        throw std::runtime_error("No se pudo registrar la clase de ventana.");
    }
}

void UI::createMainWindow()
{
    hwnd_ = CreateWindowExA(
        0,
        kWindowClassName,
        "DAW Cloud Template",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        520, 260,
        nullptr,
        nullptr,
        hInstance_,
        this
    );

    if (!hwnd_)
    {
        throw std::runtime_error("No se pudo crear la ventana principal.");
    }
}

void UI::createControls()
{
    startButton_ = CreateWindowA(
        "BUTTON",
        "Start Engine",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        30, 40, 150, 40,
        hwnd_,
        reinterpret_cast<HMENU>(ID_BTN_START),
        hInstance_,
        nullptr
    );

    stopButton_ = CreateWindowA(
        "BUTTON",
        "Stop Engine",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        200, 40, 150, 40,
        hwnd_,
        reinterpret_cast<HMENU>(ID_BTN_STOP),
        hInstance_,
        nullptr
    );

    statusLabel_ = CreateWindowA(
        "STATIC",
        "Audio Engine: unknown",
        WS_VISIBLE | WS_CHILD,
        30, 110, 420, 30,
        hwnd_,
        reinterpret_cast<HMENU>(ID_LBL_STATUS),
        hInstance_,
        nullptr
    );
}

void UI::updateStatusLabel()
{
    const std::string text = engine_.getStatusText();
    SetWindowTextA(statusLabel_, text.c_str());
}

LRESULT CALLBACK UI::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UI* ui = nullptr;

    if (uMsg == WM_NCCREATE)
    {
        CREATESTRUCTA* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        ui = reinterpret_cast<UI*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    else
    {
        ui = reinterpret_cast<UI*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    switch (uMsg)
    {
    case WM_COMMAND:
        if (ui)
        {
            const int controlId = LOWORD(wParam);

            switch (controlId)
            {
            case ID_BTN_START:
                ui->engine_.start();
                ui->updateStatusLabel();
                return 0;

            case ID_BTN_STOP:
                ui->engine_.stop();
                ui->updateStatusLabel();
                return 0;

            default:
                break;
            }
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}
