// EchoClip
// written by someone who has lost the will to live
// i'm so tired
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GameManager.hpp>

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <fmod.hpp>
#include <fmod_studio.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

using namespace geode::prelude;

// why did i choose c++
static constexpr const char* TEMP_PATH = "echoclip_temp.mp4";
static constexpr const char* OUTPUT_DIR = "echoclip_recordings";
static constexpr int FRAME_WIDTH = 1280;
static constexpr int FRAME_HEIGHT = 720;
static constexpr int TARGET_FPS = 30;
static constexpr int SAMPLE_RATE = 48000;
static constexpr int CHANNELS = 2;
static constexpr int ATTEMPT_WINDOW = 5;
static constexpr int MAX_AUDIO_BUFFER = 4800;


struct AudioChunk {
    std::vector<float> samples;
    int sampleCount = 0;
    int64_t pts = 0;
};

struct AttemptMarker {
    int64_t timestampMs;
    int64_t frameNumber;
    int64_t audioSample;
};


class EchoClipEngine {
public: 
    std::mutex audioMutex;
    std::condition_variable audioCV;
    std::deque<AudioChunk> audioQueue;
    
    std::mutex attemptMutex;
    std::deque<AttemptMarker> attemptMarkers;
    
    std::atomic<bool> isRecording{false};
    std::atomic<bool> shouldStop{false};
    std::thread encodingThread;
    
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* videoCodecCtx = nullptr;
    AVCodecContext* audioCodecCtx = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    
    SwsContext* swsCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    AVBufferRef* hwDeviceCtx = nullptr;
    
    FMOD::Studio::System* fmodStudio = nullptr;
    FMOD::System* fmodCore = nullptr;
    
    int64_t audioSampleCounter = 0;
    std::chrono::steady_clock::time_point recordStart;
    
    EchoClipEngine() {}
    
    ~EchoClipEngine() {
        // why do i keep doing this
        shutdown();
    }
    
    void shutdown() {
        shouldStop = true;
        if (encodingThread.joinable()) {
            encodingThread.join();
        }
        cleanupFFmpeg();
        cleanupAudio();
    }
    
