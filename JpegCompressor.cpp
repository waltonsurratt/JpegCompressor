// Application: JPEG Compressor
// Developer: Walton Surratt
// Copyright (c) 2026 Surratt Solutions. All rights reserved.
// 
// JpegCompressor.cpp : Defines the entry point for the application.

#include <windows.h>   // MUST be first
#include <commdlg.h>   // defines OPENFILENAMEW
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

// If these are already defined in your headers, these guards avoid redefinition errors.
//#ifndef WM_COMPRESS_PROGRESS
//#define WM_COMPRESS_PROGRESS (WM_APP + 2)
//#endif
//#ifndef WM_COMPRESS_DONE
//#define WM_COMPRESS_DONE     (WM_APP + 3)
//#endif
//
//// New message for per-file batch status updates
//#ifndef WM_BATCH_FILE_START
//#define WM_BATCH_FILE_START  (WM_APP + 50)
//#endif

// ------------------------------------------------------------
// Global Variables
// ------------------------------------------------------------
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HWND hStatusBar = nullptr;                      // Status bar handle
HWND hProgressBar = nullptr;                    // Progress bar handle
HWND hBtnStart = nullptr;                       // Start button handle

// Input selection display (fixes missing hEditInputFile usage)
HWND hEditInputFile = nullptr;                  // Shows selected file or "<Multiple Files Selected>"

// Selected JPEG files to compress (batch)
std::vector<std::wstring> g_InputFiles;

// Output folder details
std::wstring g_OutputFolder;
HWND hEditOutputFolder = nullptr;

// Slider bar details
HWND hQualitySlider = nullptr;
int g_QualityValue = 80;   // Default quality (80%)
HWND hQualityValueLabel = nullptr;

// Compression state
std::atomic<bool> g_CompressInProgress(false);
HWND g_hMainWnd = nullptr;

// Drag-and-drop visual state
bool g_IsDragHovering = false;

// Per-file status base for batch mode
std::wstring g_CurrentFileStatusBase;

// ------------------------------------------------------------
// JPEG error handling
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// HELPER FUNCTIONS
// ------------------------------------------------------------
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

// Generates an output path for the compressed image based on the input file name and output folder.
std::wstring MakeMiniOutputPath(
    const std::wstring& inputPath,
    const std::wstring& outputFolder)
{
    // Extract filename without path
    size_t slashPos = inputPath.find_last_of(L"\\/");
    std::wstring filename =
        (slashPos == std::wstring::npos)
        ? inputPath
        : inputPath.substr(slashPos + 1);

    // Remove extension
    size_t dotPos = filename.find_last_of(L'.');
    if (dotPos != std::wstring::npos)
        filename = filename.substr(0, dotPos);

    // Append suffix + extension
    return outputFolder + L"\\" + filename + L"_mini.jpg";
}

// Updates the enabled/disabled state of the Start button based on whether both input and output paths are set.
void UpdateStartButtonState()
{
    bool canStart =
        !g_InputFiles.empty() &&
        !g_OutputFolder.empty() &&
        !g_CompressInProgress;

    if (hBtnStart)
        EnableWindow(hBtnStart, canStart);
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

    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom - 22);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

