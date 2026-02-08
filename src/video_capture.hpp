#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>

using namespace geode::prelude;
using Microsoft::WRL::ComPtr;

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
    std::atomic<bool> isCapturing{false};
    std::thread captureThread;
    
    int captureWidth = 1280;
    int captureHeight = 720;
    int targetFPS = 30;
    
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> stagingTexture;
    
    std::chrono::steady_clock::time_point captureStartTime;
    int64_t frameCounter = 0;
    static constexpr size_t MAX_QUEUE = 60;

public:
    ScreenCapture(int fps = 30, int width = 1280, int height = 720)
        : targetFPS(fps), captureWidth(width), captureHeight(height) {}
    
    ~ScreenCapture() {
        stop();
    }
    
    bool start() {
        if (isCapturing) return true;
        
        if (!initializeDXGI()) {
            log::error("screencapture: failed to initialize dxgi");
            return false;
        }
        
        isCapturing = true;
        captureStartTime = std::chrono::steady_clock::now();
        frameCounter = 0;
        captureThread = std::thread([this] { captureLoop(); });
        return true;
    }
    
    void stop() {
        if (!isCapturing) return;
        isCapturing = false;
        if (captureThread.joinable()) captureThread.join();
        cleanup();
    }
    
    bool getFrame(VideoFrame& outFrame) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (frameQueue.empty()) return false;
        
        outFrame = frameQueue.front();
        frameQueue.pop_front();
        return true;
    }
    
    std::vector<VideoFrame> popAllFrames() {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::vector<VideoFrame> frames(frameQueue.begin(), frameQueue.end());
        frameQueue.clear();
        return frames;
    }

private:
    void cleanup() {
        stagingTexture.Reset();
        duplication.Reset();
        context.Reset();
        device.Reset();
        output.Reset();
        adapter.Reset();
        factory.Reset();
    }

    bool initializeDXGI() {
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) return false;
        if (FAILED(factory->EnumAdapters1(0, adapter.GetAddressOf()))) return false;
        if (FAILED(adapter->EnumOutputs(0, output.GetAddressOf()))) return false;
        
        D3D_FEATURE_LEVEL featureLevel;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), &featureLevel, context.GetAddressOf()))) return false;
        
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)output1.GetAddressOf()))) return false;
        if (FAILED(output1->DuplicateOutput(device.Get(), duplication.GetAddressOf()))) return false;
        
        return true;
    }
    
    void captureLoop() {
        int frameInterval = 1000 / targetFPS;
        
        while (isCapturing) {
            auto loopStart = std::chrono::steady_clock::now();
            
            if (!duplication) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            
            HRESULT hr = duplication->AcquireNextFrame(50, &frameInfo, desktopResource.GetAddressOf());
            
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                continue; 
            }
            if (FAILED(hr)) { 
                duplication->ReleaseFrame(); 
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue; 
            }
            
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(desktopResource.As(&texture))) {
                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);
                
                if (!stagingTexture) {
                    D3D11_TEXTURE2D_DESC stDesc = desc;
                    stDesc.Usage = D3D11_USAGE_STAGING;
                    stDesc.BindFlags = 0;
                    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stDesc.MiscFlags = 0;
                    device->CreateTexture2D(&stDesc, nullptr, stagingTexture.GetAddressOf());
                }
                
                context->CopyResource(stagingTexture.Get(), texture.Get());
                
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                    VideoFrame frame;
                    frame.width = captureWidth;
                    frame.height = captureHeight;
                    frame.pitch = captureWidth * 3;
                    frame.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - captureStartTime).count();
                    frame.frameNumber = frameCounter++;
                    frame.data.resize(captureWidth * captureHeight * 3);
                    
                    uint8_t* src = static_cast<uint8_t*>(map.pData);
                    uint8_t* dst = frame.data.data();
                    
                    int copyW = std::min((int)desc.Width, captureWidth);
                    int copyH = std::min((int)desc.Height, captureHeight);
                    
                    for (int y = 0; y < copyH; ++y) {
                        for (int x = 0; x < copyW; ++x) {
                            int srcIdx = y * map.RowPitch + x * 4;
                            int dstIdx = (y * captureWidth + x) * 3;
                            
                            dst[dstIdx + 0] = src[srcIdx + 2];
                            dst[dstIdx + 1] = src[srcIdx + 1];
                            dst[dstIdx + 2] = src[srcIdx + 0];
                        }
                    }
                    
                    context->Unmap(stagingTexture.Get(), 0);
                    
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        if (frameQueue.size() < MAX_QUEUE) {
                            frameQueue.push_back(frame);
                        }
                    }
                }
            }
            
            duplication->ReleaseFrame();
            
            auto loopEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart).count();
            if (elapsed < frameInterval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(frameInterval - elapsed));
            }
        }
    }
};