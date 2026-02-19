// written by someone who ACTUALLY learned gpu programming this time
// meow meow
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <queue>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <fmod.hpp>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace geode::prelude;

static std::string getTempOutputPath() {
    return (Mod::get()->getSaveDir() / "echoclip_temp.mp4").string();
}

static std::string getOutputDir() {
    return Mod::get()->getSaveDir().string();
}

void showNotification(std::string message, bool error = false) {
    auto icon = error ? NotificationIcon::Error : NotificationIcon::Success;
    Notification::create(message, icon)->show();
}

template<typename T>
class LockFreeRingBuffer {
private:
    std::vector<T> buffer;
    std::atomic<size_t> writeIdx{0};
    std::atomic<size_t> readIdx{0};
    const size_t capacity;
    const size_t mask;

public:
    explicit LockFreeRingBuffer(size_t cap) 
        : capacity(cap), mask(cap - 1) {
        if ((cap & (cap - 1)) != 0) {
            throw std::runtime_error("Capacity must be power of 2");
        }
        buffer.resize(capacity);
    }

    bool push(T&& item) {
        size_t w = writeIdx.load(std::memory_order_relaxed);
        size_t nextW = (w + 1) & mask;
        size_t r = readIdx.load(std::memory_order_acquire);
        
        if (nextW == r) return false;
        
        buffer[w] = std::move(item);
        writeIdx.store(nextW, std::memory_order_release);
        return true;
    }

    bool pop(T& outItem) {
        size_t r = readIdx.load(std::memory_order_relaxed);
        size_t w = writeIdx.load(std::memory_order_acquire);
        
        if (r == w) return false;
        
        outItem = std::move(buffer[r]);
        readIdx.store((r + 1) & mask, std::memory_order_release);
        return true;
    }

    size_t getSize() {
        size_t w = writeIdx.load(std::memory_order_acquire);
        size_t r = readIdx.load(std::memory_order_acquire);
        return (w - r) & mask;
    }

    bool isNearFull(size_t threshold) {
        return getSize() >= threshold;
    }
};

struct VideoFrame {
    std::vector<uint8_t> data;
    size_t width = 0;
    size_t height = 0;
    int64_t frameNumber = 0;
};

class FramePool {
private:
    std::queue<VideoFrame> available;
    std::mutex mtx;
    size_t frameWidth;
    size_t frameHeight;
    size_t frameSize;
    const size_t poolSize = 128;

public:
    FramePool(size_t w, size_t h) : frameWidth(w), frameHeight(h), frameSize(w * h * 4) {
        for (size_t i = 0; i < poolSize; ++i) {
            VideoFrame f;
            f.data.reserve(frameSize);
            f.data.resize(frameSize);
            f.width = w;
            f.height = h;
            available.push(std::move(f));
        }
    }

    VideoFrame acquire() {
        std::lock_guard<std::mutex> lock(mtx);
        if (!available.empty()) {
            VideoFrame f = std::move(available.front());
            available.pop();
            return f;
        }
        VideoFrame f;
        f.data.reserve(frameSize);
        f.data.resize(frameSize);
        f.width = frameWidth;
        f.height = frameHeight;
        return f;
    }

    void release(VideoFrame&& f) {
        std::lock_guard<std::mutex> lock(mtx);
        if (available.size() < poolSize) {
            available.push(std::move(f));
        }
    }
};

const char* SCALE_SHADER = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET {
    return tex.Sample(samp, input.uv);
}
)";

const char* VS_SHADER = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)";

class ScreenCapture {
private:
    LockFreeRingBuffer<VideoFrame> frameQueue{256};
    std::unique_ptr<FramePool> framePool;
    
    int captureWidth = 854;
    int captureHeight = 480;
    int targetFPS = 30;
    int64_t frameCounter = 0;
    
    std::atomic<bool> isCapturing{false};
    std::atomic<bool> shouldStop{false};
    std::thread captureThread;
    
