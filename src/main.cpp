// written by someone who regrets everything

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <geode.custom-keybinds/include/Keybinds.hpp>

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <fstream>

#include "audio.hpp"
#include "video_capture.hpp"
#include "ffmpeg_encoder.hpp"

using namespace geode::prelude;
using namespace keybinds;

static constexpr const char* TEMP_OUTPUT_PATH = "echoclip_recordings/echoclip_temp.mp4";
static const std::string OUTPUT_DIR = "echoclip_recordings";
static constexpr int DEFAULT_FRAME_WIDTH = 854;
static constexpr int DEFAULT_FRAME_HEIGHT = 480;
static constexpr int DEFAULT_TARGET_FPS = 30;
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int DEFAULT_ATTEMPT_WINDOW = 5;

struct AttemptMarker {
    int64_t frameNumber;
    std::string levelName;
    int attemptIndex;
    int gdAttemptCount = 0; // store actual gd attempt number
};

void showNotification(std::string message, bool error = false) {
    auto icon = error ? NotificationIcon::Error : NotificationIcon::Success;
    Notification::create(message, icon)->show();
}

int getSettingInt(const char* key, int defaultVal) {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
}

bool getSettingBool(const char* key, bool defaultVal) {
    return Mod::get()->getSettingValue<bool>(key);
}

std::string getSettingString(const char* key, const std::string& defaultVal) {
    return Mod::get()->getSettingValue<std::string>(key);
}

void parseResolution(const std::string& res, int& w, int& h) {
    size_t x = res.find('x');
    if (x != std::string::npos) {
        w = std::stoi(res.substr(0, x));
        h = std::stoi(res.substr(x + 1));
    }
}

class EchoClipEngine {
public:
    std::atomic<bool> isRecording{false};
    std::atomic<bool> shouldStop{false};
    
    std::unique_ptr<ScreenCapture> videoCapture;
    std::unique_ptr<FMODMicrophoneCapture> micCapture;
    std::unique_ptr<FFmpegEncoder> encoder;
    
    std::thread audioMixThread;
    
    int64_t videoFrameCounter = 0;
    int attemptCounter = 0;
    int frameWidth = DEFAULT_FRAME_WIDTH;
    int frameHeight = DEFAULT_FRAME_HEIGHT;
    int targetFPS = DEFAULT_TARGET_FPS;
    int attemptWindow = DEFAULT_ATTEMPT_WINDOW;
    
    std::mutex attemptMutex;
    std::deque<AttemptMarker> attemptMarkers;
    
    EchoClipEngine() = default;
    
    ~EchoClipEngine() {
        shutdown();
    }
    
    void loadSettings() {
        std::string res = getSettingString("capture-resolution", "854x480");
        parseResolution(res, frameWidth, frameHeight);
        targetFPS = getSettingInt("target-fps", 30);
        attemptWindow = getSettingInt("clip-length", 5);
    }
    
    void shutdown() {
        if (!isRecording) return;
        shouldStop = true;
        if (audioMixThread.joinable()) audioMixThread.join();
        if (videoCapture) videoCapture->stop();
        if (micCapture) micCapture->stop();
        if (encoder) encoder->stop();
        isRecording = false;
    }
    
