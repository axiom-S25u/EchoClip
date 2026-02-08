#pragma once

#include <Geode/Geode.hpp>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <iomanip>

using namespace geode::prelude;

struct PendingFrame {
    std::vector<uint8_t> data;
    size_t width = 0;
    size_t height = 0;
    size_t pitch = 0;
};

struct PendingAudio {
    std::vector<float> samples;
    size_t count = 0;
};

class FFmpegEncoder {
private:
    std::string outputPath;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int sampleRate = 48000;
    int channels = 2;
    float bitrate = 5.0f;
    bool useHWEncoding = true;
    
    std::atomic<bool> isEncoding{false};
    std::atomic<bool> shouldStop{false};
    std::thread encodingThread;
    
    std::queue<PendingFrame> videoQueue;
    std::queue<PendingAudio> audioQueue;
    mutable std::mutex videoMutex;
    mutable std::mutex audioMutex;
    
    std::string videoTempFile;
    std::string audioTempFile;
    
    uint64_t totalAudioSamples = 0;

public:
    FFmpegEncoder(const std::string& output, int w = 1280, int h = 720, int f = 30)
        : outputPath(output), width(w), height(h), fps(f) {
        videoTempFile = "echoclip_video_temp.rgb";
        audioTempFile = "echoclip_audio_temp.wav";
    }
    
    ~FFmpegEncoder() {
        stop();
    }
    
    void setHardwareEncoding(bool use) {
        useHWEncoding = use;
    }
    
    void setBitrate(float mbps) {
        bitrate = mbps;
    }
    
    bool initialize() {
        log::info("ffmpeg: initializing");
        
        int result = std::system("ffmpeg -version >nul 2>&1");
        if (result != 0) {
            log::error("ffmpeg: not found install it");
            return false;
        }
        
        isEncoding = true;
        shouldStop = false;
        totalAudioSamples = 0;
        encodingThread = std::thread([this] { encodingLoop(); });
        
        log::info("ffmpeg: thread started");
        return true;
    }
    
    void stop() {
        if (!isEncoding) return;
        
        shouldStop = true;
        
        if (encodingThread.joinable()) {
            encodingThread.join();
        }
        
        isEncoding = false;
    }
    
    bool submitVideoFrame(const uint8_t* rgbData, size_t w, size_t h, size_t pitch = 0) {
        if (!isEncoding || !rgbData) return false;
        
        PendingFrame frame;
        frame.width = w;
        frame.height = h;
        frame.pitch = pitch > 0 ? pitch : w * 3;
        
        size_t dataSize = frame.height * frame.pitch;
        frame.data.assign(rgbData, rgbData + dataSize);
        
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            videoQueue.push(frame);
        }
        
        return true;
    }
    
    bool submitAudioFrame(const float* samples, size_t count) {
        if (!isEncoding || !samples || count == 0) return false;
        
        PendingAudio audio;
        audio.count = count;
        audio.samples.assign(samples, samples + (count * channels));
        
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            audioQueue.push(audio);
        }
        
        return true;
    }
    
    size_t getVideoQueueSize() const {
        std::lock_guard<std::mutex> lock(videoMutex);
        return videoQueue.size();
    }
    
    size_t getAudioQueueSize() const {
        std::lock_guard<std::mutex> lock(audioMutex);
        return audioQueue.size();
    }

