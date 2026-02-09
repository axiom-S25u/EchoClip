#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <fmod.hpp>

using namespace geode::prelude;

struct AudioFrame {
    std::vector<float> samples;
    size_t sampleCount = 0;
    int64_t timestampMs = 0;
};

class FMODMicrophoneCapture {
private:
    FMOD::System* m_system = nullptr;
    FMOD::Sound* m_recordSound = nullptr;
    int m_recordDevice = 0;
    
    std::deque<AudioFrame> audioQueue;
    mutable std::mutex queueMutex;
    
    std::atomic<bool> isCapturing{false};
    std::atomic<bool> shouldStop{false};
    std::thread captureThread;
    
    unsigned int m_recordLastPosition = 0;
    unsigned int m_recordChunkSize = 0;
    std::chrono::steady_clock::time_point captureStartTime;
    
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;
    static constexpr size_t MAX_QUEUE_SIZE = 1000;

public:
    FMODMicrophoneCapture() = default;
    
    ~FMODMicrophoneCapture() {
        stop();
    }
    
    bool initialize(FMOD::System* system) {
        m_system = system;
        if (!m_system) {
            log::error("fmod: system is null");
            return false;
        }
        
        int numDrivers = 0;
        int numConnected = 0;
        FMOD_RESULT res = m_system->getRecordNumDrivers(&numDrivers, &numConnected);
        
        if (numDrivers == 0) {
            log::warn("fmod: no recording devices");
            return false;
        }
        
        m_recordDevice = 0;
        return true;
    }
    
    bool start() {
        if (isCapturing) return true;
        if (!m_system) return false;
        
        FMOD_CREATESOUNDEXINFO exinfo = {};
        exinfo.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
        exinfo.numchannels = CHANNELS;
        exinfo.format = FMOD_SOUND_FORMAT_PCMFLOAT;
        exinfo.defaultfrequency = SAMPLE_RATE;
        exinfo.length = sizeof(float) * SAMPLE_RATE * CHANNELS;
        m_recordChunkSize = exinfo.length;
        
        FMOD_RESULT res = m_system->createSound(
            nullptr,
            FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_CREATESAMPLE,
            &exinfo,
            &m_recordSound
        );
        
        if (res != FMOD_OK) return false;
        
        res = m_system->recordStart(m_recordDevice, m_recordSound, true);
        if (res != FMOD_OK) {
            if (m_recordSound) {
                m_recordSound->release();
                m_recordSound = nullptr;
            }
            return false;
        }
        
        isCapturing = true;
        shouldStop = false;
        m_recordLastPosition = 0;
        captureStartTime = std::chrono::steady_clock::now();
        captureThread = std::thread([this] { captureLoop(); });
        
        return true;
    }
    
    void stop() {
        if (!isCapturing) return;
        shouldStop = true;
        if (captureThread.joinable()) captureThread.join();
        
        if (m_system) m_system->recordStop(m_recordDevice);
        if (m_recordSound) {
            m_recordSound->release();
            m_recordSound = nullptr;
        }
        isCapturing = false;
    }
    
    bool getAudio(AudioFrame& outFrame) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (audioQueue.empty()) return false;
        
        outFrame = std::move(audioQueue.front());
        audioQueue.pop_front();
        return true;
    }
    
    std::vector<AudioFrame> popAllFrames() {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::vector<AudioFrame> result;
        result.reserve(audioQueue.size());
        while (!audioQueue.empty()) {
            result.push_back(std::move(audioQueue.front()));
            audioQueue.pop_front();
        }
        return result;
    }

private:
    void captureLoop() {
        while (!shouldStop) {
            if (!m_system || !m_recordSound) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            unsigned int pos = 0;
            m_system->getRecordPosition(m_recordDevice, &pos);
            
            if (pos == m_recordLastPosition) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            
            float* pcmData = nullptr;
            unsigned int pcmLen = 0;
            
            if (m_recordSound->lock(0, m_recordChunkSize, (void**)&pcmData, nullptr, &pcmLen, nullptr) != FMOD_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            
            size_t samplesToCopy = 0;
            if (pos > m_recordLastPosition) samplesToCopy = pos - m_recordLastPosition;
            else samplesToCopy = (pcmLen / sizeof(float)) - m_recordLastPosition + pos;
            
            if (samplesToCopy > 0) {
                AudioFrame frame;
                frame.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - captureStartTime).count();
                frame.sampleCount = samplesToCopy;
                frame.samples.resize(samplesToCopy * CHANNELS);
                
                if (pos > m_recordLastPosition) {
                    std::memcpy(frame.samples.data(), pcmData + m_recordLastPosition, samplesToCopy * sizeof(float));
                } else {
                    size_t remaining = (pcmLen / sizeof(float)) - m_recordLastPosition;
                    std::memcpy(frame.samples.data(), pcmData + m_recordLastPosition, remaining * sizeof(float));
                    std::memcpy(frame.samples.data() + remaining, pcmData, (samplesToCopy - remaining) * sizeof(float));
                }
                
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    audioQueue.push_back(std::move(frame));
                    if (audioQueue.size() > MAX_QUEUE_SIZE) audioQueue.pop_front();
                }
            }
            m_recordLastPosition = pos;
            m_recordSound->unlock(pcmData, nullptr, pcmLen, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};