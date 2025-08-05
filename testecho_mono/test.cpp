#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <portaudio.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

class EchoCancellation {
private:
    // Audio parameters
    static const int SAMPLE_RATE = 16000;
    static const int FRAME_SIZE = 160;  // 10ms at 16kHz
    static const int FILTER_LENGTH = 1600;  // 100ms tail length
    static const int CHANNELS = 1;

    // PortAudio streams
    PaStream* inputStream;
    PaStream* outputStream;

    // Speex components
    SpeexEchoState* echo_state;
    SpeexPreprocessState* preprocess_state;

    // Audio buffers
    std::queue<std::vector<int16_t>> inputBuffer;
    std::queue<std::vector<int16_t>> outputBuffer;
    std::queue<std::vector<int16_t>> echoBuffer;

    // Thread synchronization
    std::mutex inputMutex;
    std::mutex outputMutex;
    std::mutex echoMutex;
    std::condition_variable inputCondition;
    std::condition_variable outputCondition;
    std::atomic<bool> running;

    // Processing thread
    std::thread processingThread;

public:
    EchoCancellation() :
        inputStream(nullptr),
        outputStream(nullptr),
        echo_state(nullptr),
        preprocess_state(nullptr),
        running(false) {
    }

    ~EchoCancellation() {
        cleanup();
    }

    bool initialize() {
        // Initialize PortAudio
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
            return false;
        }

        // Initialize Speex echo cancellation
        echo_state = speex_echo_state_init(FRAME_SIZE, FILTER_LENGTH);
        if (!echo_state) {
            std::cerr << "Failed to initialize Speex echo state" << std::endl;
            return false;
        }

        // Set sample rate for echo cancellation
        int sampleRate = SAMPLE_RATE;
        speex_echo_ctl(echo_state, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);

        // Initialize preprocessing (for additional noise reduction)
        preprocess_state = speex_preprocess_state_init(FRAME_SIZE, SAMPLE_RATE);
        if (!preprocess_state) {
            std::cerr << "Failed to initialize Speex preprocess state" << std::endl;
            return false;
        }