    void cleanupFFmpeg() {
        // i don't know why this works and im too tired to care
        if (swsCtx) {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
        if (swrCtx) {
            swr_free(&swrCtx);
        }
        if (videoCodecCtx) {
            avcodec_free_context(&videoCodecCtx);
        }
        if (audioCodecCtx) {
            avcodec_free_context(&audioCodecCtx);
        }
        if (formatCtx) {
            if (formatCtx->pb) {
                avio_closep(&formatCtx->pb);
            }
            avformat_free_context(formatCtx);
        }
        if (hwDeviceCtx) {
            av_buffer_unref(&hwDeviceCtx);
        }
    }
    
    void cleanupAudio() {
        // please god just compile
        if (fmodCore) {
            fmodCore->release();
            fmodCore = nullptr;
        }
        if (fmodStudio) {
            fmodStudio->release();
            fmodStudio = nullptr;
        }
    }
    
    bool initialize() {
        if (!initFFmpeg()) {
            CC_LOG(CCLogLevel::Error, "FFmpeg init failed. life is meaningless.");
            return false;
        }
        
        if (!initAudio()) {
            CC_LOG(CCLogLevel::Warning, "Audio init failed. we continue anyway.");
        }
        
        isRecording = true;
        recordStart = std::chrono::steady_clock::now();
        audioSampleCounter = 0;
        
        encodingThread = std::thread([this] { audioEncoderLoop(); });
        
        CC_LOG(CCLogLevel::Info, "EchoClip initialized. suffering has begun. PC will stay cool.");
        return true;
    }
    
private:
    bool initFFmpeg() {
        avformat_alloc_output_context2(&formatCtx, nullptr, "mp4", TEMP_PATH);
        if (!formatCtx) {
            CC_LOG(CCLogLevel::Error, "can't allocate output context. i give up.");
            return false;
        }
        
        // try to use nvidia nvenc because apparently i hate myself
        if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
            CC_LOG(CCLogLevel::Warning, "CUDA not available. switching to software encoding hell.");
            hwDeviceCtx = nullptr;
        }
        
        // the source of all my pain
        const AVCodec* videoCodec = nullptr;
        
        if (hwDeviceCtx) {
            videoCodec = avcodec_find_encoder_by_name("hevc_nvenc");
            if (!videoCodec) {
                videoCodec = avcodec_find_encoder_by_name("h264_nvenc");
            }
        }
        
        if (!videoCodec) {
            videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        
        if (!videoCodec) {
            CC_LOG(CCLogLevel::Error, "no video codec found. this is why i drink.");
            return false;
        }
        
        videoCodecCtx = avcodec_alloc_context3(videoCodec);
        if (!videoCodecCtx) {
            CC_LOG(CCLogLevel::Error, "can't allocate video codec context");
            return false;
        }
        
        videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        videoCodecCtx->codec_id = videoCodec->id;
        videoCodecCtx->width = FRAME_WIDTH;
        videoCodecCtx->height = FRAME_HEIGHT;
        videoCodecCtx->time_base = {1, TARGET_FPS};
        videoCodecCtx->framerate = {TARGET_FPS, 1};
        videoCodecCtx->pix_fmt = hwDeviceCtx ? AV_PIX_FMT_CUDA : AV_PIX_FMT_YUV420P;
        videoCodecCtx->bit_rate = 5000000; // 5 mbps, a compromise between quality and my will to live
        videoCodecCtx->gop_size = 30;
        videoCodecCtx->max_b_frames = 0;
        
        if (hwDeviceCtx) {
            videoCodecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        }
        
        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        
        if (avcodec_open2(videoCodecCtx, videoCodec, nullptr) < 0) {
            CC_LOG(CCLogLevel::Error, "can't open video codec. prepare for segfault.");
            return false;
        }
        
        videoStream = avformat_new_stream(formatCtx, videoCodec);
        if (!videoStream) {
            CC_LOG(CCLogLevel::Error, "can't create video stream");
            return false;
        }
        
        avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
        
        // please let this work
        const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audioCodec) {
            audioCodec = avcodec_find_encoder(AV_CODEC_ID_MP3);
        }
        
        if (!audioCodec) {
            CC_LOG(CCLogLevel::Error, "no audio codec. i'm done.");
            return false;
        }
        
        audioCodecCtx = avcodec_alloc_context3(audioCodec);
        if (!audioCodecCtx) {
            CC_LOG(CCLogLevel::Error, "can't allocate audio codec context");
            return false;
        }
        
        audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
        audioCodecCtx->codec_id = audioCodec->id;
        audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audioCodecCtx->sample_rate = SAMPLE_RATE;
        audioCodecCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        audioCodecCtx->bit_rate = 128000;
        audioCodecCtx->time_base = {1, SAMPLE_RATE};
        
        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        
        if (avcodec_open2(audioCodecCtx, audioCodec, nullptr) < 0) {
            CC_LOG(CCLogLevel::Error, "can't open audio codec");
            return false;
        }
        
        audioStream = avformat_new_stream(formatCtx, audioCodec);
        if (!audioStream) {
            CC_LOG(CCLogLevel::Error, "can't create audio stream");
            return false;
        }
        
        avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx);
        