// Validates if the given path is a valid directory that can be used for output.
bool IsValidOutputDirectory(const std::wstring& path)
{
    if (path.empty())
        return false;

    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return false;

    return (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Retrieves the version string of the current executable in the format "Version X.Y.Z".
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

    outVersion =
        L"Version: " +
        std::to_wstring(major) + L"." +
        std::to_wstring(minor) + L"." +
        std::to_wstring(build);

    return true;
}

// ------------------------------------------------------------
// Compress single JPEG file using libjpeg-turbo
// NOTE: This worker no longer flips g_CompressInProgress or posts WM_COMPRESS_DONE.
// That is managed by the batch controller.
// ------------------------------------------------------------
void CompressJpegWorker(
    std::wstring inputPath,
    std::wstring outputFolder,
    int quality)
{
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile)
        return;

    std::vector<unsigned char> inputBuffer(
        (std::istreambuf_iterator<char>(inFile)),
        std::istreambuf_iterator<char>()
    );

    inFile.close();

    if (inputBuffer.empty())
        return;

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

    // Setup decompressor
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

    // Setup compressor (memory destination)
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

    // Scanline loop with REAL progress
    while (dinfo.output_scanline < dinfo.output_height)
    {
        jpeg_read_scanlines(&dinfo, row_pointer, 1);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);

        int progress =
            static_cast<int>(
                (dinfo.output_scanline * 100) / totalRows
                );

        PostMessage(g_hMainWnd, WM_COMPRESS_PROGRESS, progress, 0);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_finish_decompress(&dinfo);

    // Write output JPEG
    {
        // FIX: use inputPath/outputFolder provided to this worker
        std::wstring outPath = MakeMiniOutputPath(inputPath, outputFolder);
        std::ofstream outFile(outPath, std::ios::binary);
        if (outFile && outBuffer && outSize > 0)
        {
            outFile.write(reinterpret_cast<char*>(outBuffer), outSize);
        }
    }

cleanup:
    if (decompressorCreated)
        jpeg_destroy_decompress(&dinfo);

    if (compressorCreated)
        jpeg_destroy_compress(&cinfo);

    if (outBuffer)
        free(outBuffer);
}

// ------------------------------------------------------------
// Batch controller for multiple JPEG files
// ------------------------------------------------------------
void CompressBatchWorker(
    std::vector<std::wstring> files,
    std::wstring outputFolder,
    int quality)
{
    const size_t total = files.size();

    for (size_t i = 0; i < total; ++i)
    {
        if (!g_CompressInProgress)
            break;

        // Post per-file status update
        std::wstring fileName = files[i];
        size_t slash = fileName.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            fileName = fileName.substr(slash + 1);

        std::wstring statusBase =
            L"Processing (" + std::to_wstring(i + 1) + L"/" + std::to_wstring(total) + L"): " + fileName;

        PostMessage(g_hMainWnd, WM_BATCH_FILE_START, 0, (LPARAM)new std::wstring(statusBase));

        // Reset progress for the new file
        PostMessage(g_hMainWnd, WM_COMPRESS_PROGRESS, 0, 0);

        CompressJpegWorker(files[i], outputFolder, quality);
    }

    if (IsWindow(g_hMainWnd))
        PostMessage(g_hMainWnd, WM_COMPRESS_DONE, 0, 0);
}