        // Associate echo cancellation with preprocessing
        speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state);

        // Enable various preprocessing features
        int denoise = 1;
        int agc = 1;
        speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_AGC, &agc);

        return setupAudioStreams();
    }

    bool setupAudioStreams() {
        // Setup input stream parameters
        PaStreamParameters inputParameters;
        inputParameters.device = Pa_GetDefaultInputDevice();
        if (inputParameters.device == paNoDevice) {
            std::cerr << "No default input device found" << std::endl;
            return false;
        }

        inputParameters.channelCount = CHANNELS;
        inputParameters.sampleFormat = paInt16;
        inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
        inputParameters.hostApiSpecificStreamInfo = nullptr;

        // Setup output stream parameters
        PaStreamParameters outputParameters;
        outputParameters.device = Pa_GetDefaultOutputDevice();
        if (outputParameters.device == paNoDevice) {
            std::cerr << "No default output device found" << std::endl;
            return false;
        }

        outputParameters.channelCount = CHANNELS;
        outputParameters.sampleFormat = paInt16;
        outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
        outputParameters.hostApiSpecificStreamInfo = nullptr;

        // Open input stream
        PaError err = Pa_OpenStream(&inputStream,
            &inputParameters,
            nullptr,
            SAMPLE_RATE,
            FRAME_SIZE,
            paClipOff,
            inputCallback,
            this);

        if (err != paNoError) {
            std::cerr << "Failed to open input stream: " << Pa_GetErrorText(err) << std::endl;
            return false;
        }

        // Open output stream
        err = Pa_OpenStream(&outputStream,
            nullptr,
            &outputParameters,
            SAMPLE_RATE,
            FRAME_SIZE,
            paClipOff,
            outputCallback,
            this);

        if (err != paNoError) {
            std::cerr << "Failed to open output stream: " << Pa_GetErrorText(err) << std::endl;
            return false;
        }

        return true;
    }

    // Input callback - captures microphone data
    static int inputCallback(const void* inputBuffer, void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void* userData) {

        EchoCancellation* ec = static_cast<EchoCancellation*>(userData);
        const int16_t* input = static_cast<const int16_t*>(inputBuffer);

        if (input) {
            std::vector<int16_t> frame(input, input + framesPerBuffer);

            std::lock_guard<std::mutex> lock(ec->inputMutex);
            ec->inputBuffer.push(frame);
            ec->inputCondition.notify_one();
        }

        return paContinue;
    }

    // Output callback - plays processed audio
    static int outputCallback(const void* inputBuffer, void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void* userData) {

        EchoCancellation* ec = static_cast<EchoCancellation*>(userData);
        int16_t* output = static_cast<int16_t*>(outputBuffer);

        std::unique_lock<std::mutex> lock(ec->outputMutex);

        if (!ec->outputBuffer.empty()) {
            std::vector<int16_t> frame = ec->outputBuffer.front();
            ec->outputBuffer.pop();
            lock.unlock();

            std::copy(frame.begin(), frame.end(), output);

            // Store echo reference for cancellation
            std::lock_guard<std::mutex> echoLock(ec->echoMutex);
            ec->echoBuffer.push(frame);

            // Limit echo buffer size to prevent memory issues
            while (ec->echoBuffer.size() > 10) {
                ec->echoBuffer.pop();
            }
        }
        else {
            // Output silence if no data available
            std::fill(output, output + framesPerBuffer, 0);
        }

        return paContinue;
    }

    void processAudio() {
        std::vector<int16_t> micFrame(FRAME_SIZE);
        std::vector<int16_t> speakerFrame(FRAME_SIZE);
        std::vector<int16_t> outputFrame(FRAME_SIZE);

        while (running) {
            // Wait for input data
            std::unique_lock<std::mutex> inputLock(inputMutex);
            inputCondition.wait(inputLock, [this] { return !inputBuffer.empty() || !running; });

            if (!running) break;

            if (!inputBuffer.empty()) {
                micFrame = inputBuffer.front();
                inputBuffer.pop();
                inputLock.unlock();

                // Get echo reference (what was played from speakers)
                std::lock_guard<std::mutex> echoLock(echoMutex);
                if (!echoBuffer.empty()) {
                    speakerFrame = echoBuffer.front();
                    echoBuffer.pop();
                }
                else {
                    // No echo reference available, use silence
                    std::fill(speakerFrame.begin(), speakerFrame.end(), 0);
                }

                // Perform echo cancellation
                speex_echo_cancellation(echo_state,
                    micFrame.data(),
                    speakerFrame.data(),
                    outputFrame.data());

                // Apply additional preprocessing (noise reduction, AGC)
                speex_preprocess_run(preprocess_state, outputFrame.data());

                // Add processed frame to output buffer
                std::lock_guard<std::mutex> outputLock(outputMutex);
                outputBuffer.push(outputFrame);
            }
        }
    }

    bool start() {
        if (running) return false;

        running = true;

        // Start processing thread
        processingThread = std::thread(&EchoCancellation::processAudio, this);

        // Start audio streams
        PaError err = Pa_StartStream(inputStream);
        if (err != paNoError) {
            std::cerr << "Failed to start input stream: " << Pa_GetErrorText(err) << std::endl;
            running = false;
            return false;
        }

        err = Pa_StartStream(outputStream);
        if (err != paNoError) {
            std::cerr << "Failed to start output stream: " << Pa_GetErrorText(err) << std::endl;
            running = false;
            Pa_StopStream(inputStream);
            return false;
        }

        std::cout << "Echo cancellation started. Press Enter to stop..." << std::endl;
        return true;
    }

    void stop() {
        if (!running) return;

        running = false;

        // Stop audio streams
        if (inputStream) Pa_StopStream(inputStream);
        if (outputStream) Pa_StopStream(outputStream);

        // Wake up processing thread
        inputCondition.notify_all();

        // Wait for processing thread to finish
        if (processingThread.joinable()) {
            processingThread.join();
        }

        std::cout << "Echo cancellation stopped." << std::endl;
    }

    void cleanup() {
        stop();

        // Close streams
        if (inputStream) {
            Pa_CloseStream(inputStream);
            inputStream = nullptr;
        }
        if (outputStream) {
            Pa_CloseStream(outputStream);
            outputStream = nullptr;
        }

        // Cleanup Speex
        if (echo_state) {
            speex_echo_state_destroy(echo_state);
            echo_state = nullptr;
        }
        if (preprocess_state) {
            speex_preprocess_state_destroy(preprocess_state);
            preprocess_state = nullptr;
        }

        // Terminate PortAudio
        Pa_Terminate();
    }
};

int main() {
    std::cout << "Speex Echo Cancellation Demo" << std::endl;
    std::cout << "=============================" << std::endl;

    EchoCancellation ec;

    if (!ec.initialize()) {
        std::cerr << "Failed to initialize echo cancellation" << std::endl;
        return -1;
    }

    if (!ec.start()) {
        std::cerr << "Failed to start echo cancellation" << std::endl;
        return -1;
    }

    // Wait for user input to stop
    std::cin.get();

    ec.stop();

    return 0;
}