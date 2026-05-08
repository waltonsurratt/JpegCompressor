// Application: JPEG Compressor
// Developer: Walton Surratt
// Copyright (c) 2026 Surratt Solutions. All rights reserved.
// 
// JpegCompressor.cpp : Defines the entry point for the application.
#include <windows.h>   // MUST be first
#include <commdlg.h>   // ✅ defines OPENFILENAMEW
#include <shlobj.h>
#include <string>
#include <commctrl.h>
#include <turbojpeg.h>
#include <thread>
#include <atomic>
#include <vector>
#include <fstream>
#include <jpeglib.h>
#include <setjmp.h>
#include "framework.h"
#include "JpegCompressor.h"

#pragma comment(lib, "comctl32.lib")

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HWND hStatusBar = nullptr;                      // Status bar handle
HWND hProgressBar = nullptr;                    // Progress bar handle
HWND hBtnStart = nullptr;                       // Start button handle

// Selected JPEG file to compress
std::wstring g_SelectedFile;

// Output folder details
std::wstring g_OutputFolder;
HWND hEditOutputFolder = nullptr;

// Slider bar details
HWND hQualitySlider = nullptr;
int g_QualityValue = 80;   // Default quality (80%)
HWND hQualityValueLabel = nullptr;

// Compression state
bool g_IsCompressing = false;
std::atomic<bool> g_CompressInProgress(false);
HWND g_hMainWnd = nullptr;

// Drag-and-drop visual state
bool g_IsDragHovering = false;

// ----------------------------------------

struct JpegErrorMgr
{
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void JpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorMgr* err = (JpegErrorMgr*)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

// ----------------------------------------
// HELPER FUNCTIONS
// ----------------------------------------
int SnapQuality(int value)
{
    const int snapValues[] = { 60, 75, 85, 95 };
    const int count = sizeof(snapValues) / sizeof(snapValues[0]);

    int closest = snapValues[0];
    int minDiff = abs(value - snapValues[0]);

    for (int i = 1; i < count; ++i)
    {
        int diff = abs(value - snapValues[i]);
        if (diff < minDiff)
        {
            minDiff = diff;
            closest = snapValues[i];
        }
    }
    return closest;
}

// Updates the enabled/disabled state of the Start button based on whether both input and output paths are set.
void UpdateStartButtonState()
{
    const bool hasInput = !g_SelectedFile.empty();
    const bool hasOutput = !g_OutputFolder.empty();

    EnableWindow(hBtnStart, hasInput && hasOutput);
}

// Validates if the given file path has a JPEG extension (.jpg, .jpeg, .jpe).
bool IsJpegFile(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;

    std::wstring ext = path.substr(dot);
    for (auto& c : ext)
        c = towlower(c);

    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe";
}

// Draws a dotted rectangle outline to indicate a drag-and-drop area.
void DrawDragOutline(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    InflateRect(&rc, -8, -8);

    HPEN pen = CreatePen(PS_DOT, 1, RGB(80, 80, 80));
    HBRUSH brush = (HBRUSH)GetStockObject(HOLLOW_BRUSH);

    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);

    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom - 22); // Subtract 22 px from the bottom for status bar height

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

// Retrieves the version string of the current executable in the format "Version X.Y.Z.W".
bool GetExecutableVersionString(std::wstring& outVersion)
{
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return false;

    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath, &handle);
    if (size == 0)
        return false;

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(exePath, 0, size, buffer.data()))
        return false;

    VS_FIXEDFILEINFO* verInfo = nullptr;
    UINT len = 0;

    if (!VerQueryValueW(
        buffer.data(),
        L"\\",
        reinterpret_cast<LPVOID*>(&verInfo),
        &len) || len == 0)
        return false;

    WORD major = HIWORD(verInfo->dwFileVersionMS);
    WORD minor = LOWORD(verInfo->dwFileVersionMS);
    WORD build = HIWORD(verInfo->dwFileVersionLS);
    WORD revision = LOWORD(verInfo->dwFileVersionLS);

    outVersion =
        L"Version: " +
        std::to_wstring(major) + L"." +
        std::to_wstring(minor) + L"." +
        std::to_wstring(build); // + L"." +
        //std::to_wstring(revision);

    return true;
}