// Forward declarations
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

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_JPEGCOMPRESSOR, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_JPEGCOMPRESSOR));

    MSG msg;
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

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex{};

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_JPEGCOMPRESSOR));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_JPEGCOMPRESSOR);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_JPEGCOMPRESSOR));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    HWND hWnd = CreateWindowW(
        szWindowClass,
        L"JPEG Compressor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        0,
        500,
        320,
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        g_hMainWnd = hWnd;

        InitCommonControls();
        DragAcceptFiles(g_hMainWnd, TRUE);

        // Create status bar
        hStatusBar = CreateWindowEx(
            0,
            STATUSCLASSNAME,
            L"Ready",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0,
            hWnd,
            nullptr,
            hInst,
            nullptr);

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
            nullptr);

        SendMessage(hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(hProgressBar, PBM_SETPOS, 0, 0);
        SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Ready");

        // "Choose File:" label
        CreateWindowW(
            L"STATIC",
            L"Choose File:",
            WS_CHILD | WS_VISIBLE,
            20, 20, 90, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr);

        // Browse button
        CreateWindowW(
            L"BUTTON",
            L"Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            120, 16, 110, 26,
            hWnd,
            (HMENU)IDC_BTN_CHOOSE_FILE,
            hInst,
            nullptr);

        // Read-only input display (fix)
        hEditInputFile = CreateWindowW(
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
            240, 16, 220, 26,
            hWnd,
            nullptr,
            hInst,
            nullptr);

        // "Output Folder:" label
        CreateWindowW(
            L"STATIC",
            L"Output Folder:",
            WS_CHILD | WS_VISIBLE,
            20, 60, 100, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr);

        // Editable output folder edit control (requested)
        hEditOutputFolder = CreateWindowW(
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            130, 58, 230, 24,
            hWnd,
            (HMENU)IDC_EDIT_OUTPUT_FOLDER,
            hInst,
            nullptr);

        // Browse output folder button
        CreateWindowW(
            L"BUTTON",
            L"Browse",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            370, 56, 90, 26,
            hWnd,
            (HMENU)IDC_BTN_OUTPUT_FOLDER,
            hInst,
            nullptr);

        // "Quality:" label
        CreateWindowW(
            L"STATIC",
            L"Quality:",
            WS_CHILD | WS_VISIBLE,
            20, 100, 60, 20,
            hWnd,
            nullptr,
            hInst,
            nullptr);

        // Quality slider
        hQualitySlider = CreateWindowW(
            TRACKBAR_CLASS,
            nullptr,
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            90, 95, 250, 30,
            hWnd,
            (HMENU)IDC_SLIDER_QUALITY,
            hInst,
            nullptr);

        SendMessage(hQualitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(hQualitySlider, TBM_SETPOS, TRUE, g_QualityValue);
        SendMessage(hQualitySlider, TBM_SETTICFREQ, 10, 0);

        // Numeric quality label
        hQualityValueLabel = CreateWindowW(
            L"STATIC",
            L"80%",
            WS_CHILD | WS_VISIBLE,
            350, 100, 50, 20,
            hWnd,
            (HMENU)IDC_STATIC_QUALITY_VALUE,
            hInst,
            nullptr);

        // Start button
        hBtnStart = CreateWindowW(
            L"BUTTON",
            L"Start",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_DISABLED,
            180, 140, 120, 40,
            hWnd,
            (HMENU)IDC_BTN_START,
            hInst,
            nullptr);

        // Force initial layout so progress bar is positioned correctly immediately
        SendMessage(hWnd, WM_SIZE, 0, 0);

        UpdateStartButtonState();
    }
    break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);

        // Handle manual edits of output folder (requested)
        if (HIWORD(wParam) == EN_CHANGE && wmId == IDC_EDIT_OUTPUT_FOLDER)
        {
            wchar_t buffer[MAX_PATH]{};
            GetWindowTextW(hEditOutputFolder, buffer, MAX_PATH);
            g_OutputFolder = buffer;
            UpdateStartButtonState();
            return 0;
        }

        switch (wmId)
        {
        case IDC_BTN_CHOOSE_FILE:
        {
            wchar_t fileBuffer[32768] = { 0 };

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"JPEG Images (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0";
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = _countof(fileBuffer);
            ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
            ofn.lpstrTitle = L"Select JPEG Images";

            if (GetOpenFileNameW(&ofn))
            {
                g_InputFiles.clear();

                wchar_t* ptr = fileBuffer;
                std::wstring directory = ptr;
                ptr += directory.length() + 1;

                if (*ptr == L'\0')
                {
                    // Single file selected
                    g_InputFiles.push_back(directory);
                    if (hEditInputFile)
                        SetWindowTextW(hEditInputFile, directory.c_str());
                }
                else
                {
                    // Multiple files selected
                    while (*ptr)
                    {
                        g_InputFiles.push_back(directory + L"\\" + ptr);
                        ptr += wcslen(ptr) + 1;
                    }

                    if (hEditInputFile)
                        SetWindowTextW(hEditInputFile, L"<Multiple Files Selected>");
                }

                UpdateStartButtonState();
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
                wchar_t folderPath[MAX_PATH]{};
                if (SHGetPathFromIDListW(pidl, folderPath))
                {
                    g_OutputFolder = folderPath;
                    SetWindowTextW(hEditOutputFolder, g_OutputFolder.c_str());
                    std::wstring status = L"Output Folder: " + g_OutputFolder;
                    SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)status.c_str());
                    UpdateStartButtonState();
                }
                CoTaskMemFree(pidl);
            }
        }
        break;

        case IDC_BTN_START:
        {
            if (g_CompressInProgress)
                break;

            // Error handling for invalid output directory (requested)
            if (!IsValidOutputDirectory(g_OutputFolder))
            {
                SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Invalid Output Directory");
                MessageBoxW(
                    hWnd,
                    L"The selected output folder is invalid or does not exist.",
                    L"Invalid Output Directory",
                    MB_ICONERROR | MB_OK);
                break;
            }

            if (g_InputFiles.empty())
                break;

            g_CompressInProgress = true;
            EnableWindow(hBtnStart, FALSE);
            SendMessage(hProgressBar, PBM_SETPOS, 0, 0);

            g_CurrentFileStatusBase.clear();
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Starting..."
            );

            std::thread worker(CompressBatchWorker, g_InputFiles, g_OutputFolder, g_QualityValue);
            worker.detach();
        }
        break;

        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
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
            g_QualityValue = (int)SendMessage(hQualitySlider, TBM_GETPOS, 0, 0);
            g_QualityValue = SnapQuality(g_QualityValue);
            SendMessage(hQualitySlider, TBM_SETPOS, TRUE, g_QualityValue);

            std::wstring labelText = std::to_wstring(g_QualityValue) + L"%";
            SetWindowTextW(hQualityValueLabel, labelText.c_str());

            std::wstring statusText = L"Quality: " + labelText;
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
                g_InputFiles.clear();
                g_InputFiles.push_back(droppedPath);

                if (hEditInputFile)
                    SetWindowTextW(hEditInputFile, droppedPath.c_str());

                UpdateStartButtonState();

                SendMessageW(hStatusBar, SB_SETTEXT, 0, (LPARAM)droppedPath.c_str());
            }
        }

        g_IsDragHovering = false;
        InvalidateRect(g_hMainWnd, nullptr, TRUE);

        DragFinish(hDrop);
    }
    break;

    case WM_MOUSEMOVE:
    {
        if ((wParam & MK_LBUTTON) == 0)
            break;

        if (!g_IsDragHovering)
        {
            g_IsDragHovering = true;
            InvalidateRect(g_hMainWnd, nullptr, TRUE);
        }
    }
    break;

    case WM_CAPTURECHANGED:
    {
        if (g_IsDragHovering)
        {
            g_IsDragHovering = false;
            InvalidateRect(g_hMainWnd, nullptr, TRUE);
        }
    }
    break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(g_hMainWnd, &ps);

        if (g_IsDragHovering)
        {
            DrawDragOutline(g_hMainWnd, hdc);
        }

        EndPaint(g_hMainWnd, &ps);
    }
    break;

    case WM_SIZE:
    {
        if (hStatusBar)
        {
            SendMessage(hStatusBar, WM_SIZE, 0, 0);

            RECT rcStatus{};
            GetClientRect(hStatusBar, &rcStatus);

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

    case WM_BATCH_FILE_START:
    {
        std::wstring* p = reinterpret_cast<std::wstring*>(lParam);
        if (p)
        {
            g_CurrentFileStatusBase = *p;
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)g_CurrentFileStatusBase.c_str());
            delete p;
        }
    }
    break;

    case WM_COMPRESS_PROGRESS:
    {
        int percent = (int)wParam;

        SendMessage(hProgressBar, PBM_SETPOS, percent, 0);

        // Per-file progress updates (requested)
        if (!g_CurrentFileStatusBase.empty())
        {
            wchar_t text[512];
            swprintf(text, 512, L"%s - %d%%", g_CurrentFileStatusBase.c_str(), percent);
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)text);
        }
        else
        {
            wchar_t text[64];
            swprintf(text, 64, L"Compressing...%d%%", percent);
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)text);
        }
    }
    break;

    case WM_COMPRESS_DONE:
    {
        g_CompressInProgress = false;

        SendMessage(hProgressBar, PBM_SETPOS, 100, 0);
        SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Compression complete");

        g_CurrentFileStatusBase.clear();

        UpdateStartButtonState();
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

// About dialog.
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