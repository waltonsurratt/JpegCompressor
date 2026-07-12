// Application: JPEG Compressor
// Developer: Walton Surratt
// Copyright (c) 2026 Surratt Solutions. All rights reserved.
// 
// JpegCompressor.cpp : Defines the entry point for the application.
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <commctrl.h>
#include <turbojpeg.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <fstream>
#include <jpeglib.h>
#include <setjmp.h>
#include <cstdint>

#include "framework.h"
#include "JpegCompressor.h"

#pragma comment(lib, "comctl32.lib")

#define MAX_LOADSTRING 100

// Largest batch of JPEGs that can be selected/queued at once. This bounds
// the multi-select file dialog's buffer (below) to a known, finite worst
// case instead of letting a pathologically large selection grow it without
// limit.
constexpr size_t kMaxBatchFiles = 8192;

// ------------------------------------------------------------
// Globals (unchanged)
// ------------------------------------------------------------
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

HWND hStatusBar = nullptr;
HWND hProgressBar = nullptr;
HWND hBtnStart = nullptr;
HWND hEditInputFile = nullptr;

std::vector<std::wstring> g_InputFiles;
std::wstring g_OutputFolder;

HWND hEditOutputFolder = nullptr;
HWND hQualitySlider = nullptr;
HWND hQualityValueLabel = nullptr;

int g_QualityValue = 80;

