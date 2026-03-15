#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <eclipse.ffmpeg-api/include/recorder.hpp>
#include <eclipse.ffmpeg-api/include/audio_mixer.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <string>
#include <queue>
#include <memory>
#include <algorithm>
#include <cctype>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shellapi.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

using namespace geode::prelude;

static void notif(std::string msg, bool bad = false) {
    auto ic = bad ? NotificationIcon::Error : NotificationIcon::Success;
    Loader::get()->queueInMainThread([msg, ic] {
        Notification::create(msg, ic)->show();
    });
}

static std::string safename(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-') {
            o += c;
        } else {
            o += '_';
        }
    }
    return o.empty() ? "level" : o;
}

static std::string makeUniqueStem(const std::string& base) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    return base + "_" + std::to_string(ms);
}

struct RawFrame {
    std::vector<uint8_t> px;
    int w = 0;
    int h = 0;
    int64_t idx = 0;
};

struct AttMark {
    int64_t frameIdx = 0;
    std::string lvlName;
    int att = 0;
};

#ifdef GEODE_IS_WINDOWS

static std::string getDeviceFriendlyName(IMMDevice* dev) {
    if (!dev) return "unknown";
    IPropertyStore* props = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return "unknown";

    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string result = "unknown";

    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        int sz = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        if (sz > 0) {
            std::string buf(sz, 0);
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, buf.data(), sz, nullptr, nullptr);
            buf.resize(strlen(buf.c_str()));
            result = buf;
        }
    }

    PropVariantClear(&pv);
    props->Release();
    return result;
}

class WasapiCapture {
public:
    enum class Mode { Mic, Loopback };
    explicit WasapiCapture(Mode m) : m_mode(m) {}

    bool start(int clipLenSec) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator)))
            return false;

        EDataFlow flow = (m_mode == Mode::Mic) ? eCapture : eRender;
        if (FAILED(m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device))) {
            cleanup();
            return false;
        }

        m_deviceName = getDeviceFriendlyName(m_device);

        if (FAILED(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_client))) {
            cleanup();
            return false;
        }

        WAVEFORMATEX* wfx = nullptr;
        if (FAILED(m_client->GetMixFormat(&wfx))) {
            cleanup();
            return false;
        }

        m_channels = wfx->nChannels;
        m_sampleRate = wfx->nSamplesPerSec;
        m_maxSamples = (size_t)m_sampleRate * m_channels * clipLenSec;

        DWORD flags = (m_mode == Mode::Loopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        HRESULT hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, wfx, nullptr);
        CoTaskMemFree(wfx);

        if (FAILED(hr)) {
            cleanup();
            return false;
        }

        if (FAILED(m_client->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capture))) {
            cleanup();
            return false;
        }

        m_client->Start();
        m_running = true;
        m_thread = std::thread([this] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (m_running) {
                pull();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CoUninitialize();
        });

        return true;
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_client) m_client->Stop();
        cleanup();
    }

    std::vector<float> takeAll() {
        std::lock_guard<std::mutex> lk(m_mtx);
        return std::vector<float>(m_buf.begin(), m_buf.end());
    }

    std::string getDeviceName() const { return m_deviceName; }
    int getChannels() const { return m_channels; }
    int getSampleRate() const { return m_sampleRate; }

private:
    void pull() {
        if (!m_capture) return;

        UINT32 packetSize = 0;
        while (SUCCEEDED(m_capture->GetNextPacketSize(&packetSize)) && packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && frames > 0) {
                auto fd = reinterpret_cast<float*>(data);
                int ch = m_channels > 0 ? m_channels : 2;

                std::lock_guard<std::mutex> lk(m_mtx);
                for (UINT32 i = 0; i < frames; i++) {
                    float left = fd[i * ch];
                    float right = (ch > 1) ? fd[i * ch + 1] : left;
                    m_buf.push_back(left);
                    m_buf.push_back(right);
                }

                while (m_buf.size() > m_maxSamples) {
                    m_buf.pop_front();
                }
            }

            m_capture->ReleaseBuffer(frames);
        }
    }

    void cleanup() {
        if (m_capture) {
            m_capture->Release();
            m_capture = nullptr;
        }
        if (m_client) {
            m_client->Release();
            m_client = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
        if (m_enumerator) {
            m_enumerator->Release();
            m_enumerator = nullptr;
        }
    }

    Mode m_mode;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::mutex m_mtx;
    std::deque<float> m_buf;
    size_t m_maxSamples = 48000 * 2 * 30;
    std::string m_deviceName = "unknown";
    int m_channels = 2;
    int m_sampleRate = 48000;
    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_client = nullptr;
    IAudioCaptureClient* m_capture = nullptr;
};

