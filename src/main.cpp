// written by someone who regrets everything

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
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
static constexpr int FRAME_WIDTH = 1280;
static constexpr int FRAME_HEIGHT = 720;
static constexpr int TARGET_FPS = 30;
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int ATTEMPT_WINDOW = 5;

struct AttemptMarker {
    int64_t frameNumber;
    std::string levelName;
    int attemptIndex;
};

void showNotification(std::string message, bool error = false) {
    auto icon = error ? NotificationIcon::Error : NotificationIcon::Success;
    Notification::create(message, icon)->show();
}

class EchoClipEngine {
public:
    std::atomic<bool> isRecording{false};
    std::atomic<bool> shouldStop{false};
    
    std::mutex attemptMutex;
    std::deque<AttemptMarker> attemptMarkers;
    
    int64_t videoFrameCounter = 0;
    int attemptCounter = 0;
    
    std::unique_ptr<ScreenCapture> videoCapture;
    std::unique_ptr<FMODMicrophoneCapture> micCapture;
    std::unique_ptr<FFmpegEncoder> encoder;
    
    std::thread captureThread;
    std::thread audioMixThread;
    
    EchoClipEngine() = default;
    
    ~EchoClipEngine() {
        shutdown();
    }
    
    void shutdown() {
        if (!isRecording) return;

        shouldStop = true;
        
        if (captureThread.joinable()) captureThread.join();
        if (audioMixThread.joinable()) audioMixThread.join();
        
        if (videoCapture) videoCapture->stop();
        if (micCapture) micCapture->stop();
        if (encoder) encoder->stop();
        
        isRecording = false;
    }
    
