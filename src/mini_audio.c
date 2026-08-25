#include "mini_audio.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#define MAX_AUDIO_BUFFERS 16
#define BUFFER_SAMPLES 2048

static HWAVEOUT g_hWaveOut = NULL;
static int g_sample_rate = 44100;
static int g_channels = 1;
static WAVEHDR g_headers[MAX_AUDIO_BUFFERS];
static short *g_buffers[MAX_AUDIO_BUFFERS];
static int g_buf_idx = 0;
static int g_initialized = 0;

int mini_audio_init(int sample_rate, int channels)
{
    if (g_initialized && g_hWaveOut)
        return 0;

    g_sample_rate = sample_rate > 0 ? sample_rate : 44100;
    g_channels = (channels == 2) ? 2 : 1;

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_channels;
    wfx.nSamplesPerSec = (DWORD)g_sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    MMRESULT res = waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR)
    {
        g_hWaveOut = NULL;
        return -1;
    }

    for (int i = 0; i < MAX_AUDIO_BUFFERS; i++)
    {
        g_buffers[i] = (short *)calloc(BUFFER_SAMPLES * g_channels, sizeof(short));
        memset(&g_headers[i], 0, sizeof(WAVEHDR));
        g_headers[i].lpData = (LPSTR)g_buffers[i];
        g_headers[i].dwBufferLength = (DWORD)(BUFFER_SAMPLES * g_channels * sizeof(short));
        g_headers[i].dwFlags = WHDR_DONE;
    }

    g_initialized = 1;
    return 0;
}

void mini_audio_shutdown(void)
{
    if (!g_initialized || !g_hWaveOut)
        return;

    waveOutReset(g_hWaveOut);
    for (int i = 0; i < MAX_AUDIO_BUFFERS; i++)
    {
        if (g_headers[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_hWaveOut, &g_headers[i], sizeof(WAVEHDR));
        free(g_buffers[i]);
        g_buffers[i] = NULL;
    }
    waveOutClose(g_hWaveOut);
    g_hWaveOut = NULL;
    g_initialized = 0;
}

void mini_audio_queue_pcm(const float *samples, size_t count)
{
    if (!g_initialized || !g_hWaveOut || !samples || count == 0)
        return;

    size_t offset = 0;
    while (offset < count)
    {
        WAVEHDR *hdr = &g_headers[g_buf_idx];
        short *buf = g_buffers[g_buf_idx];

        // Wait or skip if buffer still in use
        if ((hdr->dwFlags & WHDR_PREPARED) && !(hdr->dwFlags & WHDR_DONE))
        {
            // Try next slot
            int found = -1;
            for (int k = 0; k < MAX_AUDIO_BUFFERS; k++)
            {
                if (g_headers[k].dwFlags & WHDR_DONE)
                {
                    found = k;
                    break;
                }
            }
            if (found >= 0)
            {
                g_buf_idx = found;
                hdr = &g_headers[g_buf_idx];
                buf = g_buffers[g_buf_idx];
            }
            else
            {
                break; // All buffers full
            }
        }

        if (hdr->dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_hWaveOut, hdr, sizeof(WAVEHDR));

        size_t chunk = count - offset;
        size_t max_samples = BUFFER_SAMPLES * (size_t)g_channels;
        if (chunk > max_samples)
            chunk = max_samples;

        for (size_t i = 0; i < chunk; i++)
        {
            float s = samples[offset + i];
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            buf[i] = (short)(s * 32767.0f);
        }

        hdr->lpData = (LPSTR)buf;
        hdr->dwBufferLength = (DWORD)(chunk * sizeof(short));
        hdr->dwFlags = 0;

        waveOutPrepareHeader(g_hWaveOut, hdr, sizeof(WAVEHDR));
        waveOutWrite(g_hWaveOut, hdr, sizeof(WAVEHDR));

        offset += chunk;
        g_buf_idx = (g_buf_idx + 1) % MAX_AUDIO_BUFFERS;
    }
}