std::atomic<bool> g_CompressInProgress(false);
std::atomic<bool> g_CancelRequested(false);
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
// ✅ EXIF ORIENTATION
// ------------------------------------------------------------
int GetExifOrientation(j_decompress_ptr dinfo)
{
    for (jpeg_saved_marker_ptr marker = dinfo->marker_list;
        marker != nullptr;
        marker = marker->next)
    {
        if (marker->marker == (JPEG_APP0 + 1) && marker->data_length > 6)
        {
            const unsigned char* data = marker->data;

            if (memcmp(data, "Exif\0\0", 6) != 0)
                continue;

            const unsigned char* tiff = data + 6;
            bool little = tiff[0] == 'I';

            auto read16 = [&](const unsigned char* p) -> uint16_t
                {
                    return little ? (p[0] | (p[1] << 8))
                        : (p[1] | (p[0] << 8));
                };

            auto read32 = [&](const unsigned char* p) -> uint32_t
                {
                    return little
                        ? (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
                        : (p[3] | (p[2] << 8) | (p[1] << 16) | (p[0] << 24));
                };

            uint32_t ifdOffset = read32(tiff + 4);
            const unsigned char* ifd = tiff + ifdOffset;

            uint16_t count = read16(ifd);

            for (int i = 0; i < count; i++)
            {
                const unsigned char* entry = ifd + 2 + (i * 12);
                if (read16(entry) == 0x0112)
                    return read16(entry + 8);
            }
        }
    }
    return 1;
}

// ------------------------------------------------------------
// ✅ APPLY ROTATION
// ------------------------------------------------------------
std::vector<unsigned char> ApplyOrientation(
    const std::vector<unsigned char>& src,
    int width,
    int height,
    int channels,
    int orientation,
    int& outW,
    int& outH)
{
    outW = width;
    outH = height;

    if (orientation == 1)
        return src;

    std::vector<unsigned char> dst;

    switch (orientation)
    {
    case 6:
        outW = height; outH = width;
        dst.resize(outW * outH * channels);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                memcpy(&dst[((x * outW) + (outW - y - 1)) * channels],
                    &src[(y * width + x) * channels],
                    channels);
        break;

    case 3:
        dst.resize(width * height * channels);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                memcpy(&dst[((height - y - 1) * width + (width - x - 1)) * channels],
                    &src[(y * width + x) * channels],
                    channels);
        break;

    case 8:
        outW = height; outH = width;
        dst.resize(outW * outH * channels);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                memcpy(&dst[((outH - x - 1) * outW + y) * channels],
                    &src[(y * width + x) * channels],
                    channels);
        break;

    default:
        return src;
    }

    return dst;
}

// ------------------------------------------------------------
// HELPER FUNCTIONS
// ------------------------------------------------------------
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

// Updates the Start/Cancel button's caption and enabled state based on current app state.
// This is the single place that decides what the button looks like, so every code
// path (start, cancel, completion) stays in sync.
void UpdateStartButtonState()
{
    if (!hBtnStart)
        return;

    if (g_CompressInProgress)
    {
        if (g_CancelRequested)
        {
            // Cancel has already been clicked; disable briefly so the user can't
            // queue up repeated cancel requests while the worker thread unwinds.
            SetWindowTextW(hBtnStart, L"Cancelling...");
            EnableWindow(hBtnStart, FALSE);
        }
        else
        {
            // A compression batch is running - the button now acts as Cancel.
            SetWindowTextW(hBtnStart, L"Cancel");
            EnableWindow(hBtnStart, TRUE);
        }
    }
    else
    {
        bool canStart =
            !g_InputFiles.empty() &&
            !g_OutputFolder.empty();

        SetWindowTextW(hBtnStart, L"Start");
        EnableWindow(hBtnStart, canStart);
    }
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
// ✅ COMPRESS WORKER (MODIFIED - memory optimized, parallel-safe progress)
// ------------------------------------------------------------
// Returns true if the file was handled to completion (including the case where
// it failed for an unrelated reason, e.g. a bad JPEG); returns false only when
// the user cancelled mid-file, so the batch controller can stop immediately.
//
// progressSlot: when running as part of a parallel batch, each worker thread
// owns one atomic<int> "slot" that this function updates instead of posting
// window messages directly. With several threads compressing at once, having
// every thread post its own UI message on every scanline would flood the
// message queue (the very problem the original per-scanline posting logic
// was already trying to avoid for a single file); aggregating slots into one
// throttled UI update is handled by the batch controller below. Pass nullptr
// if no progress reporting is needed.
bool CompressJpegWorker(
    const std::wstring& inputPath,
    const std::wstring& outputFolder,
    int quality,
    std::atomic<int>* progressSlot = nullptr)
{
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile) return true;

    std::vector<unsigned char> inputBuffer(
        (std::istreambuf_iterator<char>(inFile)),
        std::istreambuf_iterator<char>());
    inFile.close(); // bytes are in memory now; release the OS handle right away

    jpeg_decompress_struct dinfo{};
    jpeg_compress_struct cinfo{};
    JpegErrorMgr derr{}, cerr{};

    dinfo.err = jpeg_std_error(&derr.pub);
    derr.pub.error_exit = JpegErrorExit;

    if (setjmp(derr.setjmp_buffer))
    {
        // A libjpeg error during decompression longjmps straight back to
        // here. dinfo has already been created on every path that can
        // reach this branch (jpeg_create_decompress is the very next line,
        // with nothing risky in between), so it must be torn down here
        // rather than leaked.
        jpeg_destroy_decompress(&dinfo);
        return true;
    }

    jpeg_create_decompress(&dinfo);

    // ✅ Capture EXIF
    jpeg_save_markers(&dinfo, JPEG_APP0 + 1, 0xFFFF);

    jpeg_mem_src(
        &dinfo,
        inputBuffer.data(),
        static_cast<unsigned long>(inputBuffer.size())
    );
    jpeg_read_header(&dinfo, TRUE);
    jpeg_start_decompress(&dinfo);

    int width = dinfo.output_width;
    int height = dinfo.output_height;
    int channels = dinfo.output_components;

    std::vector<unsigned char> image(static_cast<size_t>(width) * height * channels);

    int lastPostedProgress = -1;

    while (dinfo.output_scanline < dinfo.output_height)
    {
        if (g_CancelRequested)
        {
            // Abort cleanly mid-decompress - nothing has been written to disk yet.
            jpeg_destroy_decompress(&dinfo);
            return false;
        }

        unsigned char* row = &image[static_cast<size_t>(dinfo.output_scanline) * width * channels];
        jpeg_read_scanlines(&dinfo, &row, 1);

        int progress = (int)((dinfo.output_scanline * 50) / height);

        // Only report when the percentage actually changes, to keep the
        // update rate sane for very tall images.
        if (progress != lastPostedProgress)
        {
            if (progressSlot)
                progressSlot->store(progress, std::memory_order_relaxed);
            lastPostedProgress = progress;
        }
    }

    int orientation = GetExifOrientation(&dinfo);

    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);

    // ✅ MEMORY OPTIMIZATION: the compressed source bytes have now been fully
    // consumed by libjpeg, so there's no need to keep holding them in memory
    // for the rest of this file's processing (re-orientation + encoding).
    // This matters most when several files are being compressed in parallel,
    // since it keeps each thread's footprint closer to "one decoded image"
    // rather than "one decoded image + its original compressed bytes".
    inputBuffer.clear();
    inputBuffer.shrink_to_fit();

    if (g_CancelRequested)
        return false;

    int newW = width, newH = height;
    bool needsRotation = (orientation == 3 || orientation == 6 || orientation == 8);

    // ✅ MEMORY OPTIMIZATION: ApplyOrientation's "no transform" path used to
    // return a full copy of the image by value even when nothing changed
    // (the common case - most photos either have no EXIF rotation, or are
    // already upright). That copy doubled peak memory for no benefit. Now
    // we only allocate a second full-size buffer for the 3 orientations
    // that actually require one, and otherwise point straight at the
    // already-decoded buffer.
    std::vector<unsigned char> rotatedImage;
    std::vector<unsigned char>* finalImage = &image;

    if (needsRotation)
    {
        rotatedImage = ApplyOrientation(image, width, height, channels,
            orientation, newW, newH);

        // The pre-rotation pixels are no longer needed once we have the
        // rotated copy - release them now instead of letting two full-size
        // image buffers sit in memory through the entire encode step below.
        image.clear();
        image.shrink_to_fit();

        finalImage = &rotatedImage;
    }

    if (g_CancelRequested)
        return false;

    // Compress
    cinfo.err = jpeg_std_error(&cerr.pub);
    cerr.pub.error_exit = JpegErrorExit;

    unsigned char* outBuffer = nullptr;
    unsigned long outSize = 0;

    if (setjmp(cerr.setjmp_buffer))
    {
        // A libjpeg error during encoding longjmps straight back to here.
        // By this point cinfo has been created and outBuffer may already
        // hold a partially-filled buffer from jpeg_mem_dest - both need to
        // be torn down here rather than leaked.
        jpeg_destroy_compress(&cinfo);
        if (outBuffer)
        {
            free(outBuffer);
            outBuffer = nullptr;
        }
        return true;
    }

    jpeg_create_compress(&cinfo);

    jpeg_mem_dest(&cinfo, &outBuffer, &outSize);

    cinfo.image_width = newW;
    cinfo.image_height = newH;
    cinfo.input_components = channels;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    int stride = newW * channels;

    while (cinfo.next_scanline < cinfo.image_height)
    {
        if (g_CancelRequested)
        {
            // Abort cleanly mid-compress - the output file is only ever opened
            // and written further down, once the full buffer is ready, so there
            // is no partial/corrupt file on disk to clean up.
            jpeg_destroy_compress(&cinfo);
            if (outBuffer)
            {
                free(outBuffer);
                outBuffer = nullptr;
            }
            return false;
        }

        unsigned char* row = &(*finalImage)[static_cast<size_t>(cinfo.next_scanline) * stride];
        jpeg_write_scanlines(&cinfo, &row, 1);

        // Second half of the bar tracks compression, so the bar reflects the
        // whole per-file pipeline instead of sitting at 100% while encoding
        // (which can take as long as decoding, especially at high quality)
        // still has work left to do.
        int progress = 50 + (int)((cinfo.next_scanline * 50) / cinfo.image_height);

        if (progress != lastPostedProgress)
        {
            if (progressSlot)
                progressSlot->store(progress, std::memory_order_relaxed);
            lastPostedProgress = progress;
        }
    }

    jpeg_finish_compress(&cinfo);

    std::wstring outPath = MakeMiniOutputPath(inputPath, outputFolder);
    std::ofstream outFile(outPath, std::ios::binary);

    if (outFile)
        outFile.write((char*)outBuffer, outSize);

    jpeg_destroy_compress(&cinfo);
    if (outBuffer)
    {
        free(outBuffer);
        outBuffer = nullptr;
    }

    return true;
}