#else

class WasapiCapture {
public:
    enum class Mode { Mic, Loopback };
    explicit WasapiCapture(Mode) {}
    bool start(int) { return false; }
    void stop() {}
    std::vector<float> takeAll() { return {}; }
    std::string getDeviceName() const { return "unsupported"; }
    int getChannels() const { return 2; }
    int getSampleRate() const { return 48000; }
};

#endif

class Engine {
public:
    std::atomic<bool> active{false};
    std::atomic<int64_t> frameIdx{0};

    WasapiCapture mic{WasapiCapture::Mode::Mic};
    WasapiCapture gd{WasapiCapture::Mode::Loopback};

    std::mutex exportMtx;
    std::atomic<bool> exporting{false};

    int bestPct = 0;
    std::mutex bestMtx;

    int targetFps = 30;
    int clipLenSec = 30;

    std::mutex markMtx;
    std::deque<AttMark> marks;

    std::mutex ringMtx;
    std::vector<RawFrame> ring;
    size_t maxFrames = 0;
    size_t ringHead = 0;
    size_t ringCount = 0;

    std::string lastCodec = "";

    int frameSkip = 0;
    int currentSkip = 0;

    void init() {
        if (active) return;

        targetFps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
        clipLenSec = (int)Mod::get()->getSettingValue<int64_t>("clip-length");
        maxFrames = (size_t)(targetFps * clipLenSec);
        frameIdx = 0;

        if (targetFps >= 60) {
            frameSkip = 1;
        } else {
            frameSkip = 0;
        }

        {
            std::lock_guard<std::mutex> lk(ringMtx);
            ring.resize(maxFrames);
            ringHead = 0;
            ringCount = 0;
        }

        active = true;

        bool micOk = mic.start(clipLenSec);
        bool gdOk = gd.start(clipLenSec);

        std::string micName = micOk ? mic.getDeviceName() : "no mic";
        notif("echoclip ready | mic: " + micName);
    }

    void shutdown() {
        if (!active) return;
        active = false;
        mic.stop();
        gd.stop();
    }

    void markAtt() {
        if (!active) return;

        AttMark m;
        m.frameIdx = frameIdx.load();

        if (auto pl = PlayLayer::get()) {
            if (auto lvl = pl->m_level) {
                m.lvlName = lvl->m_levelName;
            }
            m.att = pl->m_attempts;
        }

        std::lock_guard<std::mutex> lk(markMtx);
        marks.push_front(m);

        while (marks.size() > 256) {
            marks.pop_back();
        }
    }

    void checkBest(int pct) {
        if (!active || !Mod::get()->getSettingValue<bool>("clip-on-new-best")) return;

        bool nb = false;
        {
            std::lock_guard<std::mutex> lk(bestMtx);
            if (pct > bestPct) {
                bestPct = pct;
                nb = true;
            }
        }

        if (nb && pct > 0) {
            clip();
        }
    }

