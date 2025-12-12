#ifndef AUDIO_HANDLER_H
#define AUDIO_HANDLER_H

#include "esp_err.h"
#include <stdint.h>git 
#include <stddef.h>
#include <stdbool.h>

// Audio configuration
#define AUDIO_SAMPLE_RATE_CAPTURE  48000  // INMP441 native rate
#define AUDIO_SAMPLE_RATE_OUTPUT   24000  // OpenAI Realtime API requires 24kHz
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1
#define AUDIO_CHUNK_DURATION_MS 40  // 40ms chunks for real-time (matching bridge server)

// Calculate chunk sizes
#define AUDIO_CHUNK_SIZE_CAPTURE ((AUDIO_SAMPLE_RATE_CAPTURE * AUDIO_BITS_PER_SAMPLE * AUDIO_CHANNELS * AUDIO_CHUNK_DURATION_MS) / (8 * 1000))
#define AUDIO_CHUNK_SIZE_OUTPUT  ((AUDIO_SAMPLE_RATE_OUTPUT * AUDIO_BITS_PER_SAMPLE * AUDIO_CHANNELS * AUDIO_CHUNK_DURATION_MS) / (8 * 1000))

// RMS thresholds for voice detection
#define AUDIO_RMS_STOP_THRESHOLD    500
#define SILENCE_DURATION_MS         5000

// Queue configuration
// ESP32-S3 has 8MB PSRAM - use ~5MB for queue, leaving 3MB for WiFi/other
// 3500 chunks @ ~1452 bytes = ~5MB = 140 seconds of audio (~2.3 minutes)
#define AUDIO_QUEUE_LENGTH 3500  // 3500 chunks = 140 seconds buffer (PSRAM-backed)

// Audio chunk structure for queue
typedef struct {
    uint8_t data[1440];  // Fixed 40ms @ 24kHz
    size_t length;
    uint32_t sequence;
    bool is_last_chunk;
} audio_chunk_t;

// Basic audio functions
esp_err_t audio_init(void);
esp_err_t audio_play_test_tone(void);
esp_err_t audio_test_microphone_quick(void);
esp_err_t audio_test_tx_with_known_sample(void);
esp_err_t audio_test_abrupt_ending(void);  // Test for "y y y y" bug

// Streaming functions
esp_err_t audio_start_streaming(void);
esp_err_t audio_capture_chunk_to_buffer(uint8_t *output_buffer, size_t *bytes_captured);
uint32_t audio_calculate_rms(int16_t *samples, size_t sample_count);

// Queue-based playback functions
esp_err_t audio_playback_queue_init(void);
esp_err_t audio_playback_queue_push(const uint8_t *data, size_t len, uint32_t seq, bool is_last);
void audio_playback_queue_start(void);
void audio_playback_queue_stop(void);
size_t audio_playback_queue_space(void);

#endif // AUDIO_HANDLER_H