        // another day, another segment fault
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&formatCtx->pb, TEMP_PATH, AVIO_FLAG_WRITE) < 0) {
                CC_LOG(CCLogLevel::Error, "can't open output file");
                return false;
            }
        }
        
        if (avformat_write_header(formatCtx, nullptr) < 0) {
            CC_LOG(CCLogLevel::Error, "can't write header");
            return false;
        }
        
        swsCtx = sws_getContext(
            FRAME_WIDTH, FRAME_HEIGHT, AV_PIX_FMT_RGB24,
            FRAME_WIDTH, FRAME_HEIGHT, AV_PIX_FMT_YUV420P,
            SWS_BICUBIC, nullptr, nullptr, nullptr
        );
        
        if (!swsCtx) {
            CC_LOG(CCLogLevel::Error, "can't create sws context");
            return false;
        }
        
        swrCtx = swr_alloc();
        av_opt_set_int(swrCtx, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swrCtx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swrCtx, "in_sample_rate", SAMPLE_RATE, 0);
        av_opt_set_int(swrCtx, "out_sample_rate", SAMPLE_RATE, 0);
        av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
        
        if (swr_init(swrCtx) < 0) {
            CC_LOG(CCLogLevel::Error, "can't init resampler");
            return false;
        }
        
        CC_LOG(CCLogLevel::Info, "FFmpeg ready. pray.");
        return true;
    }
    
    bool initAudio() {
        FMOD_RESULT result;
        
        result = FMOD::Studio::System::create(&fmodStudio);
        if (result != FMOD_OK) {
            CC_LOG(CCLogLevel::Warning, "can't create fmod studio system");
            return false;
        }
        
        result = fmodStudio->initialize(32, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr);
        if (result != FMOD_OK) {
            CC_LOG(CCLogLevel::Warning, "can't initialize fmod studio");
            return false;
        }
        
        result = fmodStudio->getCoreSystem(&fmodCore);
        if (result != FMOD_OK) {
            CC_LOG(CCLogLevel::Warning, "can't get fmod core system");
            return false;
        }
        
        CC_LOG(CCLogLevel::Info, "FMOD ready. barely.");
        return true;
    }
    
    void audioEncoderLoop() {
        while (!shouldStop) {
            {
                std::unique_lock<std::mutex> lock(audioMutex);
                audioCV.wait_for(lock, std::chrono::milliseconds(15), [this] {
                    return !audioQueue.empty() || shouldStop;
                });
                
                while (!audioQueue.empty() && !shouldStop) {
                    AudioChunk chunk = audioQueue.front();
                    audioQueue.pop();
                    lock.unlock();
                    
                    encodeAudioFrame(chunk);
                    
                    lock.lock();
                }
            }
        }
        
        flushAudioEncoder();
    }
    
    void encodeVideoFrameDirectly(const uint8_t* rgbData) {
        if (!videoCodecCtx || !formatCtx || !rgbData) return;
        
        AVFrame* frame = av_frame_alloc();
        if (!frame) return;
        
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = FRAME_WIDTH;
        frame->height = FRAME_HEIGHT;
        
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return;
        }
        
        const uint8_t* srcPlanes[1] = {rgbData};
        int srcStrides[1] = {FRAME_WIDTH * 3};
        
        uint8_t* dstPlanes[3] = {frame->data[0], frame->data[1], frame->data[2]};
        int dstStrides[3] = {frame->linesize[0], frame->linesize[1], frame->linesize[2]};
        
        sws_scale(swsCtx, srcPlanes, srcStrides, 0, FRAME_HEIGHT, dstPlanes, dstStrides);
        static int64_t frameCounter = 0;
        frame->pts = frameCounter++;
        
        if (avcodec_send_frame(videoCodecCtx, frame) < 0) {
            CC_LOG(CCLogLevel::Warning, "error sending frame");
            av_frame_free(&frame);
            return;
        }
        
        AVPacket* pkt = av_packet_alloc();
        int ret;
        while ((ret = avcodec_receive_packet(videoCodecCtx, pkt)) == 0) {
            av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
            pkt->stream_index = videoStream->index;
            av_interleaved_write_frame(formatCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
    
    void encodeAudioFrame(const AudioChunk& chunk) {
        if (!audioCodecCtx || !formatCtx || chunk.sampleCount == 0) return;
        
        AVFrame* frame = av_frame_alloc();
        if (!frame) return;
        
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        frame->sample_rate = SAMPLE_RATE;
        frame->nb_samples = chunk.sampleCount;
        frame->pts = audioSampleCounter;
        audioSampleCounter += chunk.sampleCount;
        
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return;
        }
        
        float** dst = reinterpret_cast<float**>(frame->data);
        for (int i = 0; i < chunk.sampleCount; ++i) {
            dst[0][i] = chunk.samples[i * 2] * 0.5f;
            dst[1][i] = chunk.samples[i * 2 + 1] * 0.5f;
        }
        
        if (avcodec_send_frame(audioCodecCtx, frame) < 0) {
            av_frame_free(&frame);
            return;
        }
        
        AVPacket* pkt = av_packet_alloc();
        int ret;
        while ((ret = avcodec_receive_packet(audioCodecCtx, pkt)) == 0) {
            av_packet_rescale_ts(pkt, audioCodecCtx->time_base, audioStream->time_base);
            pkt->stream_index = audioStream->index;
            av_interleaved_write_frame(formatCtx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
    
    void flushAudioEncoder() {
        AVPacket* pkt = av_packet_alloc();
        
        if (audioCodecCtx) {
            avcodec_send_frame(audioCodecCtx, nullptr);
            while (avcodec_receive_packet(audioCodecCtx, pkt) == 0) {
                av_packet_rescale_ts(pkt, audioCodecCtx->time_base, audioStream->time_base);
                pkt->stream_index = audioStream->index;
                av_interleaved_write_frame(formatCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        
        av_packet_free(&pkt);
    }
    
    void flushAllEncoders() {
        AVPacket* pkt = av_packet_alloc();
        
        if (videoCodecCtx) {
            avcodec_send_frame(videoCodecCtx, nullptr);
            while (avcodec_receive_packet(videoCodecCtx, pkt) == 0) {
                av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
                pkt->stream_index = videoStream->index;
                av_interleaved_write_frame(formatCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        
        if (audioCodecCtx) {
            avcodec_send_frame(audioCodecCtx, nullptr);
            while (avcodec_receive_packet(audioCodecCtx, pkt) == 0) {
                av_packet_rescale_ts(pkt, audioCodecCtx->time_base, audioStream->time_base);
                pkt->stream_index = audioStream->index;
                av_interleaved_write_frame(formatCtx, pkt);
                av_packet_unref(pkt);
            }
        }
        
        av_packet_free(&pkt);
        
        if (formatCtx) {
            av_write_trailer(formatCtx);
        }
    }
    
public:
    void submitFrame(const uint8_t* rgbData) {
        if (!isRecording || !rgbData) return;
        encodeVideoFrameDirectly(rgbData);
    }
    
    void submitAudio(const float* gameAudio, const float* micAudio, int sampleCount) {
        if (!isRecording || !gameAudio || !micAudio || sampleCount <= 0) return;
        
        AudioChunk chunk;
        chunk.sampleCount = sampleCount;
        chunk.samples.resize(sampleCount * CHANNELS);
        // please dont explode 
        for (int i = 0; i < sampleCount; ++i) {
            float mixedLeft = gameAudio[i * 2] * 0.7f + micAudio[i * 2] * 0.3f;
            float mixedRight = gameAudio[i * 2 + 1] * 0.7f + micAudio[i * 2 + 1] * 0.3f;
            
            chunk.samples[i * 2] = std::clamp(mixedLeft, -1.0f, 1.0f);
            chunk.samples[i * 2 + 1] = std::clamp(mixedRight, -1.0f, 1.0f);
        }
        
        {
            std::lock_guard<std::mutex> lock(audioMutex);
            if (audioQueue.size() < MAX_AUDIO_BUFFER) {
                audioQueue.push_back(chunk);
                audioCV.notify_one();
            }
        }
    }
    
    void recordAttempt() {
        // this is where we cry
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        AttemptMarker marker;
        marker.timestampMs = getRecordingTimeMs();
        marker.frameNumber = getFrameNumber();
        marker.audioSample = audioSampleCounter;
        
        attemptMarkers.push_front(marker);
        
        if (attemptMarkers.size() > ATTEMPT_WINDOW) {
            attemptMarkers.pop_back();
        }
        
        CC_LOG(CCLogLevel::Info, 
               "Attempt recorded - Frame: " + std::to_string(marker.frameNumber) + 
               " Audio: " + std::to_string(marker.audioSample) +
               " Time: " + std::to_string(marker.timestampMs) + "ms");
    }
    
    int64_t getRecordingTimeMs() const {
        auto elapsed = std::chrono::steady_clock::now() - recordStart;
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    }
    
    int64_t getFrameNumber() const {
        auto elapsed = std::chrono::steady_clock::now() - recordStart;
        double seconds = std::chrono::duration<double>(elapsed).count();
        return static_cast<int64_t>(seconds * TARGET_FPS);
    }
    
    bool exportClip(const std::string& outputPath) {
        // oh god this is the worst part
        std::lock_guard<std::mutex> lock(attemptMutex);
        
        if (attemptMarkers.size() < ATTEMPT_WINDOW) {
            CC_LOG(CCLogLevel::Warning, 
                   "not enough attempts recorded: " + std::to_string(attemptMarkers.size()) + 
                   " out of " + std::to_string(ATTEMPT_WINDOW));
            return false;
        }
        const AttemptMarker& oldestAttempt = attemptMarkers.back();
        const AttemptMarker& newestAttempt = attemptMarkers.front();
        
        int64_t startFrame = oldestAttempt.frameNumber;
        int64_t endFrame = newestAttempt.frameNumber;
        
        double startTimeSeconds = static_cast<double>(startFrame) / TARGET_FPS;
        double endTimeSeconds = static_cast<double>(endFrame) / TARGET_FPS;
        double durationSeconds = endTimeSeconds - startTimeSeconds;
        
        CC_LOG(CCLogLevel::Info, 
               "Exporting clip from frame " + std::to_string(startFrame) + 
               " to " + std::to_string(endFrame) + 
               " (" + std::to_string(endFrame - startFrame) + " frames total)");
        
        CC_LOG(CCLogLevel::Info,
               "Time range: " + std::to_string(startTimeSeconds) + "s to " + 
               std::to_string(endTimeSeconds) + "s (" + std::to_string(durationSeconds) + "s duration)");
        
        std::ostringstream cmd;
        cmd << "ffmpeg -ss " << startTimeSeconds 
            << " -i \"" << TEMP_PATH << "\" "
            << "-t " << durationSeconds 
            << " -c copy \"" << outputPath << "\" -y";
        
        std::string command = cmd.str();
        CC_LOG(CCLogLevel::Info, "Running: " + command);
        
        int result = std::system(command.c_str());
        
        if (result == 0) {
            CC_LOG(CCLogLevel::Info, "Export done: " + outputPath);
            return true;
        } else {
            CC_LOG(CCLogLevel::Error, "ffmpeg export failed with code " + std::to_string(result));
            return false;
        }
    }
    
    void finalShutdown() {
        if (videoCodecCtx || audioCodecCtx) {
            flushAllEncoders();
        }
    }
};

static std::unique_ptr<EchoClipEngine> g_engine;

class $modify(PlayLayerEchoClip, PlayLayer) {
    void resetLevel() override {
        if (g_engine) {
            g_engine->recordAttempt();
            CC_LOG(CCLogLevel::Debug, "Attempt recorded by resetLevel hook");
        }
        return PlayLayer::resetLevel();
    }
    
    void onQuit() override {
        // goodbye cruel world
        if (g_engine) {
            CC_LOG(CCLogLevel::Info, "Level exited, stopping recording");
        }
        return PlayLayer::onQuit();
    }
    
    void levelComplete() override {
        // also track level completions as attempts
        if (g_engine) {
            g_engine->recordAttempt();
            CC_LOG(CCLogLevel::Debug, "Level completed - attempt recorded");
        }
        return PlayLayer::levelComplete();
    }
};

$on_mod(Loaded) {
    CC_LOG(CCLogLevel::Info, "EchoClip loading...");
    
    // create output directory
    std::filesystem::create_directories(OUTPUT_DIR);
    
    // initialize the engine
    g_engine = std::make_unique<EchoClipEngine>();
    if (!g_engine->initialize()) {
        CC_LOG(CCLogLevel::Error, "Failed to initialize EchoClip. nothing works.");
        g_engine.reset();
        return;
    }
    
    // register the hotkey
    // TODO: this needs proper keybind registration from geode.custom-keybinds
    // for now just log that we're ready
    CC_LOG(CCLogLevel::Info, "EchoClip loaded. set your hotkey to export clips. your pc will thank you.");
}

$on_mod(Unloaded) {
    CC_LOG(CCLogLevel::Info, "EchoClip unloading. finally.");
    
    if (g_engine) {
        // flush everything before shutdown
        g_engine->finalShutdown();
        g_engine->shutdown();
        g_engine.reset();
    }
    
    // delete temp file
    std::filesystem::remove(TEMP_PATH);
    
    CC_LOG(CCLogLevel::Info, "EchoClip unloaded. it's over.");
}