    void clip() {
        if (!active) {
            notif("echoclip not active", true);
            return;
        }
        if (exporting.exchange(true)) {
            notif("already exporting", true);
            return;
        }

        AttMark mark;
        int64_t attStart = 0;

        {
            std::lock_guard<std::mutex> lk(markMtx);
            if (!marks.empty()) {
                mark = marks.front();
                size_t lookback = std::min(marks.size(), size_t(5));
                attStart = marks[lookback - 1].frameIdx;
            }
        }

        std::deque<RawFrame> frames;
        {
            std::lock_guard<std::mutex> lk(ringMtx);
            size_t startIdx = (ringHead + maxFrames - ringCount) % maxFrames;
            for (size_t i = 0; i < ringCount; i++) {
                size_t idx = (startIdx + i) % maxFrames;
                if (ring[idx].idx >= attStart) {
                    frames.push_back(std::move(ring[idx]));
                }
            }
            ringCount = 0;
            ringHead = 0;
        }

        std::vector<float> ma = mic.takeAll();
        std::vector<float> ga = gd.takeAll();

        if (frames.empty()) {
            notif("no frames to clip", true);
            exporting = false;
            return;
        }

        std::thread([this, frames = std::move(frames), ma = std::move(ma), ga = std::move(ga), mark]() mutable {
            doExport(std::move(frames), std::move(ma), std::move(ga), mark);
            exporting = false;
        }).detach();
    }

private:
    std::string pickVideoCodec() {
        ffmpeg::Recorder testRec;
        auto codecs = testRec.getAvailableCodecs();

        std::vector<std::string> preferred = {
            "h264_nvenc",
            "h264_amf",
            "h264_qsv",
            "libx264",
        };

        for (auto& pref : preferred) {
            for (auto& c : codecs) {
                if (c == pref) {
                    lastCodec = pref;
                    return pref;
                }
            }
        }

        if (!codecs.empty()) {
            lastCodec = codecs[0];
            return codecs[0];
        }

        lastCodec = "libx264";
        return "libx264";
    }

    void doExport(std::deque<RawFrame> frames, std::vector<float> ma, std::vector<float> ga, AttMark mark) {
        notif("creating clip...");

        auto dir = Mod::get()->getSaveDir() / "clips";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::string base = safename(mark.lvlName) + "_att" + std::to_string(mark.att);
        std::string stem = makeUniqueStem(base);

        std::filesystem::path vidpath = dir / (stem + "_tmp.mp4");
        std::filesystem::path outpath = dir / (stem + ".mkv");

        if (frames.empty()) {
            notif("frame error", true);
            return;
        }

        int fw = frames.front().w & ~1;
        int fh = frames.front().h & ~1;

        if (fw <= 0 || fh <= 0) {
            notif("invalid frame size", true);
            return;
        }

        int fps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
        int crf = (int)Mod::get()->getSettingValue<int64_t>("crf-quality");

        std::string codec = pickVideoCodec();
        int bitrate = (int)(50000000.0 * std::pow(0.75, std::max(0, crf - 18)));

        {
            ffmpeg::Recorder rec;
            ffmpeg::RenderSettings rs;
            rs.m_width = (uint32_t)fw;
            rs.m_height = (uint32_t)fh;
            rs.m_fps = (uint16_t)fps;
            rs.m_codec = codec;
            rs.m_bitrate = (uint32_t)bitrate;
            rs.m_outputFile = vidpath.string();
            rs.m_pixelFormat = ffmpeg::PixelFormat::RGB0;

            auto r = rec.init(rs);
            if (r.isErr()) {
                rs.m_codec = "libx264";
                lastCodec = "libx264";
                r = rec.init(rs);
                if (r.isErr()) {
                    ffmpeg::Recorder listRec;
                    auto codecs = listRec.getAvailableCodecs();
                    std::string clist;
                    for (auto& c : codecs) clist += c + " ";
                    notif("all codecs failed. available: " + clist, true);
                    return;
                }
            }

            for (auto& f : frames) {
                if (f.px.empty() || f.w != fw || f.h != fh) continue;
                (void)rec.writeFrame(f.px);
            }

            rec.stop();
        }

        frames.clear();

        if (!std::filesystem::exists(vidpath, ec)) {
            notif("encode error - video file missing", true);
            return;
        }

        auto vidSize = std::filesystem::file_size(vidpath, ec);
        if (vidSize < 512) {
            notif("encode failed - file too small", true);
            std::filesystem::remove(vidpath, ec);
            return;
        }

        size_t alen = std::max(ma.size(), ga.size());

        if (alen == 0) {
            std::filesystem::rename(vidpath, outpath, ec);
            notif("clip saved (no audio)");
            return;
        }

        float mvol = (float)Mod::get()->getSettingValue<double>("mic-audio-volume") / 100.f;
        float gvol = (float)Mod::get()->getSettingValue<double>("game-audio-volume") / 100.f;

        std::vector<float> mixed(alen, 0.f);

        for (size_t i = 0; i < ma.size(); i++) {
            mixed[i] += ma[i] * mvol;
        }
        for (size_t i = 0; i < ga.size(); i++) {
            mixed[i] += ga[i] * gvol;
        }

        for (auto& s : mixed) {
            s = std::clamp(s, -1.f, 1.f);
        }

        ma.clear();
        ga.clear();

        auto mr = ffmpeg::AudioMixer::mixVideoRaw(vidpath, mixed, outpath);

        std::filesystem::remove(vidpath, ec);

        if (std::filesystem::exists(outpath)) {
            notif("clip saved!");
        } else {
            notif("audio mix failed", true);
        }
    }
};