    std::chrono::high_resolution_clock::time_point nextFrameTime;
    long long frameIntervalUs = 0;
    
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIOutputDuplication* deskDupl = nullptr;
    IDXGIOutput1* output1 = nullptr;
    
    ID3D11Texture2D* scaledRT = nullptr;
    ID3D11Texture2D* scaledStaging = nullptr;
    ID3D11RenderTargetView* scaledRTV = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    
    std::atomic<bool> dxgiInitialized{false};
    std::mutex reinitMtx;

public:
    ScreenCapture(int fps = 30, int width = 854, int height = 480)
        : captureWidth(width), captureHeight(height), targetFPS(fps) {
        frameIntervalUs = (1000000LL / fps);
        framePool = std::make_unique<FramePool>(width, height);
        log::info("screencapture: {}x{} @ {}fps (real gpu scaling)", width, height, fps);
    }
    
    ~ScreenCapture() {
        stop();
        cleanupDXGI();
    }
    
    bool start() {
        if (isCapturing) return true;
        
        if (!initializeDXGI()) {
            log::error("screencapture: dxgi init failed");
            return false;
        }
        
        frameCounter = 0;
        shouldStop = false;
        isCapturing = true;
        nextFrameTime = std::chrono::high_resolution_clock::now();
        
        captureThread = std::thread([this] { captureLoop(); });
        SetThreadPriority(captureThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        
        return true;
    }
    
    void stop() {
        isCapturing = false;
        shouldStop = true;
        
        if (captureThread.joinable()) {
            captureThread.join();
        }
    }
    
    bool getFrame(VideoFrame& outFrame) {
        return frameQueue.pop(outFrame);
    }

private:
    bool initializeDXGI() {
        std::lock_guard<std::mutex> lock(reinitMtx);
        
        HRESULT hr;
        
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
        
        if (FAILED(hr)) return false;
        
        IDXGIDevice* dxgiDevice = nullptr;
        hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) {
            cleanupDXGI();
            return false;
        }
        
        IDXGIAdapter* dxgiAdapter = nullptr;
        hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
        dxgiDevice->Release();
        if (FAILED(hr)) {
            cleanupDXGI();
            return false;
        }
        
        IDXGIOutput* dxgiOutput = nullptr;
        hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
        dxgiAdapter->Release();
        if (FAILED(hr)) {
            cleanupDXGI();
            return false;
        }
        
        hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        dxgiOutput->Release();
        if (FAILED(hr)) {
            cleanupDXGI();
            return false;
        }
        
        hr = output1->DuplicateOutput(d3dDevice, &deskDupl);
        if (FAILED(hr)) {
            cleanupDXGI();
            return false;
        }
        
        if (!createScalingPipeline()) {
            cleanupDXGI();
            return false;
        }
        
        dxgiInitialized = true;
        return true;
    }
    
    bool createScalingPipeline() {
        HRESULT hr;
        
        ID3DBlob* vsBlob = nullptr;
        hr = D3DCompile(VS_SHADER, strlen(VS_SHADER), nullptr, nullptr, nullptr, 
                       "main", "vs_4_0", 0, 0, &vsBlob, nullptr);
        if (FAILED(hr)) return false;
        
        hr = d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 
                                           nullptr, &vs);
        if (FAILED(hr)) {
            vsBlob->Release();
            return false;
        }
        
        ID3DBlob* psBlob = nullptr;
        hr = D3DCompile(SCALE_SHADER, strlen(SCALE_SHADER), nullptr, nullptr, nullptr,
                       "main", "ps_4_0", 0, 0, &psBlob, nullptr);
        if (FAILED(hr)) {
            vsBlob->Release();
            return false;
        }
        