// ------------------------------------------------------------
// ✅ PARALLEL BATCH PROCESSING (NEW)
// ------------------------------------------------------------
// Shared, thread-safe state for one batch run. Aggregates per-file progress
// from however many worker threads are active into a single overall
// percentage and status line for the UI.
struct BatchProgressState
{
    std::vector<std::atomic<int>> slotProgress; // 0-100 progress of whatever file each worker slot currently holds
    std::vector<std::wstring> slotActiveFile;   // filename currently held by each slot (empty = idle)
    std::mutex activeFileMutex;                 // guards slotActiveFile
    std::atomic<size_t> filesCompleted{ 0 };
    size_t filesTotal = 0;

    BatchProgressState(size_t numSlots, size_t totalFiles)
        : slotProgress(numSlots), slotActiveFile(numSlots), filesTotal(totalFiles)
    {
        // vector<atomic<int>>(count) default-constructs each slot, but
        // std::atomic's default constructor doesn't guarantee a zeroed
        // value before C++20 - set it explicitly so the ticker never sums
        // an indeterminate value for a slot no worker has claimed yet.
        for (auto& slot : slotProgress)
            slot.store(0, std::memory_order_relaxed);
    }
};

// Picks how many files to compress at once.
//
// Each in-flight file holds a full decoded RGB buffer in memory (and,
// briefly, a second one for files that need EXIF rotation), so blindly
// using every hardware thread could multiply peak memory by the core count
// on a machine with many cores - a single batch of large photos could
// exhaust RAM. This combines a hard ceiling with a check against actual
// available physical memory, so the degree of parallelism backs off
// automatically on machines that are low on RAM instead of just on core count.
size_t GetWorkerCount(size_t fileCount)
{
    constexpr size_t kHardCap = 8; // never spin up more compression threads than this
    constexpr unsigned long long kAssumedBytesPerFile = 150ULL * 1024 * 1024;
    // ^ Conservative worst-case per-file estimate: a 24MP photo decodes to
    // roughly 72MB as RGB; add headroom for a temporary rotated copy and
    // the compressed-output buffer.

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2; // hardware_concurrency() is allowed to return 0; fall back to a small default

    size_t workers = std::min<size_t>(hw, kHardCap);

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus))
    {
        size_t memoryAllowedWorkers =
            static_cast<size_t>(memStatus.ullAvailPhys / kAssumedBytesPerFile);
        workers = std::min(workers, std::max<size_t>(1, memoryAllowedWorkers));
    }

    workers = std::min(workers, fileCount); // never more workers than there is work
    return std::max<size_t>(1, workers);
}

