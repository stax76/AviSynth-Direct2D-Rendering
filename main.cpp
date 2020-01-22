
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <d2d1_1.h>

#include "avisynth_headers\avisynth.h"

HWND  g_MainWindow  = NULL;
HWND  g_VideoWindow = NULL;
HWND  g_Trackbar    = NULL;

int   g_Position   = 0;
int   g_BorderSize = 0;
                   
float g_DpiScale   = 0;

const AVS_Linkage*  AVS_linkage = NULL;
IScriptEnvironment* g_AvsScriptEnvironment = NULL;
PClip               g_AvsClip;
AVSValue            g_AvsValue;
VideoInfo           g_AvsVideoInfo;

ID2D1Factory*          g_D2D_Factory   = NULL;
ID2D1HwndRenderTarget* g_RenderTarget  = NULL;
ID2D1DeviceContext*    g_DeviceContext = NULL;

HRESULT CreateGraphicsResources();
void    DiscardGraphicsResources();
void    OnPaint();

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
template <class T> void SafeRelease(T** ppT);


int APIENTRY wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_     LPWSTR    lpCmdLine,
    _In_     int       nCmdShow)
{    
    // D2D

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_D2D_Factory)))
        throw;

    // AviSynth

    HMODULE avsDLL = LoadLibrary(L"AviSynth");

    if (!avsDLL)
        throw;

    IScriptEnvironment* (*CreateScriptEnvironment)(int version) =
        (IScriptEnvironment * (*)(int)) GetProcAddress(avsDLL, "CreateScriptEnvironment");

    if (!CreateScriptEnvironment)
        throw;

    int avsInterfaceVersion = 6;
    g_AvsScriptEnvironment = CreateScriptEnvironment(avsInterfaceVersion);

    if (!g_AvsScriptEnvironment)
        throw;

    AVS_linkage = g_AvsScriptEnvironment->GetAVSLinkage();

    AVSValue src("D:\\Samples\\Mad Max - Fury Road_temp\\Mad Max - Fury Road_new.avs");

    g_AvsValue = g_AvsScriptEnvironment->Invoke("Import", AVSValue(&src, 1));
    g_AvsValue = g_AvsScriptEnvironment->Invoke("ConvertToRGB32", g_AvsValue);
    g_AvsValue = g_AvsScriptEnvironment->Invoke("FlipVertical", g_AvsValue);

    if (!g_AvsValue.IsClip())
        throw;

    g_AvsClip      = g_AvsValue.AsClip();
    g_AvsVideoInfo = g_AvsClip->GetVideoInfo();

    // main window class

    WNDCLASSEXW wcex = {};

    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = L"AvsMainWindowClass";
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassEx(&wcex))
        throw;

    // video window class

    wcex = {};

    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = L"AvsVideoWindowClass";

    if (!RegisterClassEx(&wcex))
        throw;

    // windows

    HDC screen = GetDC(NULL);
    g_DpiScale = GetDeviceCaps(screen, LOGPIXELSX) / (float)96;
    ReleaseDC(NULL, screen);

    g_BorderSize = (int)(10 * g_DpiScale);

    g_MainWindow = CreateWindowEx(
        NULL, L"AvsMainWindowClass", L"AviSynth Direct2D Rendering", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)(650 * g_DpiScale), (int)(500 * g_DpiScale),
        NULL, NULL, hInstance, NULL);

    RECT clientRect;
    GetClientRect(g_MainWindow, &clientRect);

    g_VideoWindow = CreateWindowEx(
        NULL, L"AvsVideoWindowClass", NULL, WS_CHILD | WS_VISIBLE,
        g_BorderSize, g_BorderSize,
        clientRect.right - g_BorderSize * 2,
        clientRect.bottom - g_BorderSize * 6,
        g_MainWindow, NULL, hInstance, NULL);

    g_Trackbar = CreateWindowEx(
        NULL, TRACKBAR_CLASS, NULL, WS_CHILD | WS_VISIBLE,
        g_BorderSize, clientRect.bottom - g_BorderSize * 4,
        clientRect.right - g_BorderSize * 2, g_BorderSize * 3,
        g_MainWindow, NULL, hInstance, NULL);

    int max = g_AvsVideoInfo.num_frames - 1;
    SendMessage(g_Trackbar, TBM_SETRANGE, TRUE, MAKELONG(0, max));
    SetFocus(g_Trackbar);
    ShowWindow(g_MainWindow, nCmdShow);
    UpdateWindow(g_MainWindow);

    // message loop

    MSG msg = {};

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // release recourses
    DiscardGraphicsResources();
    SafeRelease(&g_D2D_Factory);

    g_AvsClip = NULL;
    g_AvsValue = NULL;
    AVS_linkage = NULL;
    g_AvsScriptEnvironment->DeleteScriptEnvironment();

    return (int) msg.wParam;
}


LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        {
            RECT clientRect;
            GetClientRect(g_MainWindow, &clientRect);

            MoveWindow(g_VideoWindow,
                g_BorderSize, g_BorderSize,
                clientRect.right - g_BorderSize * 2,
                clientRect.bottom - g_BorderSize * 6,
                FALSE
            );

            MoveWindow(g_Trackbar,
                g_BorderSize, clientRect.bottom - g_BorderSize * 4,
                clientRect.right - g_BorderSize * 2, g_BorderSize * 3,
                FALSE);

            if (g_RenderTarget != NULL)
            {
                RECT rect;
                GetClientRect(g_VideoWindow, &rect);
                D2D1_SIZE_U size = D2D1::SizeU(rect.right, rect.bottom);
                g_RenderTarget->Resize(size);
                InvalidateRect(g_VideoWindow, NULL, FALSE);
            }

            return 0;
        }

    case WM_HSCROLL:
        if (!HIWORD(wParam))
            g_Position = (int)SendMessage(g_Trackbar, TBM_GETPOS, 0, 0);
        else if (LOWORD(wParam) == TB_THUMBPOSITION || LOWORD(wParam) == TB_THUMBTRACK)
            g_Position = HIWORD(wParam);

        InvalidateRect(g_VideoWindow, NULL, FALSE);
        return 0;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}


template <class T> void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}


HRESULT CreateGraphicsResources()
{
    HRESULT hr = S_OK;

    if (g_RenderTarget == NULL)
    {
        RECT rect;
        GetClientRect(g_VideoWindow, &rect);
        D2D1_SIZE_U size = D2D1::SizeU(rect.right, rect.bottom);

        hr = g_D2D_Factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(g_VideoWindow, size),
            &g_RenderTarget);

        if (SUCCEEDED(hr))
        {
            hr = g_RenderTarget->QueryInterface(
                __uuidof(ID2D1DeviceContext),
                reinterpret_cast<void**>(&g_DeviceContext)
            );
        }
    }

    return hr;
}


void DiscardGraphicsResources()
{    
    SafeRelease(&g_DeviceContext);
    SafeRelease(&g_RenderTarget);
}


void OnPaint()
{
    HRESULT hr = CreateGraphicsResources();

    if (SUCCEEDED(hr))
    {
        g_DeviceContext->BeginDraw();

        PVideoFrame frame = g_AvsClip->GetFrame(g_Position, g_AvsScriptEnvironment);
        auto readPtr = frame->GetReadPtr();

        if (readPtr)
        {
            D2D1_BITMAP_PROPERTIES properties;
            properties.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
            properties.dpiX = properties.dpiY = 96;
            ID2D1Bitmap* pBitmap = NULL;
            D2D1_SIZE_U size = D2D1::SizeU(g_AvsVideoInfo.width, g_AvsVideoInfo.height);
            hr = g_DeviceContext->CreateBitmap(size, readPtr, frame->GetPitch(), properties, &pBitmap);

            if (SUCCEEDED(hr))
            {
                D2D1_SIZE_F destinationSize = g_DeviceContext->GetSize();
                D2D1_RECT_F destinationRect = D2D1::RectF(0, 0, destinationSize.width, destinationSize.height);                            
                g_DeviceContext->DrawBitmap(pBitmap, &destinationRect, 1.0,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, NULL, NULL);
                pBitmap->Release();
            }
        }

        hr = g_DeviceContext->EndDraw();

        if (FAILED(hr) || hr == D2DERR_RECREATE_TARGET)
            DiscardGraphicsResources();
    }
}