// Compresses a JPEG file using libjpeg-turbo and saves it to the specified output folder with the given quality.
void CompressJpegWorker(
    std::wstring inputPath,
    std::wstring outputFolder,
    int quality)
{
    // ---------------------------------------
    // Load input JPEG fully into memory (EXE)
    // ---------------------------------------
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile)
        return;

    // IMPORTANT: note the parentheses to avoid the "most vexing parse"
    std::vector<unsigned char> inputBuffer(
        (std::istreambuf_iterator<char>(inFile)),
        std::istreambuf_iterator<char>()
    );

    inFile.close();

    if (inputBuffer.empty())
        return;

    // ---------------------------------------
    // Declare ALL objects before setjmp
    // ---------------------------------------
    jpeg_decompress_struct dinfo{};
    jpeg_compress_struct   cinfo{};

    JpegErrorMgr derr{};
    JpegErrorMgr cerr{};

    std::vector<JSAMPLE> scanlineBuffer;
    JSAMPROW row_pointer[1]{ nullptr };

    unsigned char* outBuffer = nullptr;
    unsigned long  outSize = 0;

    int totalRows = 0;
    int rowStride = 0;

    bool decompressorCreated = false;
    bool compressorCreated = false;

    // ---------------------------------------
    // Setup decompressor
    // ---------------------------------------
    dinfo.err = jpeg_std_error(&derr.pub);
    derr.pub.error_exit = JpegErrorExit;

    if (setjmp(derr.setjmp_buffer))
        goto cleanup;

    jpeg_create_decompress(&dinfo);
    decompressorCreated = true;

    jpeg_mem_src(
        &dinfo,
        inputBuffer.data(),
        static_cast<unsigned long>(inputBuffer.size())
    );

    jpeg_read_header(&dinfo, TRUE);
    jpeg_start_decompress(&dinfo);

    totalRows = static_cast<int>(dinfo.output_height);
    rowStride = dinfo.output_width * dinfo.output_components;

    scanlineBuffer.resize(rowStride);
    row_pointer[0] = scanlineBuffer.data();

    // ---------------------------------------
    // Setup compressor (memory destination)
    // ---------------------------------------
    cinfo.err = jpeg_std_error(&cerr.pub);
    cerr.pub.error_exit = JpegErrorExit;

    if (setjmp(cerr.setjmp_buffer))
        goto cleanup;

    jpeg_create_compress(&cinfo);
    compressorCreated = true;

    jpeg_mem_dest(&cinfo, &outBuffer, &outSize);

    cinfo.image_width = dinfo.output_width;
    cinfo.image_height = dinfo.output_height;
    cinfo.input_components = dinfo.output_components;
    cinfo.in_color_space = dinfo.out_color_space;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // ---------------------------------------
    // Scanline loop with REAL progress
    // ---------------------------------------
    while (dinfo.output_scanline < dinfo.output_height)
    {
        jpeg_read_scanlines(&dinfo, row_pointer, 1);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);

        int progress =
            static_cast<int>(
                (dinfo.output_scanline * 100) / totalRows
                );

        PostMessage(
            g_hMainWnd,
            WM_COMPRESS_PROGRESS,
            progress,
            0
        );
    }

    jpeg_finish_compress(&cinfo);
    jpeg_finish_decompress(&dinfo);

    // ---------------------------------------
    // Write output JPEG (EXE-owned file I/O)
    // ---------------------------------------
    {
        std::wstring outPath = outputFolder + L"\\compressed.jpg";
        std::ofstream outFile(outPath, std::ios::binary);
        if (outFile && outBuffer && outSize > 0)
        {
            outFile.write(
                reinterpret_cast<char*>(outBuffer),
                outSize
            );
        }
    }

