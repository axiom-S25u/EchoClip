// written by someone who regrets everything
// meow meow meow meow
#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <eclipse.ffmpeg-api/include/recorder.hpp>

#include <fstream>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#pragma comment(lib, "ole32.lib")
#endif

using namespace geode::prelude;

static std::filesystem::path g_saveDir() {
    return Mod::get()->getSaveDir();
}

static void g_notify(std::string msg, bool err = false) {
    auto icon = err ? NotificationIcon::Error : NotificationIcon::Success;
    Loader::get()->queueInMainThread([msg, icon] {
        Notification::create(msg, icon)->show();
    });
}

static std::string g_safeName(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (std::isalnum(c)) out += c;
        else if (std::isspace(c)) out += '_';
    }
    return out.empty() ? "unknown" : out;
}

struct CapturedFrame {
    std::vector<uint8_t> rgba;
    int64_t frameIndex = 0;
};

#ifdef GEODE_IS_WINDOWS
class MicCapture {
public:
    std::vector<float> g_micSamples;
    std::mutex g_micMtx;
    std::atomic<bool> g_micRunning{false};
    std::thread g_micThread;

    IMMDeviceEnumerator* g_enumerator    = nullptr;
    IMMDevice*           g_micDevice     = nullptr;
    IAudioClient*        g_audioClient   = nullptr;
    IAudioCaptureClient* g_captureClient = nullptr;

