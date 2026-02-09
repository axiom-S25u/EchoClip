#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <atomic>
#include <thread>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace geode::prelude;

struct VideoFrame {
    std::vector<uint8_t> data;
    size_t width = 0;
    size_t height = 0;
    size_t pitch = 0;
    int64_t timestampMs = 0;
    int64_t frameNumber = 0;
};

class ScreenCapture {
private:
    std::deque<VideoFrame> frameQueue;
    mutable std::mutex queueMutex;
    
    int captureWidth = 854;
    int captureHeight = 480;
    int targetFPS = 30;
    int64_t frameCounter = 0;
    
    std::chrono::steady_clock::time_point captureStartTime;
    size_t maxQueue = 30;
    
    std::atomic<bool> isCapturing{false};
    std::atomic<bool> shouldStop{false};
    std::thread captureThread;
    
    int frameIntervalMs = 33;
    
    // dxgi resources
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIOutputDuplication* deskDupl = nullptr;
    IDXGIOutput1* output1 = nullptr;
    
    bool dxgiInitialized = false;

public:
    ScreenCapture(int fps = 30, int width = 854, int height = 480)
        : captureWidth(width), captureHeight(height), targetFPS(fps) {
        frameIntervalMs = 1000 / targetFPS;
        maxQueue = fps * 2;
        log::info("screencapture: initialized {}x{} dxgi capture", width, height);
    }
    
    ~ScreenCapture() {
        stop();
        cleanupDXGI();
    }
    
    bool start() {
        if (isCapturing) return true;
        
        if (!initializeDXGI()) {
            log::error("screencapture: failed to init dxgi");
            return false;
        }
        
        captureStartTime = std::chrono::steady_clock::now();
        frameCounter = 0;
        shouldStop = false;
        isCapturing = true;
        
        captureThread = std::thread([this] { captureLoop(); });
        
        return true;
    }
    
    void stop() {
        isCapturing = false;
        shouldStop = true;
        
        if (captureThread.joinable()) {
            captureThread.join();
        }
        
        log::info("screencapture: stopped");
    }
    
    bool getFrame(VideoFrame& outFrame) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (frameQueue.empty()) return false;
        
        outFrame = std::move(frameQueue.front());
        frameQueue.pop_front();
        return true;
    }
    
    void setGDWindow(HWND hwnd) {}

private:
    bool initializeDXGI() {
        HRESULT hr;
        
        // create d3d11 device
        D3D_FEATURE_LEVEL featureLevel;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            &featureLevel,
            &d3dContext
        );
        
        if (FAILED(hr)) {
            log::error("screencapture: d3d11createdevice failed 0x{:x}", hr);
            return false;
        }
        
        // get dxgi device
        IDXGIDevice* dxgiDevice = nullptr;
        hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) {
            log::error("screencapture: query dxgidevice failed");
            cleanupDXGI();
            return false;
        }
        
        // get adapter
        IDXGIAdapter* dxgiAdapter = nullptr;
        hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
        dxgiDevice->Release();
        if (FAILED(hr)) {
            log::error("screencapture: get adapter failed");
            cleanupDXGI();
            return false;
        }
        
        // get output
        IDXGIOutput* dxgiOutput = nullptr;
        hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
        dxgiAdapter->Release();
        if (FAILED(hr)) {
            log::error("screencapture: enum outputs failed");
            cleanupDXGI();
            return false;
        }
        
        // get output1 interface for duplication
        hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        dxgiOutput->Release();
        if (FAILED(hr)) {
            log::error("screencapture: query output1 failed");
            cleanupDXGI();
            return false;
        }
        
        // create desktop duplication
        hr = output1->DuplicateOutput(d3dDevice, &deskDupl);
        if (FAILED(hr)) {
            log::error("screencapture: duplicateoutput failed 0x{:x}", hr);
            cleanupDXGI();
            return false;
        }
        
        dxgiInitialized = true;
        log::info("screencapture: dxgi initialized");
        return true;
    }
    
    void cleanupDXGI() {
        if (deskDupl) {
            deskDupl->Release();
            deskDupl = nullptr;
        }
        if (output1) {
            output1->Release();
            output1 = nullptr;
        }
        if (d3dContext) {
            d3dContext->Release();
            d3dContext = nullptr;
        }
        if (d3dDevice) {
            d3dDevice->Release();
            d3dDevice = nullptr;
        }
        dxgiInitialized = false;
    }
    
    void captureLoop() {
        auto lastCapture = std::chrono::steady_clock::now();
        
        while (!shouldStop) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCapture).count();
            
            if (elapsed < frameIntervalMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            lastCapture = now;
            captureSingleFrameDXGI();
        }
    }
    
    void captureSingleFrameDXGI() {
        if (!deskDupl) return;
        
        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        
        // acquire next frame with 100ms timeout
        HRESULT hr = deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                return; // no new frame
            }
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                log::warn("screencapture: access lost, reinitializing");
                cleanupDXGI();
                initializeDXGI();
                return;
            }
            return;
        }
        
        // get texture from resource
        ID3D11Texture2D* acquiredTex = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquiredTex);
        desktopResource->Release();
        if (FAILED(hr)) {
            deskDupl->ReleaseFrame();
            return;
        }
        
        // get texture description
        D3D11_TEXTURE2D_DESC desc;
        acquiredTex->GetDesc(&desc);
        
        int srcW = desc.Width;
        int srcH = desc.Height;
        
        // create staging texture for cpu read
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        
        ID3D11Texture2D* stagingTex = nullptr;
        hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
        if (FAILED(hr)) {
            acquiredTex->Release();
            deskDupl->ReleaseFrame();
            return;
        }
        
        // copy to staging
        d3dContext->CopyResource(stagingTex, acquiredTex);
        acquiredTex->Release();
        
        // map to cpu
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = d3dContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            stagingTex->Release();
            deskDupl->ReleaseFrame();
            return;
        }
        
        // create frame
        VideoFrame frame;
        frame.width = captureWidth;
        frame.height = captureHeight;
        frame.pitch = captureWidth * 3;
        frame.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - captureStartTime).count();
        frame.frameNumber = frameCounter++;
        frame.data.resize(captureWidth * captureHeight * 3);
        
        // copy and convert bgra to rgb
        uint8_t* src = static_cast<uint8_t*>(mapped.pData);
        int srcPitch = mapped.RowPitch;
        
        // simple nearest neighbor downscale and format convert
        for (int y = 0; y < captureHeight; ++y) {
            for (int x = 0; x < captureWidth; ++x) {
                int srcX = (x * srcW) / captureWidth;
                int srcY = (y * srcH) / captureHeight;
                
                if (srcX >= srcW) srcX = srcW - 1;
                if (srcY >= srcH) srcY = srcH - 1;
                
                // bgra to rgb
                uint8_t* pixel = src + (srcY * srcPitch) + (srcX * 4);
                int dstIdx = (y * captureWidth + x) * 3;
                
                frame.data[dstIdx + 0] = pixel[2]; // r
                frame.data[dstIdx + 1] = pixel[1]; // g
                frame.data[dstIdx + 2] = pixel[0]; // b
            }
        }
        
        d3dContext->Unmap(stagingTex, 0);
        stagingTex->Release();
        deskDupl->ReleaseFrame();
        
        // push to queue
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (frameQueue.size() >= maxQueue) {
                frameQueue.pop_front();
            }
            frameQueue.push_back(std::move(frame));
        }
    }
};