        hr = d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, &ps);
        psBlob->Release();
        if (FAILED(hr)) {
            vsBlob->Release();
            return false;
        }
        
        D3D11_INPUT_ELEMENT_DESC layout_[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        
        hr = d3dDevice->CreateInputLayout(layout_, 2, vsBlob->GetBufferPointer(),
                                          vsBlob->GetBufferSize(), &layout);
        vsBlob->Release();
        if (FAILED(hr)) return false;
        
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        
        hr = d3dDevice->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr)) return false;
        
        float vertices[] = {
            -1, 1, 0, 0,
             1, 1, 1, 0,
            -1,-1, 0, 1,
             1,-1, 1, 1
        };
        
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(vertices);
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        
        D3D11_SUBRESOURCE_DATA vbData = { vertices };
        hr = d3dDevice->CreateBuffer(&vbDesc, &vbData, &vb);
        if (FAILED(hr)) return false;
        
        D3D11_TEXTURE2D_DESC rtDesc = {};
        rtDesc.Width = captureWidth;
        rtDesc.Height = captureHeight;
        rtDesc.MipLevels = 1;
        rtDesc.ArraySize = 1;
        rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtDesc.SampleDesc.Count = 1;
        rtDesc.Usage = D3D11_USAGE_DEFAULT;
        rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        
        hr = d3dDevice->CreateTexture2D(&rtDesc, nullptr, &scaledRT);
        if (FAILED(hr)) return false;
        
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        
        hr = d3dDevice->CreateRenderTargetView(scaledRT, &rtvDesc, &scaledRTV);
        if (FAILED(hr)) return false;
        
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = captureWidth;
        stagingDesc.Height = captureHeight;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        
        hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &scaledStaging);
        if (FAILED(hr)) return false;
        
        return true;
    }
    
    void cleanupDXGI() {
        if (sampler) {
            sampler->Release();
            sampler = nullptr;
        }
        if (layout) {
            layout->Release();
            layout = nullptr;
        }
        if (ps) {
            ps->Release();
            ps = nullptr;
        }
        if (vs) {
            vs->Release();
            vs = nullptr;
        }
        if (vb) {
            vb->Release();
            vb = nullptr;
        }
        if (scaledRTV) {
            scaledRTV->Release();
            scaledRTV = nullptr;
        }
        if (scaledStaging) {
            scaledStaging->Release();
            scaledStaging = nullptr;
        }
        if (scaledRT) {
            scaledRT->Release();
            scaledRT = nullptr;
        }
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
    
    void reinitDXGI() {
        std::lock_guard<std::mutex> lock(reinitMtx);
        log::warn("screencapture: reinitializing dxgi");
        cleanupDXGI();
        if (!initializeDXGI()) {
            log::error("screencapture: reinit failed");
        }
    }
    
    void captureLoop() {
        while (!shouldStop) {
            if (frameQueue.isNearFull(200)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            
            auto now = std::chrono::high_resolution_clock::now();
            
            if (now < nextFrameTime) {
                auto sleepTime = nextFrameTime - now;
                if (sleepTime.count() > 0) {
                    std::this_thread::sleep_until(nextFrameTime);
                }
                continue;
            }
            
            nextFrameTime += std::chrono::microseconds(frameIntervalUs);
            
            if (!dxgiInitialized) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            captureSingleFrame();
        }
    }
    
    void captureSingleFrame() {
        if (!deskDupl) return;
        
        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        
        HRESULT hr = deskDupl->AcquireNextFrame(33, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                reinitDXGI();
                return;
            }
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                return;
            }
            if (desktopResource) desktopResource->Release();
            return;
        }
        
        ID3D11Texture2D* acquiredTex = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&acquiredTex);
        desktopResource->Release();
        if (FAILED(hr)) {
            deskDupl->ReleaseFrame();
            return;
        }
        
        D3D11_TEXTURE2D_DESC desc;
        acquiredTex->GetDesc(&desc);
        
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        
        ID3D11ShaderResourceView* currentSRV = nullptr;
        hr = d3dDevice->CreateShaderResourceView(acquiredTex, &srvDesc, &currentSRV);
        if (FAILED(hr)) {
            acquiredTex->Release();
            deskDupl->ReleaseFrame();
            return;
        }
        
        d3dContext->OMSetRenderTargets(1, &scaledRTV, nullptr);
        
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(captureWidth);
        vp.Height = static_cast<float>(captureHeight);
        vp.MaxDepth = 1.0f;
        d3dContext->RSSetViewports(1, &vp);
        
        d3dContext->IASetInputLayout(layout);
        d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        
        UINT stride = 16;
        UINT offset = 0;
        d3dContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        
        d3dContext->VSSetShader(vs, nullptr, 0);
        d3dContext->PSSetShader(ps, nullptr, 0);
        d3dContext->PSSetShaderResources(0, 1, &currentSRV);
        d3dContext->PSSetSamplers(0, 1, &sampler);
        
        float clearColor[4] = { 0, 0, 0, 1 };
        d3dContext->ClearRenderTargetView(scaledRTV, clearColor);
        d3dContext->Draw(4, 0);
        
        ID3D11ShaderResourceView* nullSRV = nullptr;
        d3dContext->PSSetShaderResources(0, 1, &nullSRV);
        
        currentSRV->Release();
        
        d3dContext->CopyResource(scaledStaging, scaledRT);
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = d3dContext->Map(scaledStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            d3dContext->Flush();
            hr = d3dContext->Map(scaledStaging, 0, D3D11_MAP_READ, 0, &mapped);
        }
        
        if (FAILED(hr)) {
            acquiredTex->Release();
            deskDupl->ReleaseFrame();
            return;
        }
        
        VideoFrame frame = framePool->acquire();
        frame.frameNumber = frameCounter++;
        
        uint8_t* src = static_cast<uint8_t*>(mapped.pData);
        int srcPitch = mapped.RowPitch;
        
        for (int y = 0; y < captureHeight; ++y) {
            std::memcpy(frame.data.data() + (y * captureWidth * 4),
                       src + (y * srcPitch),
                       captureWidth * 4);
        }
        
        d3dContext->Unmap(scaledStaging, 0);
        acquiredTex->Release();
        deskDupl->ReleaseFrame();
        
        if (!frameQueue.push(std::move(frame))) {
            framePool->release(std::move(frame));
        }
    }
};