    bool start() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_enumerator);
        if (FAILED(hr)) return false;

        hr = g_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &g_micDevice);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_micDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_audioClient);
        if (FAILED(hr)) { cleanup(); return false; }

        WAVEFORMATEX* wfx = nullptr;
        hr = g_audioClient->GetMixFormat(&wfx);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, wfx, nullptr);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_captureClient);
        if (FAILED(hr)) { cleanup(); return false; }

        g_audioClient->Start();
        g_micRunning = true;

        g_micThread = std::thread([this] {
            while (g_micRunning) {
                drainPackets();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        return true;
    }

    void stop() {
        g_micRunning = false;
        if (g_micThread.joinable()) g_micThread.join();
        if (g_audioClient) g_audioClient->Stop();
        cleanup();
    }

    std::vector<float> takeSamples() {
        std::lock_guard<std::mutex> lk(g_micMtx);
        return std::exchange(g_micSamples, {});
    }

private:
    void drainPackets() {
        if (!g_captureClient) return;
        UINT32 packetSize = 0;
        while (SUCCEEDED(g_captureClient->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(g_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && frames > 0) {
                float* fdata = reinterpret_cast<float*>(data);
                std::lock_guard<std::mutex> lk(g_micMtx);
                g_micSamples.insert(g_micSamples.end(), fdata, fdata + (frames * 2));
            }

            g_captureClient->ReleaseBuffer(frames);
        }
    }

    void cleanup() {
        if (g_captureClient) { g_captureClient->Release(); g_captureClient = nullptr; }
        if (g_audioClient)   { g_audioClient->Release();   g_audioClient   = nullptr; }
        if (g_micDevice)     { g_micDevice->Release();     g_micDevice     = nullptr; }
        if (g_enumerator)    { g_enumerator->Release();    g_enumerator    = nullptr; }
    }
};

class GDAudioCapture {
public:
    std::vector<float> g_samples;
    std::mutex g_mtx;
    std::atomic<bool> g_running{false};
    std::thread g_thread;

    IMMDeviceEnumerator* g_enumerator  = nullptr;
    IMMDevice*           g_device      = nullptr;
    IAudioClient*        g_audioClient = nullptr;
    IAudioCaptureClient* g_capture     = nullptr;

    bool start() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_enumerator);
        if (FAILED(hr)) return false;

        hr = g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_device);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_audioClient);
        if (FAILED(hr)) { cleanup(); return false; }

        WAVEFORMATEX* wfx = nullptr;
        hr = g_audioClient->GetMixFormat(&wfx);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wfx, nullptr);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { cleanup(); return false; }

        hr = g_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_capture);
        if (FAILED(hr)) { cleanup(); return false; }

        g_audioClient->Start();
        g_running = true;

        g_thread = std::thread([this] {
            while (g_running) {
                drainPackets();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        return true;
    }

    void stop() {
        g_running = false;
        if (g_thread.joinable()) g_thread.join();
        if (g_audioClient) g_audioClient->Stop();
        cleanup();
    }

    std::vector<float> takeSamples() {
        std::lock_guard<std::mutex> lk(g_mtx);
        return std::exchange(g_samples, {});
    }

private:
    void drainPackets() {
        if (!g_capture) return;
        UINT32 packetSize = 0;
        while (SUCCEEDED(g_capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(g_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && frames > 0) {
                float* fdata = reinterpret_cast<float*>(data);
                std::lock_guard<std::mutex> lk(g_mtx);
                g_samples.insert(g_samples.end(), fdata, fdata + (frames * 2));
            }

            g_capture->ReleaseBuffer(frames);
        }
    }

    void cleanup() {
        if (g_capture)     { g_capture->Release();     g_capture     = nullptr; }
        if (g_audioClient) { g_audioClient->Release(); g_audioClient = nullptr; }
        if (g_device)      { g_device->Release();      g_device      = nullptr; }
        if (g_enumerator)  { g_enumerator->Release();  g_enumerator  = nullptr; }
    }
};
#else
class MicCapture {
public:
    bool start() { return false; }
    void stop() {}
    std::vector<float> takeSamples() { return {}; }
};

class GDAudioCapture {
public:
    bool start() { return false; }
    void stop() {}
    std::vector<float> takeSamples() { return {}; }
};
#endif

struct AttemptMark {
    int64_t frameIndex = 0;
    std::string levelName;
    int attempt = 0;
};

class EchoClipEngine {
public:
    int g_fps        = 60;
    int g_width      = 1920;
    int g_height     = 1080;
    int g_windowSecs = 30;

    std::atomic<bool> g_active{false};
    int64_t           g_frameIdx = 0;

    std::mutex                g_frameMtx;
    std::deque<CapturedFrame> g_frameRing;
    size_t                    g_maxFrames = 0;

    std::mutex              g_markMtx;
    std::deque<AttemptMark> g_marks;

    MicCapture     g_mic;
    GDAudioCapture g_gdAudio;

    std::mutex         g_audioMtx;
    std::vector<float> g_micAccum;
    std::vector<float> g_gdAccum;

    std::mutex g_exportMtx;
    bool       g_exportBusy = false;

    void init() {
        if (g_active) return;

        g_maxFrames = static_cast<size_t>(g_fps * g_windowSecs);
        g_frameIdx  = 0;
        g_active    = true;

        g_mic.start();
        g_gdAudio.start();

        g_notify("echoclip: on");
    }

    void shutdown() {
        if (!g_active) return;
        g_active = false;
        g_mic.stop();
        g_gdAudio.stop();
    }

    void pushFrame(const std::vector<uint8_t>& rgba) {
        {
            auto micSamp = g_mic.takeSamples();
            auto gdSamp  = g_gdAudio.takeSamples();
            std::lock_guard<std::mutex> lk(g_audioMtx);
            g_micAccum.insert(g_micAccum.end(), micSamp.begin(), micSamp.end());
            g_gdAccum.insert(g_gdAccum.end(),   gdSamp.begin(),  gdSamp.end());
        }

        CapturedFrame f;
        f.rgba       = rgba;
        f.frameIndex = g_frameIdx++;

        std::lock_guard<std::mutex> lk(g_frameMtx);
        g_frameRing.push_back(std::move(f));
        while (g_frameRing.size() > g_maxFrames)
            g_frameRing.pop_front();
    }

    void markAttempt() {
        if (!g_active) return;
        AttemptMark m;
        m.frameIndex = g_frameIdx;
        if (auto pl = PlayLayer::get()) {
            if (auto lvl = pl->m_level) m.levelName = lvl->m_levelName;
            m.attempt = pl->m_attempts;
        }
        std::lock_guard<std::mutex> lk(g_markMtx);
        g_marks.push_front(m);
        while (g_marks.size() > 256) g_marks.pop_back();
    }

    void triggerClip() {
        if (!g_active) { g_notify("echoclip not running", true); return; }

        std::lock_guard<std::mutex> expLk(g_exportMtx);
        if (g_exportBusy) { g_notify("clip in progress...", true); return; }
        g_exportBusy = true;

        std::deque<CapturedFrame> frames;
        std::vector<float>        micAudio;
        std::vector<float>        gdAudio;
        AttemptMark               mark;

        {
            std::lock_guard<std::mutex> lk(g_frameMtx);
            frames = g_frameRing;
        }
        {
            std::lock_guard<std::mutex> lk(g_audioMtx);
            micAudio = g_micAccum;
            gdAudio  = g_gdAccum;
        }
        {
            std::lock_guard<std::mutex> lk(g_markMtx);
            if (!g_marks.empty()) mark = g_marks.front();
        }

        if (frames.empty()) {
            g_notify("no frames captured yet", true);
            g_exportBusy = false;
            return;
        }

        std::thread([this,
                     frames   = std::move(frames),
                     micAudio = std::move(micAudio),
                     gdAudio  = std::move(gdAudio),
                     mark]() mutable {
            exportClip(frames, micAudio, gdAudio, mark);
            std::lock_guard<std::mutex> lk(g_exportMtx);
            g_exportBusy = false;
        }).detach();
    }

private:
    void exportClip(
        const std::deque<CapturedFrame>& frames,
        const std::vector<float>& micAudio,
        const std::vector<float>& gdAudio,
        const AttemptMark& mark)
    {
        g_notify("clipping...");

        auto dir = g_saveDir() / "clips";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string stem = g_safeName(mark.levelName) + "_att" + std::to_string(mark.attempt);

        auto videoPath = dir / (stem + "_video.mp4");
        auto mixedPath = dir / (stem + ".mp4");

        {
            ffmpeg::v2::Recorder rec;

            ffmpeg::v2::RenderSettings rs;
            rs.m_width          = static_cast<uint32_t>(g_width);
            rs.m_height         = static_cast<uint32_t>(g_height);
            rs.m_fps            = static_cast<uint16_t>(g_fps);
            rs.m_codec          = "libx264";
            rs.m_bitrate        = 30000000;
            rs.m_outputFile     = videoPath;
            rs.m_pixelFormat    = ffmpeg::v2::PixelFormat::RGBA;
            rs.m_doVerticalFlip = false;

            auto initRes = rec.init(rs);
            if (initRes.isErr()) {
                g_notify("recorder init failed: " + initRes.unwrapErr(), true);
                return;
            }

            for (const auto& f : frames) {
                auto writeRes = rec.writeFrame(f.rgba);
                if (writeRes.isErr())
                    log::warn("echoclip: frame write error: {}", writeRes.unwrapErr());
            }

            rec.stop();
        }

        if (!std::filesystem::exists(videoPath)) {
            g_notify("video encode failed", true);
            return;
        }

        size_t audioLen = std::max(micAudio.size(), gdAudio.size());

        if (audioLen == 0) {
            std::filesystem::rename(videoPath, mixedPath, ec);
            g_notify("clipped (no audio): " + mixedPath.filename().string());
            return;
        }

        std::vector<float> mixed(audioLen, 0.f);
        for (size_t i = 0; i < micAudio.size(); ++i)
            mixed[i] += micAudio[i];
        for (size_t i = 0; i < gdAudio.size(); ++i)
            mixed[i] += gdAudio[i];
        for (auto& s : mixed)
            s = std::clamp(s, -1.f, 1.f);

        auto wavPath = dir / (stem + "_audio.wav");

        {
            std::ofstream wav(wavPath, std::ios::binary);
            uint32_t sampleRate   = 48000;
            uint16_t channels     = 2;
            uint16_t bitsPerSample = 32;
            uint32_t byteRate     = sampleRate * channels * (bitsPerSample / 8);
            uint16_t blockAlign   = channels * (bitsPerSample / 8);
            uint32_t dataSize     = static_cast<uint32_t>(mixed.size() * sizeof(float));
            uint32_t chunkSize    = 36 + dataSize;

            wav.write("RIFF", 4);
            wav.write(reinterpret_cast<const char*>(&chunkSize), 4);
            wav.write("WAVE", 4);
            wav.write("fmt ", 4);
            uint32_t fmtSize = 18;
            uint16_t audioFmt = 3;
            wav.write(reinterpret_cast<const char*>(&fmtSize), 4);
            wav.write(reinterpret_cast<const char*>(&audioFmt), 2);
            wav.write(reinterpret_cast<const char*>(&channels), 2);
            wav.write(reinterpret_cast<const char*>(&sampleRate), 4);
            wav.write(reinterpret_cast<const char*>(&byteRate), 4);
            wav.write(reinterpret_cast<const char*>(&blockAlign), 2);
            wav.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
            uint16_t extSize = 0;
            wav.write(reinterpret_cast<const char*>(&extSize), 2);
            wav.write("data", 4);
            wav.write(reinterpret_cast<const char*>(&dataSize), 4);
            wav.write(reinterpret_cast<const char*>(mixed.data()), dataSize);
        }

        std::string cmd = "ffmpeg -y -i \"" + videoPath.string() + "\" -i \"" + wavPath.string()
            + "\" -c:v copy -c:a aac -shortest \"" + mixedPath.string() + "\"";

        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back(0);

        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;

        bool ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

        if (ok) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (code != 0) ok = false;
        }

        std::filesystem::remove(videoPath, ec);
        std::filesystem::remove(wavPath, ec);

        if (ok)
            g_notify("clipped: " + mixedPath.filename().string());
        else
            g_notify("audio mix failed", true);
    }
};

static EchoClipEngine g_engine;

class $modify(EchoClipBGL, GJBaseGameLayer) {
    struct Fields {
        bool g_isyapping = false;
    };

    void update(float dt) override {
        GJBaseGameLayer::update(dt);

        if (!g_engine.g_active) return;
        if (m_gameState.m_currentProgress <= 0) return;

        m_fields->g_isyapping = true;
        captureCurrentFrame();
        m_fields->g_isyapping = false;
    }

    void visit() override {
        if (!m_fields->g_isyapping) {
            GJBaseGameLayer::visit();
            return;
        }
        GJBaseGameLayer::visit();
    }

    void captureCurrentFrame() {
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSizeInPixels();

        int w = static_cast<int>(winSize.width);
        int h = static_cast<int>(winSize.height);
        size_t dataSize = static_cast<size_t>(w * h * 4);

        std::vector<uint8_t> rgba(dataSize);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        std::vector<uint8_t> flipped(dataSize);
        size_t rowBytes = static_cast<size_t>(w * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(flipped.data() + y * rowBytes,
                        rgba.data() + (h - 1 - y) * rowBytes,
                        rowBytes);

        g_engine.pushFrame(flipped);
    }
};

class $modify(EchoClipPL, PlayLayer) {
    void resetLevel() {
        if (g_engine.g_active) g_engine.markAttempt();
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (g_engine.g_active) {
            g_engine.markAttempt();
            g_engine.triggerClip();
        }
    }
};

$execute {
    static auto g_keyHandle = geode::KeyboardInputEvent(cocos2d::enumKeyCodes::KEY_F6).listen(
        [](cocos2d::enumKeyCodes, geode::KeyboardInputData& data) {
            if (data.action == geode::KeyboardInputData::Action::Press)
                g_engine.triggerClip();
        }
    );

    Loader::get()->queueInMainThread([] {
        g_engine.init();
    });
}