    bool initialize() {
        if (isRecording) return true;

        std::filesystem::create_directories(OUTPUT_DIR);
        
        videoCapture = std::make_unique<ScreenCapture>(TARGET_FPS, FRAME_WIDTH, FRAME_HEIGHT);
        if (!videoCapture->start()) {
            log::error("echoclip: failed to start video capture");
            return false;
        }
        
        micCapture = std::make_unique<FMODMicrophoneCapture>();
        auto fmodSystem = FMODAudioEngine::get()->m_system;
        if (!micCapture->initialize(fmodSystem)) {
            log::warn("echoclip: microphone initialization failed");
        } else if (!micCapture->start()) {
            log::warn("echoclip: microphone capture failed");
        }
        
        encoder = std::make_unique<FFmpegEncoder>(TEMP_OUTPUT_PATH, FRAME_WIDTH, FRAME_HEIGHT, TARGET_FPS);
        
        auto bitrate = Mod::get()->getSavedValue<double>("bitrate", 5.0);
        encoder->setBitrate(static_cast<float>(bitrate));
        
        auto useHWEnc = Mod::get()->getSavedValue<bool>("use-hardware-encoding", true);
        encoder->setHardwareEncoding(useHWEnc);

        if (!encoder->initialize()) {
            log::error("echoclip: failed to initialize ffmpeg encoder install ffmpeg maybe");
            return false;
        }
        
        isRecording = true;
        shouldStop = false;
        videoFrameCounter = 0;
        attemptCounter = 0;
        
        captureThread = std::thread([this] { captureLoop(); });
        audioMixThread = std::thread([this] { audioMixLoop(); });
        
        log::info("echoclip: initialized suffering has begun");
        return true;
    }

private:
    void captureLoop() {
        int framesSinceLastLog = 0;
        
        while (!shouldStop) {
            VideoFrame frame;
            if (videoCapture && videoCapture->getFrame(frame)) {
                videoFrameCounter = frame.frameNumber + 1;
                
                if (encoder) {
                    encoder->submitVideoFrame(frame.data.data(), frame.width, frame.height, frame.pitch);
                }
                
                framesSinceLastLog++;
                if (framesSinceLastLog >= 300) {
                    log::debug("echoclip: {} frames captured", videoFrameCounter);
                    framesSinceLastLog = 0;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    
    void audioMixLoop() {
        float micVolume = Mod::get()->getSavedValue<double>("mic-audio-volume", 30.0) / 100.0f;
        int audioLogCounter = 0;
        
        while (!shouldStop) {
            AudioFrame micFrame;
            
            if (micCapture && micCapture->getAudio(micFrame)) {
                std::vector<float> adjusted(micFrame.samples.size());
                for (size_t i = 0; i < micFrame.samples.size(); ++i) {
                    adjusted[i] = micFrame.samples[i] * micVolume;
                    if (adjusted[i] > 1.0f) adjusted[i] = 1.0f;
                    if (adjusted[i] < -1.0f) adjusted[i] = -1.0f;
                }
                
                if (encoder) {
                    encoder->submitAudioFrame(adjusted.data(), micFrame.sampleCount);
                }
                
                audioLogCounter++;
                if (audioLogCounter >= 100) {
                    log::debug("echoclip: audio processing");
                    audioLogCounter = 0;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

public:
    void recordAttempt() {
        if (!isRecording) return;

        std::lock_guard<std::mutex> lock(attemptMutex);
        
        std::string levelName = "unknown";
        if (auto pm = PlayLayer::get()) {
            if (auto level = pm->m_level) {
                levelName = level->m_levelName;
            }
        }
        
        attemptCounter++;
        
        AttemptMarker marker;
        marker.frameNumber = videoFrameCounter;
        marker.levelName = levelName;
        marker.attemptIndex = attemptCounter;
        
        attemptMarkers.push_front(marker);
        
        if (attemptMarkers.size() > ATTEMPT_WINDOW) {
            attemptMarkers.pop_back();
        }
        
        log::info("echoclip: attempt #{} recorded at frame {}", attemptCounter, videoFrameCounter);
    }
    
    bool exportClip(const std::string& outputPath) {
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        if (attemptMarkers.empty()) {
            log::warn("echoclip: no attempts recorded");
            showNotification("no attempts to clip", true);
            return false;
        }
        
        // ensure output directory exists
        std::error_code ec;
        std::filesystem::create_directories(OUTPUT_DIR, ec);
        
        // wait for temp file to exist and have data
        int waitCount = 0;
        while (!std::filesystem::exists(TEMP_OUTPUT_PATH) && waitCount < 100) {
            log::debug("echoclip: waiting for temp file... {}/100", waitCount);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            waitCount++;
        }
        
        // if temp file still doesn't exist, it might have been deleted
        // in that case, we can't recover - encoder needs to create it
        if (!std::filesystem::exists(TEMP_OUTPUT_PATH)) {
            log::error("echoclip: temp file missing - encoder may have crashed");
            showNotification("recording file corrupted, restart game", true);
            return false;
        }
        
        auto fileSize = std::filesystem::file_size(TEMP_OUTPUT_PATH);
        if (fileSize < 500000) {
            log::warn("echoclip: temp file too small ({} bytes)", fileSize);
            showNotification("recording not ready yet", true);
            return false;
        }
        
        log::info("echoclip: temp file size: {} MB", fileSize / 1000000.0);
        
        const AttemptMarker& newestAttempt = attemptMarkers.front();
        const AttemptMarker& oldestAttempt = attemptMarkers.back();
        
        // frame to seconds: frame / fps
        double msPerFrame = 1000.0 / static_cast<double>(TARGET_FPS);
        double startTimeMs = oldestAttempt.frameNumber * msPerFrame;
        double endTimeMs = newestAttempt.frameNumber * msPerFrame;
        
        double startTimeSec = startTimeMs / 1000.0;
        double durationSec = (endTimeMs - startTimeMs) / 1000.0;
        
        if (durationSec < 0.5) {
            durationSec = 0.5;
        }
        
        log::info("echoclip: extracting clip {:.2f}s to {:.2f}s duration {:.2f}s",
            startTimeSec, (endTimeMs / 1000.0), durationSec);
        
        std::ostringstream cmdStream;
        cmdStream << std::fixed << std::setprecision(2);
        cmdStream << "ffmpeg -y "
                  << "-ss " << startTimeSec << " "
                  << "-i \"" << TEMP_OUTPUT_PATH << "\" "
                  << "-t " << durationSec << " "
                  << "-c:v copy -c:a copy "
                  << "\"" << outputPath << "\" "
                  << "2>echoclip_ffmpeg_error.txt";
        
        std::string command = cmdStream.str();
        log::info("echoclip: extracting with codec copy");
        int result = std::system(command.c_str());
        
        if (result == 0) {
            log::info("echoclip: saved to {}", outputPath);
            return true;
        } else {
            log::warn("echoclip: codec copy failed, trying with re-encode");
            
            // fallback: re-encode
            std::ostringstream fallbackStream;
            fallbackStream << std::fixed << std::setprecision(2);
            fallbackStream << "ffmpeg -y "
                          << "-ss " << startTimeSec << " "
                          << "-i \"" << TEMP_OUTPUT_PATH << "\" "
                          << "-t " << durationSec << " "
                          << "-c:v libx264 -preset ultrafast -b:v 5M "
                          << "-c:a aac -b:a 128k "
                          << "\"" << outputPath << "\" "
                          << "2>echoclip_ffmpeg_error.txt";
            
            std::string fallbackCmd = fallbackStream.str();
            log::info("echoclip: re-encode command: {}", fallbackCmd);
            result = std::system(fallbackCmd.c_str());
            
            if (result == 0) {
                log::info("echoclip: re-encode success, saved to {}", outputPath);
                return true;
            } else {
                log::error("echoclip: re-encode also failed with code {}", result);
                
                // read error file
                std::ifstream errFile("echoclip_ffmpeg_error.txt");
                if (errFile.is_open()) {
                    std::stringstream buffer;
                    buffer << errFile.rdbuf();
                    log::error("echoclip: ffmpeg stderr: {}", buffer.str());
                    errFile.close();
                }
                return false;
            }
        }
    }
    
    std::string generateClipFilename() {
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        if (attemptMarkers.empty()) {
            return std::string(OUTPUT_DIR) + "/clip_error.mp4";
        }
        
        std::string levelName = attemptMarkers.front().levelName;
        int attemptCount = attemptMarkers.size();
        
        std::string cleanName;
        for (char c : levelName) {
            if (std::isalnum(c) || c == '_' || c == '-') {
                cleanName += c;
            } else if (std::isspace(c)) {
                cleanName += '_';
            }
        }
        
        if (cleanName.empty()) {
            cleanName = "unknown";
        }
        
        std::ostringstream filename;
        filename << OUTPUT_DIR << "/clip_" << cleanName << "_" << attemptCount << "att.mp4";
        return filename.str();
    }
};

static std::unique_ptr<EchoClipEngine> g_engine;

class $modify(AutoStartMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        
        if (g_engine && !g_engine->isRecording) {
            if (g_engine->initialize()) {
                showNotification("echoclip: recording started");
            } else {
                showNotification("echoclip: failed to start", true);
            }
        }
        return true;
    }
};

class $modify(PlayLayerEchoClip, PlayLayer) {
    void resetLevel() {
        if (g_engine && g_engine->isRecording) {
            g_engine->recordAttempt();
        }
        PlayLayer::resetLevel();
    }
    
    void levelComplete() {
        if (g_engine && g_engine->isRecording) {
            g_engine->recordAttempt();
        }
        PlayLayer::levelComplete();
    }
};

$on_mod(Loaded) {
    log::info("echoclip: mod loading");
    
    std::error_code ec;
    if (!std::filesystem::exists(OUTPUT_DIR)) {
        std::filesystem::create_directories(OUTPUT_DIR, ec);
    }
    
    g_engine = std::make_unique<EchoClipEngine>();

    BindManager::get()->registerBindable({
        "echoclip-save"_spr,
        "Save Clip",
        "Saves the last 5 attempts to disk",
        { Keybind::create(KEY_F6, Modifier::None) },
        "EchoClip"
    }); 
    
    new EventListener<InvokeBindFilter>(+[](InvokeBindEvent* event) {
        if (event->isDown()) {
            if (!g_engine || !g_engine->isRecording) {
                showNotification("engine is off", true);
                return ListenerResult::Propagate;
            }

            showNotification("clipping...");
            
            std::string outputPath = g_engine->generateClipFilename();
            
            std::thread clipThread([outputPath]() {
                if (g_engine && g_engine->exportClip(outputPath)) {
                    showNotification("clipped");
                } else {
                    showNotification("save failed", true);
                }
            });
            clipThread.detach();
        }
        return ListenerResult::Propagate;
    }, InvokeBindFilter(nullptr, "echoclip-save"_spr));
}