static Engine g_eng;

class $modify(EchoBGL, GJBaseGameLayer) {
    struct Fields {
        int lastPct = 0;
        float frameTimer = 0.f;
        int capW = 0;
        int capH = 0;
        float frameAccum = 0.f;
        int frameCount = 0;
    };

    void update(float dt) override {
        GJBaseGameLayer::update(dt);

        if (!g_eng.active || !Mod::get()->getSettingValue<bool>("enabled")) {
            return;
        }
        if (m_gameState.m_currentProgress <= 0) {
            return;
        }

        int pct = (int)m_gameState.m_currentProgress;
        if (pct != m_fields->lastPct) {
            m_fields->lastPct = pct;
            g_eng.checkBest(pct);
        }

        int fps = (int)Mod::get()->getSettingValue<int64_t>("target-fps");
        float frameDt = 1.f / (float)fps;

        m_fields->frameAccum += dt;

        if (m_fields->frameAccum >= frameDt * 3.f) {
            m_fields->frameAccum = frameDt;
        }

        if (m_fields->frameAccum < frameDt) {
            return;
        }

        m_fields->frameAccum -= frameDt;
        m_fields->frameCount++;

        if (g_eng.frameSkip > 0 && (m_fields->frameCount % (g_eng.frameSkip + 1)) != 0) {
            return;
        }

        grabFrameAsync();
    }

    void grabFrameAsync() {
        GLint vp[4] = {};
        glGetIntegerv(GL_VIEWPORT, vp);

        int w = vp[2] & ~1;
        int h = vp[3] & ~1;

        if (w <= 0 || h <= 0) {
            return;
        }

        size_t sz = (size_t)(w * h * 4);

        if (m_fields->capW != w || m_fields->capH != h) {
            m_fields->capW = w;
            m_fields->capH = h;

            std::lock_guard<std::mutex> lk(g_eng.ringMtx);
            for (auto& f : g_eng.ring) f.px.clear();
            g_eng.ringCount = 0;
        }

        std::lock_guard<std::mutex> lk(g_eng.ringMtx);
        if (g_eng.ring.empty()) return;

        auto& fr = g_eng.ring[g_eng.ringHead];
        if (fr.px.size() != sz) {
            fr.px.resize(sz);
        }

        glReadPixels(vp[0], vp[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, fr.px.data());
        fr.w = w;
        fr.h = h;
        fr.idx = g_eng.frameIdx.fetch_add(1);
        g_eng.ringHead = (g_eng.ringHead + 1) % g_eng.maxFrames;
        if (g_eng.ringCount < g_eng.maxFrames) g_eng.ringCount++;
    }
};

class $modify(EchoPL, PlayLayer) {
    bool init(GJGameLevel* lvl, bool ur, bool doc) {
        if (!PlayLayer::init(lvl, ur, doc)) {
            return false;
        }

        std::lock_guard<std::mutex> lk(g_eng.bestMtx);
        g_eng.bestPct = 0;

        return true;
    }

    void resetLevel() {
        if (g_eng.active) {
            g_eng.markAtt();
        }
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        PlayLayer::levelComplete();

        if (g_eng.active && Mod::get()->getSettingValue<bool>("clip-on-beat")) {
            g_eng.clip();
        }
    }
};

$on_game(Loaded) {
    g_eng.init();

    listenForKeybindSettingPresses("clip-keybind", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            g_eng.clip();
        }
    });
}