// One worker thread's loop: repeatedly claims the next not-yet-started file
// from the shared file list (via an atomic index) and compresses it. Pulling
// work from a shared index, rather than pre-assigning a fixed slice of files
// to each thread, means a thread that finishes a small file quickly picks up
// the next one immediately instead of sitting idle while another thread is
// still working through a large one.
void ParallelCompressionWorker(
    size_t slotIndex,
    std::atomic<size_t>& nextFileIndex,
    const std::vector<std::wstring>& files,
    const std::wstring& outputFolder,
    int quality,
    BatchProgressState& progress)
{
    for (;;)
    {
        if (g_CancelRequested)
            break;

        size_t idx = nextFileIndex.fetch_add(1, std::memory_order_relaxed);
        if (idx >= files.size())
            break;

        {
            std::lock_guard<std::mutex> lock(progress.activeFileMutex);
            progress.slotActiveFile[slotIndex] = files[idx];
        }
        progress.slotProgress[slotIndex].store(0, std::memory_order_relaxed);

        bool completed = CompressJpegWorker(files[idx], outputFolder, quality,
            &progress.slotProgress[slotIndex]);

        if (!completed)
        {
            // User cancelled mid-file - stop pulling new work on this thread.
            break;
        }

        progress.slotProgress[slotIndex].store(100, std::memory_order_relaxed);
        progress.filesCompleted.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(progress.activeFileMutex);
            progress.slotActiveFile[slotIndex].clear();
        }
    }
}

