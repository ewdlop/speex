using NAudio.Wave;
using System.Media;

string wavFile = "recorded_audio_20240804_143022.wav";
string pcmFile = "mono_micin_48k_s16_mono.pcm_speaker_48k_s16_mono.pcm_20250804_161317.pcm";
string pcmFile2 = "micin_48k_s16_mono.pcm";
string pcmFile3 = "speaker_48k_s16_mono_161317.pcm";

// Play WAV file (most common)
if (File.Exists(wavFile))
{
    Console.WriteLine("Playing WAV file...");
    PlayWavFile(wavFile);
}

// Play raw PCM file (from C++ output)
if (File.Exists(pcmFile))
{
    Console.WriteLine("Playing raw PCM file...");
    //PlayRawPcm(pcmFile, 48000, 1, 16); // 48kHz, mono, 16-bit
    _ = Task.Run(() => PlayRawPcm(pcmFile2, 48000, 1, 16));
    _ = Task.Run(() => PlayRawPcm(pcmFile3, 48000, 1, 16));
}
Console.ReadLine();

// Async playback example
if (File.Exists(wavFile))
{
    Console.WriteLine("Async playback...");
    await PlayWavFileAsync(wavFile);
}

// Method 1: Play WAV file using NAudio (Recommended)
static void PlayWavFile(string fileName)
{
    using var audioFile = new AudioFileReader(fileName);
    using var outputDevice = new WaveOutEvent();

    outputDevice.Init(audioFile);
    outputDevice.Play();

    Console.WriteLine($"Playing: {fileName}");
    Console.WriteLine("Press any key to stop playback...");

    // Wait for playback to finish or user input
    while (outputDevice.PlaybackState == PlaybackState.Playing)
    {
        if (Console.KeyAvailable)
        {
            Console.ReadKey();
            break;
        }
        Thread.Sleep(100);
    }

    outputDevice.Stop();
}

// Method 2: Play with volume control
static void PlayWavFileWithVolume(string fileName, float volume = 0.5f)
{
    using var audioFile = new AudioFileReader(fileName);
    using var outputDevice = new WaveOutEvent();

    audioFile.Volume = volume; // 0.0f to 1.0f
    outputDevice.Init(audioFile);
    outputDevice.Play();

    Console.WriteLine($"Playing: {fileName} (Volume: {volume * 100}%)");
    Console.WriteLine("Press any key to stop...");

    while (outputDevice.PlaybackState == PlaybackState.Playing)
    {
        if (Console.KeyAvailable)
        {
            Console.ReadKey();
            break;
        }
        Thread.Sleep(100);
    }
}

// Method 4: Async playback with NAudio
static async Task PlayWavFileAsync(string fileName)
{
    using var audioFile = new AudioFileReader(fileName);
    using var outputDevice = new WaveOutEvent();

    var tcs = new TaskCompletionSource<bool>();

    outputDevice.PlaybackStopped += (s, e) => tcs.SetResult(true);
    outputDevice.Init(audioFile);
    outputDevice.Play();

    Console.WriteLine($"Playing: {fileName}");
    await tcs.Task;
    Console.WriteLine("Playback finished.");
}

// Method 5: Play raw PCM data (if you have PCM files from the C++ version)
static void PlayRawPcm(string fileName, int sampleRate, int channels = 1, int bitsPerSample = 16)
{
    var format = new WaveFormat(sampleRate, bitsPerSample, channels);

    using var fileStream = new FileStream(fileName, FileMode.Open, FileAccess.Read);
    using var rawSource = new RawSourceWaveStream(fileStream, format);
    using var outputDevice = new WaveOutEvent();

    outputDevice.Init(rawSource);
    outputDevice.Play();

    Console.WriteLine($"Playing raw PCM: {fileName}");
    Console.WriteLine($"Format: {sampleRate}Hz, {channels} channel(s), {bitsPerSample}-bit");
    Console.WriteLine("Press any key to stop...");

    while (outputDevice.PlaybackState == PlaybackState.Playing)
    {
        if (Console.KeyAvailable)
        {
            Console.ReadKey();
            break;
        }
        Thread.Sleep(100);
    }
}