private:
    void encodingLoop() {
        log::info("ffmpeg: encoding loop started");
        
        // ensure temp directory exists
        std::error_code ec;
        std::filesystem::create_directories("echoclip_recordings", ec);
        
        FILE* videoFP = nullptr;
        FILE* audioFP = nullptr;
        
        do {
            videoFP = fopen(videoTempFile.c_str(), "wb");
            if (!videoFP) {
                log::error("ffmpeg: failed to open video temp");
                break;
            }
            log::info("ffmpeg: video temp file created");
            
            audioFP = fopen(audioTempFile.c_str(), "wb");
            if (!audioFP) {
                log::error("ffmpeg: failed to open audio temp");
                break;
            }
            log::info("ffmpeg: audio temp file created");
            
            writeWavHeader(audioFP, sampleRate, channels, 0);
            
            while (!shouldStop) {
                bool hasData = false;
                
                // check if files still exist (in case they were deleted)
                if (!videoFP || !audioFP) {
                    log::warn("ffmpeg: file handle lost, stopping");
                    break;
                }
                
                {
                    std::lock_guard<std::mutex> lock(videoMutex);
                    while (!videoQueue.empty()) {
                        PendingFrame frame = videoQueue.front();
                        videoQueue.pop();
                        
                        if (videoFP && !frame.data.empty()) {
                            size_t written = fwrite(frame.data.data(), 1, frame.data.size(), videoFP);
                            if (written != frame.data.size()) {
                                log::error("ffmpeg: video write failed");
                                videoFP = nullptr;
                                break;
                            }
                            fflush(videoFP);
                            hasData = true;
                        }
                    }
                }
                
                {
                    std::lock_guard<std::mutex> lock(audioMutex);
                    while (!audioQueue.empty()) {
                        PendingAudio audio = audioQueue.front();
                        audioQueue.pop();
                        
                        if (audioFP && !audio.samples.empty()) {
                            size_t written = fwrite(audio.samples.data(), sizeof(float), audio.samples.size(), audioFP);
                            if (written != audio.samples.size()) {
                                log::error("ffmpeg: audio write failed");
                                audioFP = nullptr;
                                break;
                            }
                            fflush(audioFP);
                            totalAudioSamples += audio.count;
                            hasData = true;
                        }
                    }
                }
                
                if (!hasData) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            
            if (videoFP) {
                fflush(videoFP);
                fclose(videoFP);
                videoFP = nullptr;
            }
            
            if (audioFP) {
                fseek(audioFP, 0, SEEK_SET);
                writeWavHeader(audioFP, sampleRate, channels, totalAudioSamples);
                fflush(audioFP);
                fclose(audioFP);
                audioFP = nullptr;
            }
            
            log::info("ffmpeg: running ffmpeg to encode");
            encodeWithFFmpeg();
            
        } while (false);
        
        if (videoFP) fclose(videoFP);
        if (audioFP) fclose(audioFP);
        
        try {
            std::remove(videoTempFile.c_str());
            std::remove(audioTempFile.c_str());
        } catch (...) {
        }
        
        log::info("ffmpeg: encoding loop finished");
    }
    
    void writeWavHeader(FILE* fp, int sr, int ch, uint64_t sampleCount) {
        if (!fp) return;
        
        uint32_t dataSize = static_cast<uint32_t>(sampleCount * ch * sizeof(float));
        uint32_t fileSize = 36 + dataSize;
        
        fwrite("RIFF", 1, 4, fp);
        fwrite(&fileSize, 4, 1, fp);
        fwrite("WAVE", 1, 4, fp);
        
        fwrite("fmt ", 1, 4, fp);
        uint32_t subchunk1Size = 16;
        fwrite(&subchunk1Size, 4, 1, fp);
        
        uint16_t audioFormat = 3;
        fwrite(&audioFormat, 2, 1, fp);
        
        uint16_t numChannels = ch;
        fwrite(&numChannels, 2, 1, fp);
        
        uint32_t sampleRate = sr;
        fwrite(&sampleRate, 4, 1, fp);
        
        uint32_t byteRate = sr * ch * sizeof(float);
        fwrite(&byteRate, 4, 1, fp);
        
        uint16_t blockAlign = ch * sizeof(float);
        fwrite(&blockAlign, 2, 1, fp);
        
        uint16_t bitsPerSample = 32;
        fwrite(&bitsPerSample, 2, 1, fp);
        
        fwrite("data", 1, 4, fp);
        fwrite(&dataSize, 4, 1, fp);
    }
    
    void encodeWithFFmpeg() {
        std::string encoder = "libx264";
        std::string preset = "ultrafast";
        
        if (useHWEncoding) {
            encoder = "hevc_nvenc";
            preset = "default";
        }
        
        std::ostringstream cmdStream;
        cmdStream << "ffmpeg -y "
            << "-f rawvideo -pixel_format rgb24 -s " << width << "x" << height
            << " -r " << fps << " -i \"" << videoTempFile << "\" "
            << "-f f32le -ar " << sampleRate << " -ac " << channels << " -i \"" << audioTempFile << "\" "
            << "-c:v " << encoder << " -preset " << preset
            << " -b:v " << static_cast<int>(bitrate) << "M "
            << "-c:a aac -b:a 128k "
            << "-shortest \"" << outputPath << "\" "
            << ">nul 2>&1";
        
        std::string command = cmdStream.str();
        log::info("ffmpeg: encoding");
        
        int result = std::system(command.c_str());
        
        if (result == 0) {
            log::info("ffmpeg: done");
        } else {
            log::error("ffmpeg: failed");
        }
    }
};