class FFmpegEncoder {
private:
    std::string outputPath;
    int width, height, fps;
    std::atomic<bool> isEncoding{false};
    std::atomic<bool> shouldStop{false};
    std::thread encodingThread;
    LockFreeRingBuffer<VideoFrame> videoQueue{512};
    
    HANDLE hPipeWrite = NULL;
    HANDLE hPipeRead = NULL;
    PROCESS_INFORMATION pi = { 0 };
    std::atomic<bool> writerActive{true};

public:
    FFmpegEncoder(const std::string& output, int w, int h, int f)
        : outputPath(output), width(w), height(h), fps(f) {}
    
    ~FFmpegEncoder() { stop(); }
    
    static int runSilent(std::string cmd) {
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;
        
        std::vector<char> buf(cmd.begin(), cmd.end()); 
        buf.push_back(0);
        
        if (!CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, 
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, 
            NULL, NULL, &si, &pi)) {
            return -1;
        }
        
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (exitCode == 0) ? 0 : 1;
    }
    
    bool initialize() {
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&hPipeRead, &hPipeWrite, &saAttr, 2 * 1024 * 1024)) {
            return false;
        }
        SetHandleInformation(hPipeWrite, HANDLE_FLAG_INHERIT, 0);

        std::ostringstream cmd;
        cmd << "ffmpeg -y -f rawvideo -pix_fmt bgra -s " << width << "x" << height 
            << " -r " << fps << " -thread_queue_size 8192 -i - "
            << "-c:v libx264 -preset ultrafast -tune zerolatency -crf 23 -pix_fmt yuv420p "
            << "-vsync cfr -r " << fps << " \"" << outputPath << "\"";

        std::string cmdStr = cmd.str();
        std::vector<char> buf(cmdStr.begin(), cmdStr.end());
        buf.push_back(0);

        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdInput = hPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.wShowWindow = SW_HIDE;

        if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, 
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, 
            NULL, NULL, &si, &pi)) {
            CloseHandle(hPipeRead);
            CloseHandle(hPipeWrite);
            hPipeRead = NULL;
            hPipeWrite = NULL;
            return false;
        }

        isEncoding = true;
        shouldStop = false;
        writerActive = true;
        encodingThread = std::thread([this] { encodingLoop(); });
        SetThreadPriority(encodingThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        return true;
    }
    
    void stop() {
        if (!isEncoding) return;
        shouldStop = true;
        writerActive = false;
        if (encodingThread.joinable()) encodingThread.join();
        
        if (hPipeWrite) {
            CloseHandle(hPipeWrite);
            hPipeWrite = NULL;
        }
        if (hPipeRead) {
            CloseHandle(hPipeRead);
            hPipeRead = NULL;
        }
        
        if (pi.hProcess) {
            WaitForSingleObject(pi.hProcess, 3000);
            TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi.hProcess = NULL;
        }
        isEncoding = false;
    }
    
    void flush() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!shouldStop && std::chrono::steady_clock::now() < deadline) {
            if (videoQueue.getSize() == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        writerActive = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    bool submitVideoFrame(VideoFrame&& frame) {
        if (!isEncoding || shouldStop || !writerActive) return false;
        return videoQueue.push(std::move(frame));
    }

private:
    void encodingLoop() {
        while (!shouldStop || videoQueue.getSize() > 0) {
            VideoFrame frame;
            if (!videoQueue.pop(frame)) {
                if (!writerActive) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            if (hPipeWrite) {
                DWORD toWrite = static_cast<DWORD>(frame.data.size());
                DWORD written = 0;
                
                if (!WriteFile(hPipeWrite, frame.data.data(), toWrite, &written, NULL)) {
                    log::error("ffmpeg: write failed");
                    writerActive = false;
                    break;
                }
            }
        }
    }
};

struct AttemptMarker {
    int64_t frameNumber;
    std::string levelName;
    int gdAttemptCount = 0;
};

class EchoClipEngine {
public:
    std::atomic<bool> isRecording{false};
    std::atomic<bool> shouldStop{false};
    
    std::unique_ptr<ScreenCapture> videoCapture;
    std::unique_ptr<FFmpegEncoder> encoder;
    
    std::thread audioMixThread;
    
    int64_t videoFrameCounter = 0;
    int frameWidth = 854;
    int frameHeight = 480;
    int targetFPS = 30;
    int attemptWindow = 5;
    
    std::mutex attemptMutex;
    std::deque<AttemptMarker> attemptMarkers;
    
    EchoClipEngine() = default;
    
    ~EchoClipEngine() {
        shutdown();
    }
    
    void shutdown() {
        if (!isRecording) return;
        shouldStop = true;
        if (audioMixThread.joinable()) audioMixThread.join();
        if (videoCapture) videoCapture->stop();
        if (encoder) encoder->stop();
        isRecording = false;
    }
    
    bool initialize() {
        if (isRecording) return true;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return false;

        std::filesystem::create_directories(getOutputDir());
        
        videoCapture = std::make_unique<ScreenCapture>(targetFPS, frameWidth, frameHeight);
        if (!videoCapture->start()) return false;
        
        encoder = std::make_unique<FFmpegEncoder>(getTempOutputPath(), frameWidth, frameHeight, targetFPS);
        if (!encoder->initialize()) return false;
        
        isRecording = true;
        shouldStop = false;
        videoFrameCounter = 0;
        
        audioMixThread = std::thread([this] { audioMixLoop(); });
        SetThreadPriority(audioMixThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        
        Notification::create("echoclip: recording started", NotificationIcon::Success)->show();
        
        return true;
    }

    void recordAttempt() {
        if (!isRecording || shouldStop) return;
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        std::string levelName = "unknown";
        int gdAttempts = 0;
        
        if (auto pm = PlayLayer::get()) {
            if (auto level = pm->m_level) {
                levelName = level->m_levelName;
            }
            gdAttempts = pm->m_attempts;
        }
        
        AttemptMarker marker;
        marker.frameNumber = videoFrameCounter;
        marker.levelName = levelName;
        marker.gdAttemptCount = gdAttempts;
        
        attemptMarkers.push_front(marker);
        while (attemptMarkers.size() > static_cast<size_t>(attemptWindow)) {
            attemptMarkers.pop_back();
        }
    }
    
    bool exportClip(const std::string& outputPath) {
        std::lock_guard<std::mutex> lock(attemptMutex);
        if (attemptMarkers.empty()) {
            showNotification("no attempts", true);
            return false;
        }
        
        if (encoder) encoder->flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        std::string tempPath = getTempOutputPath();
        if (!std::filesystem::exists(tempPath)) {
            log::error("temp file missing");
            return false;
        }
        
        const AttemptMarker& newest = attemptMarkers.front();
        const AttemptMarker& oldest = attemptMarkers.back();
        
        double frameDuration = 1.0 / targetFPS;
        double startTime = oldest.frameNumber * frameDuration;
        double duration = (newest.frameNumber - oldest.frameNumber) * frameDuration;
        
        if (duration < 2.0) duration = 2.0;
        if (startTime > 0.5) startTime -= 0.5;
        duration += 0.5;
        
        std::ostringstream cmd;
        cmd << "ffmpeg -y -ss " << std::fixed << std::setprecision(3) << startTime
            << " -t " << duration
            << " -i \"" << tempPath << "\""
            << " -c:v copy -c:a copy -avoid_negative_ts make_zero "
            << "-fflags +genpts \"" << outputPath << "\"";
        
        return FFmpegEncoder::runSilent(cmd.str()) == 0;
    }

    std::string generateClipFilename() {
        std::lock_guard<std::mutex> lock(attemptMutex);
        if (attemptMarkers.empty()) return getOutputDir() + "/clip_error.mp4";
        
        std::string levelName = attemptMarkers.front().levelName;
        int gdAttempt = attemptMarkers.front().gdAttemptCount;
        
        std::string cleanName;
        for (char c : levelName) {
            if (std::isalnum(c)) cleanName += c;
            else if (std::isspace(c)) cleanName += '_';
        }
        
        std::ostringstream filename;
        filename << getOutputDir() << "/clip_" << (cleanName.empty() ? "unknown" : cleanName) 
                 << "_att" << gdAttempt << ".mp4";
        return filename.str();
    }

private:
    void audioMixLoop() {
        while (!shouldStop) {
            VideoFrame vframe;
            if (videoCapture && videoCapture->getFrame(vframe)) {
                videoFrameCounter = vframe.frameNumber + 1;
                if (encoder && !shouldStop) {
                    encoder->submitVideoFrame(std::move(vframe));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};

static std::unique_ptr<EchoClipEngine> g_engine;

class $modify(AutoStartMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (!g_engine) {
            g_engine = std::make_unique<EchoClipEngine>();
        }
        if (g_engine && !g_engine->isRecording) g_engine->initialize();
        return true;
    }
};

class $modify(PlayLayerEchoClip, PlayLayer) {
    void resetLevel() {
        if (g_engine && g_engine->isRecording) g_engine->recordAttempt();
        PlayLayer::resetLevel();
    }
    
    void levelComplete() {
        if (g_engine && g_engine->isRecording) g_engine->recordAttempt();
        PlayLayer::levelComplete();
    }
};

class $modify(KeyboardHandlerDirector, CCDirector) {
    void update(float dt) {
        CCDirector::update(dt);
        
        static bool lastF6 = false;
        bool currentF6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        
        if (currentF6 && !lastF6) {
            if (!g_engine || !g_engine->isRecording) {
                showNotification("engine off", true);
            } else {
                showNotification("clipping...");
                std::string path = g_engine->generateClipFilename();
                std::thread([path]() {
                    if (g_engine && g_engine->exportClip(path)) {
                        showNotification("clipped");
                    } else {
                        showNotification("save failed", true);
                    }
                }).detach();
            }
        }
        lastF6 = currentF6;
    }
};