// Periodically aggregates progress across all worker slots into a single
// overall percentage and a human-readable status line, then posts both to
// the UI thread. Centralizing this on a timer - instead of having every
// worker thread post on every scanline, as the single-threaded version did -
// keeps the number of UI messages constant no matter how many files are
// compressing in parallel or how large they are.
void ProgressTicker(std::atomic<bool>& running, BatchProgressState& progress)
{
    while (running.load(std::memory_order_relaxed))
    {
        int slotSum = 0;
        for (auto& slot : progress.slotProgress)
            slotSum += slot.load(std::memory_order_relaxed);

        size_t completed = progress.filesCompleted.load(std::memory_order_relaxed);
        int overallPercent = 0;
        if (progress.filesTotal > 0)
            overallPercent = (int)(((completed * 100) + slotSum) / progress.filesTotal);
        if (overallPercent > 100) overallPercent = 100;

        PostMessage(g_hMainWnd, WM_COMPRESS_PROGRESS, overallPercent, 0);

        std::wstring statusText;
        {
            std::lock_guard<std::mutex> lock(progress.activeFileMutex);
            size_t activeCount = 0;
            std::wstring firstName;

            for (auto& f : progress.slotActiveFile)
            {
                if (f.empty())
                    continue;

                activeCount++;
                if (firstName.empty())
                {
                    size_t slash = f.find_last_of(L"\\/");
                    firstName = (slash == std::wstring::npos) ? f : f.substr(slash + 1);
                }
            }

            if (activeCount > 0)
            {
                statusText = L"Compressing " + firstName;
                if (activeCount > 1)
                    statusText += L" (+" + std::to_wstring(activeCount - 1) + L" more)";
            }
        }

        if (!statusText.empty())
        {
            statusText += L" - " + std::to_wstring(completed) + L"/" +
                std::to_wstring(progress.filesTotal) + L" done";

            PostMessage(g_hMainWnd, WM_BATCH_FILE_START, 0, (LPARAM)new std::wstring(statusText));
        }

        if (completed >= progress.filesTotal || g_CancelRequested)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

// ------------------------------------------------------------
// Batch controller for multiple JPEG files
// ------------------------------------------------------------
void CompressBatchWorker(
    std::vector<std::wstring> files,
    std::wstring outputFolder,
    int quality)
{
    const size_t totalFiles = files.size();

    if (totalFiles == 0)
    {
        if (IsWindow(g_hMainWnd))
            PostMessage(g_hMainWnd, WM_COMPRESS_DONE, 0, 0);
        return;
    }

    // ✅ PARALLELISM: instead of compressing files one at a time on this single
    // worker thread, spin up a small pool of threads that pull from the same
    // shared file list. GetWorkerCount() bounds the pool size by core count,
    // a hard ceiling, and available RAM, so this speeds up multi-file batches
    // without letting memory usage scale unchecked with file count or cores.
    size_t numWorkers = GetWorkerCount(totalFiles);
    BatchProgressState progress(numWorkers, totalFiles);

    std::atomic<size_t> nextFileIndex(0);
    std::atomic<bool> tickerRunning(true);

    // The ticker owns all UI progress/status updates for this batch, at a
    // fixed rate, regardless of how many compression threads are running.
    std::thread ticker(ProgressTicker, std::ref(tickerRunning), std::ref(progress));

    {
        std::vector<std::thread> pool;
        pool.reserve(numWorkers);

        for (size_t i = 0; i < numWorkers; ++i)
        {
            pool.emplace_back(
                ParallelCompressionWorker,
                i,
                std::ref(nextFileIndex),
                std::cref(files),
                std::cref(outputFolder),
                quality,
                std::ref(progress));
        }

        for (auto& t : pool)
            t.join();
    }

    tickerRunning = false;
    ticker.join();

    bool cancelled = g_CancelRequested || (progress.filesCompleted.load() < totalFiles);

    // wParam carries the outcome: 1 = cancelled by the user, 0 = ran to completion.
    if (IsWindow(g_hMainWnd))
        PostMessage(g_hMainWnd, WM_COMPRESS_DONE, cancelled ? 1 : 0, 0);
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

        SendMessage(hQualitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
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
            // Heap-allocate this buffer instead of putting it on the stack.
            // GetOpenFileNameW with OFN_ALLOWMULTISELECT needs a buffer large
            // enough to hold many concatenated, double-null-terminated paths,
            // and a buffer sized for a large batch would blow the stack
            // outright if it weren't heap-allocated (and trips VS's
            // /analyze stack-usage warning, C6262, even at much smaller
            // sizes than this).
            //
            // Sized for the worst case of kMaxBatchFiles files, each up to
            // MAX_PATH characters, plus the shared directory prefix and a
            // final extra null terminator. This sizes for the worst case
            // up front rather than starting small and growing on demand:
            // when the buffer is too small, GetOpenFileNameW reports the
            // size it actually needs in only the first two bytes of the
            // buffer (a 16-bit WORD), which tops out at 65,535 - nowhere
            // near enough to reliably size a buffer for thousands of files,
            // so that auto-grow mechanism can't be trusted for a batch
            // this large.
            const size_t kFileBufferChars =
                static_cast<size_t>(MAX_PATH) + 1 +
                kMaxBatchFiles * (static_cast<size_t>(MAX_PATH) + 1) + 1;

            std::vector<wchar_t> fileBuffer(kFileBufferChars, L'\0');

            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"JPEG Images (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0";
            ofn.lpstrFile = fileBuffer.data();
            ofn.nMaxFile = (DWORD)fileBuffer.size();
            ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
            ofn.lpstrTitle = L"Select JPEG Images";

            if (GetOpenFileNameW(&ofn))
            {
                g_InputFiles.clear();

                wchar_t* ptr = fileBuffer.data();
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
                    // Multiple files selected. The buffer above is sized so
                    // a selection of up to kMaxBatchFiles files always
                    // fits, but guard the loop anyway rather than rely on
                    // that invariant blindly.
                    while (*ptr && g_InputFiles.size() < kMaxBatchFiles)
                    {
                        g_InputFiles.push_back(directory + L"\\" + ptr);
                        ptr += wcslen(ptr) + 1;
                    }

                    if (hEditInputFile)
                        SetWindowTextW(hEditInputFile, L"<Multiple Files Selected>");
                }

                UpdateStartButtonState();
            }
            else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
            {
                // The only way a buffer already sized for kMaxBatchFiles
                // comes up short is a selection bigger than the batch limit.
                std::wstring msg =
                    L"A batch can hold at most " + std::to_wstring(kMaxBatchFiles) +
                    L" files. Please select fewer files and try again.";
                MessageBoxW(hWnd, msg.c_str(), L"Too Many Files Selected", MB_ICONWARNING | MB_OK);
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
            {
                // The button is currently in "Cancel" mode. Ignore repeat
                // clicks once a cancel is already in flight.
                if (!g_CancelRequested)
                {
                    g_CancelRequested = true;

                    // Immediate feedback so the user sees the cancel register
                    // right away, even though the worker thread may take a
                    // moment to unwind out of the current file.
                    SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Cancelling...");
                    UpdateStartButtonState();
                }
                break;
            }

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
            g_CancelRequested = false;
            SendMessage(hProgressBar, PBM_SETPOS, 0, 0);

            g_CurrentFileStatusBase.clear();
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Starting..."
            );

            // Flip the button into Cancel mode right away, before the
            // worker thread is even spun up.
            UpdateStartButtonState();

            std::thread worker(CompressBatchWorker, g_InputFiles, g_OutputFolder, g_QualityValue);
            worker.detach();
        }
        break;

        case IDM_FILE_RESTART:
        {
            wchar_t exePath[MAX_PATH]{};

            // Get current executable path
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            {
                // Start a new instance of the app
                STARTUPINFOW si{};
                PROCESS_INFORMATION pi{};

                si.cb = sizeof(si);

                if (CreateProcessW(
                    exePath,          // Application name
                    nullptr,          // Command line
                    nullptr, nullptr, // Process/thread security
                    FALSE,
                    0,
                    nullptr,
                    nullptr,
                    &si,
                    &pi))
                {
                    // Close handles from CreateProcess
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);

                    // Close this instance
                    DestroyWindow(hWnd);
                }
                else
                {
                    MessageBoxW(hWnd, L"Failed to restart application.", L"Error", MB_ICONERROR);
                }
            }
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
            // Get slider position and update quality value
            g_QualityValue = (int)SendMessage(hQualitySlider, TBM_GETPOS, 0, 0);

            // Clamp to 1–100 just to be safe
            if (g_QualityValue < 1) g_QualityValue = 1;
            if (g_QualityValue > 100) g_QualityValue = 100;


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
        bool wasCancelled = (wParam != 0);

        g_CompressInProgress = false;
        g_CancelRequested = false;

        if (wasCancelled)
        {
            // Clean, immediate reset so there's no lingering progress from
            // the cancelled run.
            SendMessage(hProgressBar, PBM_SETPOS, 0, 0);
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Compression cancelled");
        }
        else
        {
            SendMessage(hProgressBar, PBM_SETPOS, 100, 0);
            SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)L"Compression complete");
        }

        g_CurrentFileStatusBase.clear();

        // Flip the button back to Start mode.
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