cleanup:
    if (decompressorCreated)
        jpeg_destroy_decompress(&dinfo);

    if (compressorCreated)
        jpeg_destroy_compress(&cinfo);

    if (outBuffer)
        free(outBuffer);

    g_CompressInProgress = false;

    if (IsWindow(g_hMainWnd))
        PostMessage(g_hMainWnd, WM_COMPRESS_DONE, 0, 0);
}

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_JPEGCOMPRESSOR, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_JPEGCOMPRESSOR));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_JPEGCOMPRESSOR));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    //wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_JPEGCOMPRESSOR);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_JPEGCOMPRESSOR));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance; // Store instance handle in our global variable

    HWND hWnd = CreateWindowW(
        szWindowClass,
        //szTitle,
        L"JPEG Compressor",
        //WS_OVERLAPPEDWINDOW,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        0,
        500,   // ✅ Width
        320,   // ✅ Height
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        g_hMainWnd = hWnd;      // Store main window handle for later use

        // Initiate common controls (for status bar and progress bar)
        InitCommonControls();

        // Allow files to be dragged onto the window
        DragAcceptFiles(g_hMainWnd, TRUE);

        // Create status bar
        hStatusBar = CreateWindowEx(
            0,
            STATUSCLASSNAME,
            L"Ready",
            //WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0,
            hWnd,
            nullptr,
            hInst,
            nullptr
        );

        // Create progress bar as a child of the status bar
        hProgressBar = CreateWindowEx(
            0,
            PROGRESS_CLASS,
            nullptr,
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 16,
            hStatusBar,
            nullptr,
            hInst,
            nullptr
        );

        // Configure progress bar
        SendMessage(hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(hProgressBar, PBM_SETPOS, 0, 0);   // First  progress bar value

        SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Ready"); // First status bar message

        // "Choose File:" label
        CreateWindowW(
            L"STATIC",
            L"Choose File:",
            WS_CHILD | WS_VISIBLE,
            20, 20, 90, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr
        );

        // Browse button
        CreateWindowW(
            L"BUTTON",
            L"Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 16, 110, 26,
            hWnd,
            (HMENU)IDC_BTN_CHOOSE_FILE,
            hInst,
            nullptr
        );

        // "Output Folder:" label
        CreateWindowW(
            L"STATIC",
            L"Output Folder:",
            WS_CHILD | WS_VISIBLE,
            20, 60, 100, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr
        );

        // Read-only edit control to show selected folder
        hEditOutputFolder = CreateWindowW(
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
            130, 58, 230, 24,
            hWnd,
            (HMENU)IDC_EDIT_OUTPUT_FOLDER,
            hInst,
            nullptr
        );

        // Browse output folder button
        CreateWindowW(
            L"BUTTON",
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            370, 56, 90, 26,
            hWnd,
            (HMENU)IDC_BTN_OUTPUT_FOLDER,
            hInst,
            nullptr
        );

        // "Quality:" label
        CreateWindowW(
            L"STATIC",
            L"Quality:",
            WS_CHILD | WS_VISIBLE,
            20, 100, 60, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr
        );

        // Quality slider (0% - 100%)
        hQualitySlider = CreateWindowW(
            TRACKBAR_CLASS,
            nullptr,
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            90, 95, 250, 30,
            hWnd,
            (HMENU)IDC_SLIDER_QUALITY,
            hInst,
            nullptr
        );

        // Configure slider range and default position
        SendMessage(hQualitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(hQualitySlider, TBM_SETPOS, TRUE, g_QualityValue);
        SendMessage(hQualitySlider, TBM_SETTICFREQ, 10, 0);


        // Numeric quality value label (e.g. "85%")
        hQualityValueLabel = CreateWindowW(
            L"STATIC",
            L"80%",                     // ✅ STRING
            WS_CHILD | WS_VISIBLE,
            350, 100, 50, 20,
            hWnd,
            (HMENU)IDC_STATIC_QUALITY_VALUE,
            hInst,
            nullptr
        );

        // Start button
        hBtnStart = CreateWindowW(
            L"BUTTON",
            L"Start",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
            180, 140, 120, 40,   // position
            hWnd,
            (HMENU)IDC_BTN_START,
            hInst,
            nullptr
        );

    }
    break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        // Parse the menu selections:
        switch (wmId)
        {
        case IDC_BTN_CHOOSE_FILE:
        {
            OPENFILENAMEW ofn{};
            wchar_t filePath[MAX_PATH] = {};

            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter =
                L"JPEG Images (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
                L"All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrDefExt = L"jpg";

            if (GetOpenFileNameW(&ofn))
            {
                std::wstring selectedFile = filePath;

                // ✅ Validate extension
                size_t dot = selectedFile.find_last_of(L'.');
                bool valid = false;

                if (dot != std::wstring::npos)
                {
                    std::wstring ext = selectedFile.substr(dot);
                    for (wchar_t& c : ext) c = towlower(c);

                    valid = (ext == L".jpg" || ext == L".jpeg");
                }

                if (!valid)
                {
                    MessageBoxW(
                        hWnd,
                        L"Invalid image format.\n\n"
                        L"This tool only supports JPEG images "
                        L"(.jpg, .jpeg).",
                        L"Unsupported File",
                        MB_ICONERROR | MB_OK
                    );

                    // ❌ Clear invalid selection
                    g_SelectedFile.clear();
                    UpdateStartButtonState();
                    break;
                }

                // ✅ Valid file selected
                g_SelectedFile = filePath;      // AFTER the user successfully selects a file
                std::wstring statusText = L"Input: " + g_SelectedFile;
                SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)statusText.c_str());
                UpdateStartButtonState();       // ✅ MUST be called AFTER assignment
            }
        }
        break;



        case IDC_BTN_OUTPUT_FOLDER:
        {
            BROWSEINFOW bi{};
            bi.hwndOwner = hWnd;
            bi.lpszTitle = L"Select Output Folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl)
            {
                wchar_t folderPath[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, folderPath))
                {
                    g_OutputFolder = folderPath;        // AFTER folder is successfully selected
                    SetWindowTextW(hEditOutputFolder, g_OutputFolder.c_str());
                    std::wstring status = L"Output Folder: " + g_OutputFolder;
                    SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)status.c_str());
                    UpdateStartButtonState();           // ✅ MUST be here
                }
                CoTaskMemFree(pidl);
            }
        }
        break;


        case IDC_BTN_START:
        {
            if (g_CompressInProgress)
                break;

            g_CompressInProgress = true;
            EnableWindow(hBtnStart, FALSE);
            SendMessage(hProgressBar, PBM_SETPOS, 0, 0);

            SendMessage(
                hStatusBar,
                SB_SETTEXT,
                0,
                (LPARAM)L"Compressing JPEG..."
            );


            std::thread worker(
                CompressJpegWorker,
                g_SelectedFile,
                g_OutputFolder,
                g_QualityValue
            );

            worker.detach();

        }
        break;


        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;

        case IDM_FILE_RESTART:
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileName(nullptr, exePath, MAX_PATH);

            STARTUPINFO si{};
            si.cb = sizeof(si);

            PROCESS_INFORMATION pi{};

            // Launch a new instance of the same executable
            if (CreateProcess(
                exePath,     // Application name
                nullptr,     // Command line
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi))
            {
                // Close handles for the new process
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }

            // Cleanly exit current instance
            PostQuitMessage(0);
        }
        break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_HSCROLL:
    {
        if ((HWND)lParam == hQualitySlider)
        {
            g_QualityValue =
                (int)SendMessage(hQualitySlider, TBM_GETPOS, 0, 0);

            // Update numeric label
            std::wstring labelText =
                std::to_wstring(g_QualityValue) + L"%";

            SetWindowTextW(hQualityValueLabel, labelText.c_str());

            // Update status bar
            std::wstring statusText =
                L"Quality: " + labelText;

            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)statusText.c_str());
        }
    }
    break;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;

        wchar_t filePath[MAX_PATH]{};
        if (DragQueryFile(hDrop, 0, filePath, MAX_PATH))
        {
            std::wstring droppedPath = filePath;

            if (IsJpegFile(droppedPath))
            {
                g_SelectedFile = droppedPath;
                UpdateStartButtonState();

                // Send text to the first status bar pane
                SendMessageW(
                    hStatusBar,
                    SB_SETTEXT,
                    0, // pane index
                    reinterpret_cast<LPARAM>(droppedPath.c_str()));

            }
        }

        g_IsDragHovering = false;
        InvalidateRect(g_hMainWnd, nullptr, TRUE);

        DragFinish(hDrop);
        break;
    }

    case WM_MOUSEMOVE:
    {
        if ((wParam & MK_LBUTTON) == 0)
            break;

        if (!g_IsDragHovering)
        {
            g_IsDragHovering = true;
            InvalidateRect(g_hMainWnd, nullptr, TRUE);
        }
        break;
    }

    case WM_CAPTURECHANGED:
    {
        if (g_IsDragHovering)
        {
            g_IsDragHovering = false;
            InvalidateRect(g_hMainWnd, nullptr, TRUE);
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(g_hMainWnd, &ps);

        if (g_IsDragHovering)
        {
            DrawDragOutline(g_hMainWnd, hdc);
        }

        EndPaint(g_hMainWnd, &ps);
        break;
    }

    case WM_SIZE:
    {
        if (hStatusBar)
        {
            // Resize the status bar to fit the new window size
            SendMessage(hStatusBar, WM_SIZE, 0, 0);

            RECT rcStatus{};
            GetClientRect(hStatusBar, &rcStatus);

            // Leave space for text on the left
            int progressWidth = 160;
            int progressHeight = rcStatus.bottom - 4;

            MoveWindow(
                hProgressBar,
                rcStatus.right - progressWidth - 4,
                2,
                progressWidth,
                progressHeight,
                TRUE
            );
        }
    }
    break;

    case WM_COMPRESS_PROGRESS:
    {
        int percent = (int)wParam;

        SendMessage(hProgressBar, PBM_SETPOS, percent, 0);

        wchar_t text[64];
        swprintf(text, 64, L"Compressing...%d%%", percent);

        SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)text);
    }
    break;

    case WM_COMPRESS_DONE:
    {
        SendMessage(hProgressBar, PBM_SETPOS, 100, 0);

        SendMessage(
            hStatusBar,
            SB_SETTEXT,
            0,
            (LPARAM)L"Compression complete"
        );

        EnableWindow(hBtnStart, TRUE);
    }
    break;

    case WM_APP + 1:
    {
        g_CompressInProgress = false;

        SendMessage(hProgressBar, PBM_SETPOS, 100, 0);
        SendMessage(
            hStatusBar,
            SB_SETTEXT,
            0,
            (LPARAM)L"Compression completed"
        );

        EnableWindow(hBtnStart, TRUE);
    }
    break;


    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    
case WM_INITDIALOG:
{
    std::wstring versionText;
    if (GetExecutableVersionString(versionText))
    {
        // IDC_STATIC_VERSION must exist in your About dialog
        SetDlgItemTextW(hDlg, IDC_STATIC_VERSION, versionText.c_str());
    }
    return (INT_PTR)TRUE;
}


    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
