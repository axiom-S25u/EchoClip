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
#include <windows.h>
#include <chrono>

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
    int width, height, fps;
    std::atomic<bool> isEncoding{false};
    std::atomic<bool> shouldStop{false};
    std::thread encodingThread;
    std::queue<PendingFrame> videoQueue;
    mutable std::mutex videoMutex;
    
    HANDLE hPipeWrite = NULL;
    PROCESS_INFORMATION pi = { 0 };

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

        HANDLE hPipeRead = NULL;
        if (!CreatePipe(&hPipeRead, &hPipeWrite, &saAttr, 0)) return false;
        SetHandleInformation(hPipeWrite, HANDLE_FLAG_INHERIT, 0);

        std::ostringstream cmd;
        cmd << "ffmpeg -y -f rawvideo -pix_fmt rgb24 -s " << width << "x" << height 
            << " -r " << fps << " -thread_queue_size 1024 -i - "
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
            return false;
        }

        CloseHandle(hPipeRead); 
        isEncoding = true;
        shouldStop = false;
        encodingThread = std::thread([this] { encodingLoop(); });
        return true;
    }
    
    void stop() {
        if (!isEncoding) return;
        shouldStop = true;
        if (encodingThread.joinable()) encodingThread.join();
        
        if (hPipeWrite) {
            CloseHandle(hPipeWrite);
            hPipeWrite = NULL;
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
        // wait for queue to empty
        while (true) {
            {
                std::lock_guard<std::mutex> lock(videoMutex);
                if (videoQueue.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // delay for ffmpeg to process remaining frames
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    bool submitVideoFrame(const uint8_t* rgbData, size_t w, size_t h, size_t pitch = 0) {
        if (!isEncoding || !rgbData) return false;
        PendingFrame frame;
        frame.data.assign(rgbData, rgbData + (h * w * 3));
        frame.width = w; frame.height = h;
        {
            std::lock_guard<std::mutex> lock(videoMutex);
            if (videoQueue.size() < 30) videoQueue.push(std::move(frame));
        }
        return true;
    }

    bool submitAudioFrame(const float* s, size_t c) { return true; }

private:
    void encodingLoop() {
        while (!shouldStop) {
            PendingFrame frame;
            bool hasFrame = false;
            {
                std::lock_guard<std::mutex> lock(videoMutex);
                if (!videoQueue.empty()) {
                    frame = std::move(videoQueue.front());
                    videoQueue.pop();
                    hasFrame = true;
                }
            }
            if (hasFrame && hPipeWrite) {
                DWORD written;
                WriteFile(hPipeWrite, frame.data.data(), static_cast<DWORD>(frame.data.size()), &written, NULL);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
};