    bool initialize() {
        if (isRecording) return true;
        if (!getSettingBool("enabled", true)) return false;

        loadSettings();
        std::filesystem::create_directories(OUTPUT_DIR);
        
        videoCapture = std::make_unique<ScreenCapture>(targetFPS, frameWidth, frameHeight);
        if (!videoCapture->start()) return false;
        
        micCapture = std::make_unique<FMODMicrophoneCapture>();
        auto fmodSystem = FMODAudioEngine::get()->m_system;
        micCapture->initialize(fmodSystem);
        micCapture->start();
        
        encoder = std::make_unique<FFmpegEncoder>(TEMP_OUTPUT_PATH, frameWidth, frameHeight, targetFPS);
        if (!encoder->initialize()) return false;
        
        isRecording = true;
        shouldStop = false;
        videoFrameCounter = 0;
        attemptCounter = 0;
        
        audioMixThread = std::thread([this] { audioMixLoop(); });
        
        Notification::create("echoclip: recording started", NotificationIcon::Success)->show();
        
        return true;
    }

private:
    void audioMixLoop() {
        float micVolume = static_cast<float>(Mod::get()->getSettingValue<double>("mic-audio-volume") / 100.0f);
        while (!shouldStop) {
            VideoFrame vframe;
            while (videoCapture && videoCapture->getFrame(vframe)) {
                videoFrameCounter = vframe.frameNumber + 1;
                if (encoder) encoder->submitVideoFrame(vframe.data.data(), vframe.width, vframe.height);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

public:
    void recordAttempt() {
        if (!isRecording) return;
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        std::string levelName = "unknown";
        int gdAttempts = 0;
        
        if (auto pm = PlayLayer::get()) {
            if (auto level = pm->m_level) {
                levelName = level->m_levelName;
            }
            // get actual attempt count from gd
            gdAttempts = pm->m_attempts;
        }
        
        attemptCounter++;
        AttemptMarker marker;
        marker.frameNumber = videoFrameCounter;
        marker.levelName = levelName;
        marker.attemptIndex = attemptCounter;
        marker.gdAttemptCount = gdAttempts;
        
        attemptMarkers.push_front(marker);
        while (attemptMarkers.size() > static_cast<size_t>(attemptWindow)) attemptMarkers.pop_back();
        
        log::info("echoclip: attempt #{} (gd attempt #{})", attemptCounter, gdAttempts);
    }
    
    bool exportClip(const std::string& outputPath) {
        std::lock_guard<std::mutex> lock(attemptMutex);
        if (attemptMarkers.empty()) {
            showNotification("no attempts to clip", true);
            return false;
        }
        
        // don't stop encoder, just extract from current file
        // flush encoder to ensure all frames are written
        if (encoder) {
            encoder->flush();
        }
        
        // wait a bit for ffmpeg to write frames
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        if (!std::filesystem::exists(TEMP_OUTPUT_PATH)) {
            log::error("echoclip: temp file missing");
            return false;
        }
        
        const AttemptMarker& newest = attemptMarkers.front();
        const AttemptMarker& oldest = attemptMarkers.back();
        
        // calculate time based on frame numbers and fps
        double frameDuration = 1.0 / targetFPS;
        double startTime = oldest.frameNumber * frameDuration;
        double duration = (newest.frameNumber - oldest.frameNumber) * frameDuration;
        
        // ensure minimum clip length
        if (duration < 2.0) duration = 2.0;
        
        // add small buffer for encoding delay
        if (startTime > 0.5) startTime -= 0.5;
        duration += 0.5;
        
        log::info("echoclip: extracting from {:.2f}s for {:.2f}s (frames {} to {})", 
            startTime, duration, oldest.frameNumber, newest.frameNumber);
        
        // use ffmpeg to extract segment - hidden window
        std::ostringstream cmd;
        cmd << "ffmpeg -y -ss " << std::fixed << std::setprecision(3) << startTime
            << " -t " << duration
            << " -i \"" << TEMP_OUTPUT_PATH << "\""
            << " -c:v copy -c:a copy -avoid_negative_ts make_zero "
            << "-fflags +genpts \"" << outputPath << "\"";
        
        int result = FFmpegEncoder::runSilent(cmd.str());
        
        return result == 0;
    }

    std::string generateClipFilename() {
        std::lock_guard<std::mutex> lock(attemptMutex);
        if (attemptMarkers.empty()) return std::string(OUTPUT_DIR) + "/clip_error.mp4";
        
        std::string levelName = attemptMarkers.front().levelName;
        int gdAttempt = attemptMarkers.front().gdAttemptCount;
        
        std::string cleanName;
        for (char c : levelName) {
            if (std::isalnum(c)) cleanName += c;
            else if (std::isspace(c)) cleanName += '_';
        }
        
        std::ostringstream filename;
        filename << OUTPUT_DIR << "/clip_" << (cleanName.empty() ? "unknown" : cleanName) 
                 << "_att" << gdAttempt << ".mp4";
        return filename.str();
    }
};

static std::unique_ptr<EchoClipEngine> g_engine;

class $modify(AutoStartMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
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

$on_mod(Loaded) {
    std::filesystem::create_directories(OUTPUT_DIR);
    g_engine = std::make_unique<EchoClipEngine>();

    BindManager::get()->registerBindable({
        "echoclip-save"_spr, "Save Clip", "save video",
        { Keybind::create(KEY_F6, Modifier::None) }, "EchoClip"
    }); 
    
    new EventListener<InvokeBindFilter>(+[](InvokeBindEvent* event) {
        if (event->isDown()) {
            if (!g_engine || !g_engine->isRecording) {
                showNotification("engine is off", true);
                return ListenerResult::Propagate;
            }
            showNotification("clipping...");
            std::string path = g_engine->generateClipFilename();
            std::thread([path]() {
                if (g_engine->exportClip(path)) showNotification("clipped");
                else showNotification("save failed", true);
            }).detach();
        }
        return ListenerResult::Propagate;
    }, InvokeBindFilter(nullptr, "echoclip-save"_spr));
}