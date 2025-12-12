#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "udp_client.h"  // For UDP streaming
#include "audio_handler.h"

static const char *TAG = "AUDIO_HANDLER";

// I2S pin definitions for INMP441 microphone input
#define I2S_MIC_SCK_GPIO    4   // SCK - Serial Clock (to INMP441 SCK)
#define I2S_MIC_WS_GPIO     5   // WS - Word Select (to INMP441 WS) 
#define I2S_MIC_SD_GPIO     6   // SD - Serial Data (to INMP441 SD)

// I2S pin definitions for audio output (DAC/Amplifier)
#define I2S_SPK_SCK_GPIO    7   // BCK - Bit Clock (to amplifier BCK)
#define I2S_SPK_WS_GPIO     8   // LCK - Left/Right Clock (to amplifier LCK)
#define I2S_SPK_SD_GPIO     9   // DIN - Data Input (to amplifier DIN)

#define I2S_MCK_GPIO        GPIO_NUM_NC  // Not used

// Audio streaming buffer size - smaller to allow streaming
#define AUDIO_STREAM_BUFFER_SIZE 4096

static i2s_chan_handle_t tx_handle = NULL;  // For speaker output
static i2s_chan_handle_t rx_handle = NULL;  // For microphone input

// Forward declarations
esp_err_t audio_stop_streaming(uint32_t *chunks_sent);

// Timing helper function
static inline int64_t get_time_ms(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}



// Replace your I2S configuration with this WORKING version
// Based on the proven ESP32-S3 + INMP441 example you provided

// Replace your I2S configuration with this WORKING version
// Based on the proven ESP32-S3 + INMP441 example you provided

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S audio...");
    
    // I2S channel configuration for RX (microphone) - FIXED CONFIG
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // FIXED: Increase DMA buffer to accommodate 40ms chunks (3,840 bytes)
    // Each frame is 2 bytes (16-bit mono), so 3,840 bytes = 1,920 frames
    // Use 2,048 frames per descriptor for good margin
    rx_chan_cfg.dma_desc_num = 16;     // Reduced descriptors for memory efficiency
    rx_chan_cfg.dma_frame_num = 1024; // Increased from 1024 to handle 40ms chunks
    
    esp_err_t ret = i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // I2S channel configuration for TX (speaker)
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 8;
    tx_chan_cfg.dma_frame_num = 512;
    
    ret = i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // WORKING I2S configuration for INMP441 - COPIED FROM PROVEN EXAMPLE
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE_CAPTURE,  // 48000 for INMP441
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,    // 16-bit like working example
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,     // Auto width
            .slot_mode = I2S_SLOT_MODE_MONO,               // MONO like working example
            .slot_mask = I2S_STD_SLOT_LEFT,                // LEFT ONLY like working example
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,                             // I2S standard format
            .left_align = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK_GPIO,    // Your GPIO 4 → INMP441 SCK
            .ws = I2S_MIC_WS_GPIO,       // Your GPIO 5 → INMP441 WS  
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_SD_GPIO,      // Your GPIO 6 → INMP441 SD
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // I2S standard configuration for TX (speaker) - EXPLICIT CONFIG for OpenAI 24kHz
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE_OUTPUT,  // 24000 Hz for OpenAI Realtime API
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,    // 16-bit like OpenAI
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,     // Auto width
            .slot_mode = I2S_SLOT_MODE_MONO,               // MONO like OpenAI
            .slot_mask = I2S_STD_SLOT_LEFT,                // LEFT ONLY
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,                             // I2S standard format
            .left_align = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_SCK_GPIO,
            .ws = I2S_SPK_WS_GPIO,
            .dout = I2S_SPK_SD_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_LOGI(TAG, "I2S TX Configuration:");
    ESP_LOGI(TAG, "  Sample Rate: %lu Hz", tx_std_cfg.clk_cfg.sample_rate_hz);
    ESP_LOGI(TAG, "  Data Width: %d bits", tx_std_cfg.slot_cfg.data_bit_width);
    ESP_LOGI(TAG, "  Slot Mode: %s", tx_std_cfg.slot_cfg.slot_mode == I2S_SLOT_MODE_MONO ? "MONO" : "STEREO");
    ESP_LOGI(TAG, "  GPIO: BCK=%d, LCK=%d, DIN=%d", I2S_SPK_SCK_GPIO, I2S_SPK_WS_GPIO, I2S_SPK_SD_GPIO);
    
    ESP_LOGI(TAG, "Initializing I2S RX channel...");
    ret = i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to initialize I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S RX channel initialized successfully");

    ESP_LOGI(TAG, "Initializing I2S TX channel...");
    ret = i2s_channel_init_std_mode(tx_handle, &tx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to initialize I2S TX: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "❌ This will prevent audio playback!");
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S TX channel initialized successfully");
    
    ESP_LOGI(TAG, "I2S initialized successfully with PROVEN INMP441 settings");
    ESP_LOGI(TAG, "Microphone: SCK=%d, WS=%d, SD=%d", I2S_MIC_SCK_GPIO, I2S_MIC_WS_GPIO, I2S_MIC_SD_GPIO);
    ESP_LOGI(TAG, "Speaker: BCK=%d, LCK=%d, DIN=%d", I2S_SPK_SCK_GPIO, I2S_SPK_WS_GPIO, I2S_SPK_SD_GPIO);
    ESP_LOGI(TAG, "CRITICAL: Ensure INMP441 L/R pin is connected to GND!");

    // audio_play_test_tone();

    return ESP_OK;
}

esp_err_t audio_play_test_tone(void)
{
    ESP_LOGI(TAG, "Generating test tone...");

    size_t tone_duration_samples = AUDIO_SAMPLE_RATE_OUTPUT / 4; // 0.25 second
    size_t tone_buffer_size = tone_duration_samples * 2;
    // Use PSRAM for tone buffer if large enough
    int16_t *tone_buffer = NULL;
    if (tone_buffer_size > 5000) {
        tone_buffer = (int16_t*)heap_caps_malloc(tone_buffer_size, MALLOC_CAP_SPIRAM);
    } else {
        tone_buffer = (int16_t*)malloc(tone_buffer_size);
    }

    if (!tone_buffer) {
        ESP_LOGE(TAG, "Failed to allocate tone buffer");
        return ESP_ERR_NO_MEM;
    }

    // Generate 800Hz tone
    for (size_t i = 0; i < tone_duration_samples; i++) {
        float time = (float)i / AUDIO_SAMPLE_RATE_OUTPUT;
        float tone = sin(2.0 * M_PI * 800.0 * time);
        tone_buffer[i] = (int16_t)(tone * 4000); // Low volume
    }

    esp_err_t ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX: %s", esp_err_to_name(ret));
        heap_caps_free(tone_buffer);
        return ret;
    }

    size_t bytes_written = 0;
    i2s_channel_write(tx_handle, tone_buffer, tone_buffer_size, &bytes_written, pdMS_TO_TICKS(1000));

    vTaskDelay(pdMS_TO_TICKS(300));

    // CRITICAL: Disable TX when done to prevent beeping
    i2s_channel_disable(tx_handle);
    heap_caps_free(tone_buffer);

    ESP_LOGI(TAG, "Test tone complete");
    return ESP_OK;
}







esp_err_t audio_play_pcm(uint8_t *pcm_data, size_t pcm_size)
{
    ESP_LOGI(TAG, "🔊 AUDIO_PLAY_PCM: Starting playback of %zu bytes", pcm_size);

    // Enhanced input validation
    if (!pcm_data) {
        ESP_LOGE(TAG, "🔊 AUDIO_PLAY_PCM: ❌ PCM data pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (pcm_size == 0) {
        ESP_LOGE(TAG, "🔊 AUDIO_PLAY_PCM: ❌ PCM size is 0");
        return ESP_ERR_INVALID_ARG;
    }

    size_t actual_size = pcm_size;
    uint8_t *play_data = pcm_data;

    if (pcm_size % 2 != 0) {
        ESP_LOGW(TAG, "Padding odd-sized audio: %zu -> %zu bytes", pcm_size, pcm_size + 1);
        actual_size = pcm_size + 1;
        play_data = malloc(actual_size);
        if (play_data) {
            memcpy(play_data, pcm_data, pcm_size);
            play_data[pcm_size] = 0;  // Pad with zero
        } else {
            actual_size = pcm_size;
            play_data = pcm_data;
        }
    }

    ESP_LOGD(TAG, "🔊 AUDIO_PLAY_PCM: Validated - PCM data=%p, size=%zu bytes (%zu samples)",
             pcm_data, pcm_size, pcm_size / 2);

    // Check if TX handle is valid
    if (!tx_handle) {
        ESP_LOGE(TAG, "🔊 AUDIO_PLAY_PCM: ❌ TX handle is NULL - I2S not initialized?");
        return ESP_ERR_INVALID_STATE;
    }

    // Enable TX channel
    esp_err_t ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "🔊 AUDIO_PLAY_PCM: ❌ Failed to enable TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Audio playback variables
    size_t total_written = 0;
    size_t bytes_written = 0;
    uint8_t *data_ptr = pcm_data;
    size_t chunk_size = 1024;

    while (total_written < pcm_size) {
        size_t to_write = (pcm_size - total_written) < chunk_size ?
                          (pcm_size - total_written) : chunk_size;

        ret = i2s_channel_write(tx_handle, data_ptr, to_write, &bytes_written, pdMS_TO_TICKS(2000));

        if (ret == ESP_OK) {
            total_written += bytes_written;
            data_ptr += bytes_written;
        } else {
            ESP_LOGE(TAG, "🔊 WRITE_ERROR: i2s_channel_write failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    // Wait for playback to complete
    vTaskDelay(pdMS_TO_TICKS(100));

    // CRITICAL: Always disable TX when done
    i2s_channel_disable(tx_handle);

    if (play_data != pcm_data) {
        free(play_data);
    }

    if (ret == ESP_OK && total_written == pcm_size) {
        ESP_LOGD(TAG, "🔊 AUDIO_PLAY_PCM: ✅ Played %zu bytes", total_written);
        return ESP_OK;
    }

    return ret;
}

// Test function to verify I2S TX works with known-good audio
esp_err_t audio_test_tx_with_known_sample(void)
{
    ESP_LOGI(TAG, "🔊 TESTING: I2S TX with known-good audio sample");

    // Generate a simple test pattern - 24kHz, 16-bit, mono
    const size_t test_duration_ms = 500;  // 0.5 seconds
    const size_t sample_rate = AUDIO_SAMPLE_RATE_OUTPUT;  // 24kHz
    const size_t test_samples = (sample_rate * test_duration_ms) / 1000;
    const size_t test_size = test_samples * 2;  // 16-bit = 2 bytes per sample

    ESP_LOGI(TAG, "🔊 TEST_SAMPLE: Generating %zu samples (%zu bytes) at %zu Hz",
             test_samples, test_size, sample_rate);

    // Use PSRAM for test buffer if large enough
    int16_t *test_buffer = NULL;
    if (test_size > 5000) {
        test_buffer = (int16_t*)heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);
    } else {
        test_buffer = (int16_t*)malloc(test_size);
    }
    if (!test_buffer) {
        ESP_LOGE(TAG, "🔊 TEST_SAMPLE: Failed to allocate test buffer");
        return ESP_ERR_NO_MEM;
    }

    // Generate a simple 800Hz tone
    for (size_t i = 0; i < test_samples; i++) {
        float time = (float)i / sample_rate;
        float tone = sin(2.0 * M_PI * 800.0 * time);
        test_buffer[i] = (int16_t)(tone * 8000);  // Moderate volume
    }

    ESP_LOGI(TAG, "🔊 TEST_SAMPLE: Generated 800Hz tone, first few samples: %d, %d, %d, %d",
             test_buffer[0], test_buffer[1], test_buffer[2], test_buffer[3]);

    // Test playback using our enhanced audio_play_pcm function
    ESP_LOGI(TAG, "🔊 TEST_SAMPLE: Testing playback via audio_play_pcm...");
    esp_err_t ret = audio_play_pcm((uint8_t*)test_buffer, test_size);

    heap_caps_free(test_buffer);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "🔊 TEST_SAMPLE: ✅ SUCCESS - I2S TX channel works with known sample!");
        ESP_LOGI(TAG, "🔊 TEST_SAMPLE: This means the issue is likely with the incoming audio data format");
    } else {
        ESP_LOGE(TAG, "🔊 TEST_SAMPLE: ❌ FAILED - I2S TX channel has issues: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "🔊 TEST_SAMPLE: This indicates a fundamental I2S TX problem");
    }

    return ret;
}

// Test function to detect "y y y y y" repeating bug
// Generates tone with abrupt ending and verifies no repetition after stop
esp_err_t audio_test_abrupt_ending(void)
{
    ESP_LOGI(TAG, "🧪 TESTING: Abrupt ending (verifying no 'y y y y' bug)");

    const size_t test_duration_ms = 500;  // 0.5 seconds
    const size_t sample_rate = AUDIO_SAMPLE_RATE_OUTPUT;  // 24kHz
    const size_t test_samples = (sample_rate * test_duration_ms) / 1000;
    const size_t test_size = test_samples * 2;

    // Generate tone with SUDDEN STOP (no fade-out)
    int16_t *test_buffer = heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);
    if (!test_buffer) {
        ESP_LOGE(TAG, "Failed to allocate test buffer");
        return ESP_ERR_NO_MEM;
    }

    // 1000Hz tone at FULL VOLUME with abrupt ending
    for (size_t i = 0; i < test_samples; i++) {
        float time = (float)i / sample_rate;
        float tone = sin(2.0 * M_PI * 1000.0 * time);
        test_buffer[i] = (int16_t)(tone * 16000);  // High volume for clear detection
    }

    ESP_LOGI(TAG, "🔊 Playing tone with ABRUPT ending (no fade)...");
    ESP_LOGI(TAG, "   Listen carefully: there should be NO repeating 'zzz' sound after the tone");

    esp_err_t ret = audio_play_pcm((uint8_t*)test_buffer, test_size);
    heap_caps_free(test_buffer);

    // Wait extra time to detect any repetition
    ESP_LOGI(TAG, "⏳ Waiting 2 seconds to detect any stale buffer replay...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ TEST PASSED - No audible repetition detected");
        ESP_LOGI(TAG, "   If you heard repeating sound, the DMA buffer clear fix needs adjustment");
    } else {
        ESP_LOGE(TAG, "❌ TEST FAILED - Playback error: %s", esp_err_to_name(ret));
    }

    return ret;
}

// Add this quick test function based on the working example
esp_err_t audio_test_microphone_quick(void)
{
    ESP_LOGI(TAG, "=== QUICK MICROPHONE TEST ===");
    ESP_LOGI(TAG, "Using PROVEN INMP441 configuration");
    ESP_LOGI(TAG, "Testing for 3 seconds - SPEAK INTO THE MIC!");
    
    const size_t buffer_len = 1024; // Same as working example
    // Use PSRAM for test buffer if large enough
    int16_t *buffer = NULL;
    if (buffer_len > 2000) {
        buffer = (int16_t*)heap_caps_malloc(buffer_len, MALLOC_CAP_SPIRAM);
    } else {
        buffer = malloc(buffer_len);
    }
    
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(ret));
        free(buffer);
        return ret;
    }
    
    // Test for 3 seconds like the working example
    for (int test_round = 0; test_round < 30; test_round++) { // ~3 seconds at 100ms intervals
        size_t bytes_read = 0;
        ret = i2s_channel_read(rx_handle, buffer, buffer_len, &bytes_read, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK && bytes_read > 0) {
            int samples_read = bytes_read / 2; // 16-bit = 2 bytes per sample
            float mean = 0;
            int non_zero = 0;
            
            // Calculate mean like the working example
            for (int i = 0; i < samples_read; ++i) {
                mean += buffer[i];
                if (buffer[i] != 0) non_zero++;
            }
            mean /= samples_read;
            
            ESP_LOGI(TAG, "Round %d: bytes=%zu, samples=%d, mean=%.2f, non_zero=%d", 
                     test_round, bytes_read, samples_read, mean, non_zero);
            
            // Show first few samples for debugging
            if (test_round % 10 == 0) {
                ESP_LOGI(TAG, "Sample data: %d %d %d %d %d", 
                         samples_read > 0 ? buffer[0] : 0,
                         samples_read > 1 ? buffer[1] : 0,
                         samples_read > 2 ? buffer[2] : 0,
                         samples_read > 3 ? buffer[3] : 0,
                         samples_read > 4 ? buffer[4] : 0);
            }
        } else {
            ESP_LOGW(TAG, "Round %d: No data received (ret=%s, bytes=%zu)", 
                     test_round, esp_err_to_name(ret), bytes_read);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    i2s_channel_disable(rx_handle);
    heap_caps_free(buffer);
    
    ESP_LOGI(TAG, "=== TEST COMPLETE ===");
    ESP_LOGI(TAG, "If you see all zeros, check:");
    ESP_LOGI(TAG, "1. INMP441 L/R pin connected to GND");
    ESP_LOGI(TAG, "2. INMP441 VDD connected to 3.3V (not 5V)");
    ESP_LOGI(TAG, "3. All solder joints are good");
    
    return ESP_OK;
}


// Calculate RMS (Root Mean Square) energy of audio samples
// Used for silence detection in continuous recording mode
uint32_t audio_calculate_rms(int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) {
        return 0;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        sum += (sample * sample);
    }

    uint32_t mean = sum / sample_count;

    // Fast integer square root (Babylonian method)
    if (mean == 0) return 0;

    uint32_t x = mean;
    uint32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + mean / x) / 2;
    }

    return x;
}

// Stop I2S TX channel (for interrupting playback)
esp_err_t audio_stop_tx(void)
{
    if (tx_handle) {
        ESP_LOGI(TAG, "🔊 Stopping TX channel (disabling for interrupt)");
        return i2s_channel_disable(tx_handle);
    }
    return ESP_OK;
}


// Persistent streaming state
static uint8_t *streaming_capture_buffer = NULL;
static uint8_t *streaming_output_buffer = NULL;
static uint32_t streaming_sequence = 0;
static bool streaming_active = false;

// just configure and kinda initlize everything before the actual streaming
esp_err_t audio_start_streaming(void)
{
    ESP_LOGI(TAG, "🎙️ Starting streaming...");

    if (streaming_active) {
        ESP_LOGW(TAG, "Streaming already active, stopping previous session first");
        audio_stop_streaming(NULL);
    }

    // Allocate buffers for streaming
    const size_t capture_chunk_size = AUDIO_CHUNK_SIZE_CAPTURE;  // 3,840 bytes
    const size_t output_chunk_size = AUDIO_CHUNK_SIZE_OUTPUT;    // 1,920 bytes

    streaming_capture_buffer = malloc(capture_chunk_size);
    streaming_output_buffer = malloc(output_chunk_size);

    if (!streaming_capture_buffer || !streaming_output_buffer) {
        ESP_LOGE(TAG, "Failed to allocate streaming buffers");
        if (streaming_capture_buffer) {
            free(streaming_capture_buffer);
            streaming_capture_buffer = NULL;
        }
        if (streaming_output_buffer) {
            free(streaming_output_buffer);
            streaming_output_buffer = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    // Enable I2S RX channel
    esp_err_t ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(ret));
        free(streaming_capture_buffer);
        free(streaming_output_buffer);
        streaming_capture_buffer = NULL;
        streaming_output_buffer = NULL;
        return ret;
    }

    // Prime I2S channel with dummy reads
    ESP_LOGI(TAG, "Priming I2S channel...");
    size_t dummy_bytes_read = 0;
    uint8_t dummy_buffer[1024];
    for (int i = 0; i < 3; i++) {
        i2s_channel_read(rx_handle, dummy_buffer, sizeof(dummy_buffer), &dummy_bytes_read, pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Reset sequence counter
    streaming_sequence = 0;
    streaming_active = true;

    ESP_LOGI(TAG, "✅ Streaming started - ready to capture chunks");
    return ESP_OK;
}

esp_err_t audio_stream_one_chunk(void)
{
    if (!streaming_active) {
        ESP_LOGE(TAG, "Streaming not active - call audio_start_streaming() first");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t capture_chunk_size = AUDIO_CHUNK_SIZE_CAPTURE;  // 3,840 bytes
    const size_t output_chunk_size = AUDIO_CHUNK_SIZE_OUTPUT;    // 1,920 bytes

    // Capture one chunk from I2S
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, streaming_capture_buffer, capture_chunk_size,
                                     &bytes_read, pdMS_TO_TICKS(1000));

    if (ret != ESP_OK || bytes_read != capture_chunk_size) {
        ESP_LOGW(TAG, "I2S read issue on chunk %lu: ret=%s, bytes=%zu/%zu",
                 streaming_sequence, esp_err_to_name(ret), bytes_read, capture_chunk_size);
        return ret;
    }

    // Downsample 48kHz → 24kHz (take every 2nd sample)
    size_t input_samples = capture_chunk_size / 2;
    int16_t *input_16 = (int16_t *)streaming_capture_buffer;
    int16_t *output_16 = (int16_t *)streaming_output_buffer;

    for (size_t i = 0; i < input_samples / 2; i++) {
        output_16[i] = input_16[i * 2];
    }

    // Send via UDP with sequence number
    esp_err_t send_ret = udp_send_audio_packet(streaming_output_buffer, output_chunk_size, streaming_sequence);

    if (send_ret == ESP_OK) {
        streaming_sequence++;

        // Log every 25 chunks (1 second worth)
        if (streaming_sequence % 25 == 0) {
            ESP_LOGI(TAG, "📤 Streamed %lu chunks (%.1f seconds)",
                     streaming_sequence, (float)streaming_sequence / 25.0f);
        }
    } else {
        ESP_LOGW(TAG, "Failed to send chunk %lu", streaming_sequence);
        return send_ret;
    }

    return ESP_OK;
}

esp_err_t audio_stop_streaming(uint32_t *chunks_sent)
{
    ESP_LOGI(TAG, "🎙️ Stopping streaming...");

    if (!streaming_active) {
        ESP_LOGW(TAG, "Streaming not active");
        if (chunks_sent) *chunks_sent = 0;
        return ESP_OK;
    }

    // Disable I2S channel
    esp_err_t ret = i2s_channel_disable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable I2S RX: %s", esp_err_to_name(ret));
    }

    // Free buffers
    if (streaming_capture_buffer) {
        free(streaming_capture_buffer);
        streaming_capture_buffer = NULL;
    }
    if (streaming_output_buffer) {
        free(streaming_output_buffer);
        streaming_output_buffer = NULL;
    }

    // Return total chunks sent
    if (chunks_sent) {
        *chunks_sent = streaming_sequence;
    }

    ESP_LOGI(TAG, "✅ Streaming stopped - %lu chunks sent (%.2f seconds)",
             streaming_sequence, (float)streaming_sequence / 25.0f);

    streaming_sequence = 0;
    streaming_active = false;

    return ESP_OK;
}

// Capture chunk to buffer without sending (for continuous recording with RMS detection)
esp_err_t audio_capture_chunk_to_buffer(uint8_t *output_buffer, size_t *bytes_captured)
{
    if (!streaming_active) {
        ESP_LOGE(TAG, "Streaming not active - call audio_start_streaming() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!output_buffer || !bytes_captured) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    const size_t capture_chunk_size = AUDIO_CHUNK_SIZE_CAPTURE;  // 3,840 bytes
    const size_t output_chunk_size = AUDIO_CHUNK_SIZE_OUTPUT;    // 1,920 bytes

    // Capture from I2S
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, streaming_capture_buffer,
                                     capture_chunk_size, &bytes_read,
                                     pdMS_TO_TICKS(1000));

    if (ret != ESP_OK || bytes_read != capture_chunk_size) {
        ESP_LOGW(TAG, "I2S read issue: ret=%s, bytes=%zu/%zu",
                 esp_err_to_name(ret), bytes_read, capture_chunk_size);
        return ret;
    }

    // Downsample 48kHz → 24kHz (take every 2nd sample)
    int16_t *input_16 = (int16_t *)streaming_capture_buffer;
    int16_t *output_16 = (int16_t *)output_buffer;

    size_t input_samples = capture_chunk_size / 2;
    for (size_t i = 0; i < input_samples / 2; i++) {
        output_16[i] = input_16[i * 2];
    }

    *bytes_captured = output_chunk_size;
    return ESP_OK;
}

// ==================== QUEUE-BASED PLAYBACK SYSTEM ====================
// New queue-based playback for precise 40ms chunk handling

static QueueHandle_t audio_playback_queue = NULL;
static TaskHandle_t queue_playback_task_handle = NULL;
static volatile bool queue_playback_active = false;

// Timing metrics for diagnostics
static int64_t last_chunk_time_ms = 0;
static int64_t first_chunk_time_ms = 0;
static uint32_t total_chunks_played = 0;
static uint32_t queue_underrun_count = 0;

// PSRAM-backed queue storage for larger buffer sizes
static StaticQueue_t queue_struct;
static uint8_t *queue_storage_buffer = NULL;

// Volume control: 0.0 (mute) to 1.0 (full volume)
// Set to 0.2 (20%) to prevent AI audio from triggering interrupt
#define PLAYBACK_VOLUME_SCALE 0.05f

esp_err_t audio_playback_queue_init(void)
{
    ESP_LOGI(TAG, "Initializing queue-based playback...");

    // Calculate queue storage size
    size_t storage_size = AUDIO_QUEUE_LENGTH * sizeof(audio_chunk_t);

    // Allocate queue storage from PSRAM (allows 128+ slots without heap overflow)
    queue_storage_buffer = heap_caps_malloc(storage_size, MALLOC_CAP_SPIRAM);
    if (!queue_storage_buffer) {
        ESP_LOGE(TAG, "Failed to allocate queue storage from PSRAM (%zu bytes)", storage_size);
        return ESP_ERR_NO_MEM;
    }

    // Create static queue using PSRAM storage
    audio_playback_queue = xQueueCreateStatic(
        AUDIO_QUEUE_LENGTH,
        sizeof(audio_chunk_t),
        queue_storage_buffer,
        &queue_struct
    );

    if (!audio_playback_queue) {
        ESP_LOGE(TAG, "Failed to create playback queue");
        heap_caps_free(queue_storage_buffer);
        queue_storage_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "✅ Playback queue created (%d slots, %zu bytes from PSRAM)",
             AUDIO_QUEUE_LENGTH, storage_size);
    return ESP_OK;
}

esp_err_t audio_playback_queue_push(const uint8_t *data, size_t len, uint32_t seq, bool is_last)
{
    if (!audio_playback_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len > 1440) {
        ESP_LOGW(TAG, "Chunk too large: %zu bytes (max 1440)", len);
        len = 1440;
    }

    audio_chunk_t chunk;
    memcpy(chunk.data, data, len);
    chunk.length = len;
    chunk.sequence = seq;
    chunk.is_last_chunk = is_last;

    // Try to push to queue (non-blocking)
    if (xQueueSend(audio_playback_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ Queue full, dropping chunk #%lu", seq);
        return ESP_ERR_NO_MEM;
    }

    // Log every 25 chunks (1 second)
    if (seq % 25 == 0) {
        ESP_LOGI(TAG, "📥 Queued chunk #%lu (%zu bytes, %d in queue)",
                 seq, len, uxQueueMessagesWaiting(audio_playback_queue));
    }

    return ESP_OK;
}

static void queue_playback_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🔊 Playback task started");

    // CRITICAL FIX: Reset metrics at the START of each playback session
    // This prevents metrics from carrying over between sessions
    total_chunks_played = 0;
    last_chunk_time_ms = 0;
    first_chunk_time_ms = 0;
    queue_underrun_count = 0;

    audio_chunk_t chunk;
    size_t bytes_written;

    // Enable I2S TX once
    esp_err_t ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(ret));
        queue_playback_active = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "✅ I2S TX enabled, waiting for audio chunks...");

    // CRITICAL FIX: Wait for pre-buffering before starting playback
    // This prevents immediate playback from starving if packets are delayed
    const int MIN_PREBUFFER_CHUNKS = 10;
    ESP_LOGI(TAG, "⏳ Waiting for %d chunks to pre-buffer...", MIN_PREBUFFER_CHUNKS);

    while (queue_playback_active && uxQueueMessagesWaiting(audio_playback_queue) < MIN_PREBUFFER_CHUNKS) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (queue_playback_active) {
        ESP_LOGI(TAG, "✅ Pre-buffer ready (%d chunks), starting playback",
                 uxQueueMessagesWaiting(audio_playback_queue));
    }

    while (queue_playback_active) {
        // Block waiting for chunk (500ms timeout - allows for network jitter)
        if (xQueueReceive(audio_playback_queue, &chunk, pdMS_TO_TICKS(500)) == pdTRUE) {

            // Timing metrics
            int64_t now_ms = get_time_ms();
            if (total_chunks_played == 0) {
                first_chunk_time_ms = now_ms;
            }
            int64_t chunk_interval_ms = (last_chunk_time_ms > 0) ? (now_ms - last_chunk_time_ms) : 0;
            last_chunk_time_ms = now_ms;
            total_chunks_played++;

            // CRITICAL FIX: Apply volume scaling HERE (not in UDP task) to prevent packet loss
            // Volume scaling in UDP task was blocking packet reception, causing massive packet loss
            int16_t *samples = (int16_t *)chunk.data;
            size_t sample_count = chunk.length / 2;
            for (size_t i = 0; i < sample_count; i++) {
                samples[i] = (int16_t)(samples[i] * PLAYBACK_VOLUME_SCALE);
            }

            // Write to I2S - use generous timeout to avoid spurious failures
            // The DMA will pace the actual transmission, write just queues to DMA buffer
            int64_t write_start_ms = get_time_ms();
            ret = i2s_channel_write(tx_handle, chunk.data, chunk.length,
                                   &bytes_written, portMAX_DELAY);
            int64_t write_duration_ms = get_time_ms() - write_start_ms;

            if (ret != ESP_OK || bytes_written != chunk.length) {
                ESP_LOGE(TAG, "I2S write failed: ret=%s, wrote %zu/%zu bytes",
                         esp_err_to_name(ret), bytes_written, chunk.length);
            }

            // Enhanced timing diagnostics every 25 chunks
            if (chunk.sequence % 25 == 0) {
                int queue_depth = uxQueueMessagesWaiting(audio_playback_queue);
                ESP_LOGI(TAG, "⏱️ TIMING: chunk=#%lu interval=%lldms i2s_write=%lldms queue_depth=%d (%.1f%% full)",
                         chunk.sequence, chunk_interval_ms, write_duration_ms, queue_depth,
                         (queue_depth * 100.0f) / AUDIO_QUEUE_LENGTH);
                ESP_LOGI(TAG, "🔊 Played chunk #%lu (%d queued) [Volume: %.0f%%]",
                         chunk.sequence, queue_depth,
                         PLAYBACK_VOLUME_SCALE * 100);
            }

            if (chunk.is_last_chunk) {
                ESP_LOGI(TAG, "🔊 Last chunk written to I2S - draining TX buffer...");

                // CRITICAL FIX: Wait for I2S DMA to finish transmitting all buffered samples
                // The I2S driver buffers multiple DMA descriptors, so we need to ensure
                // all samples are physically transmitted before signaling completion

                // Calculate drain time: DMA buffers + safety margin
                // TX config: 8 descriptors * 512 frames = 4096 samples total buffer
                // At 24kHz, 4096 samples = 170ms
                // Add 50ms margin to ensure complete drain
                vTaskDelay(pdMS_TO_TICKS(220));

                // Log final timing summary
                int64_t total_duration_ms = get_time_ms() - first_chunk_time_ms;
                float expected_duration_ms = total_chunks_played * 40.0f;  // 40ms per chunk
                float timing_error_pct = ((total_duration_ms - expected_duration_ms) / expected_duration_ms) * 100.0f;

                ESP_LOGI(TAG, "📊 PLAYBACK SUMMARY:");
                ESP_LOGI(TAG, "   Chunks played: %lu", total_chunks_played);
                ESP_LOGI(TAG, "   Total time: %lld ms", total_duration_ms);
                ESP_LOGI(TAG, "   Expected time: %.1f ms", expected_duration_ms);
                ESP_LOGI(TAG, "   Timing error: %.1f%%", timing_error_pct);
                ESP_LOGI(TAG, "   Underruns: %lu", queue_underrun_count);

                // Reset metrics
                total_chunks_played = 0;
                last_chunk_time_ms = 0;
                first_chunk_time_ms = 0;
                queue_underrun_count = 0;

                ESP_LOGI(TAG, "🔊 TX buffer drained - sending playback complete");
                udp_send_playback_complete();

                // CRITICAL FIX: Stop playback immediately to prevent underrun repeats
                // The task will exit naturally, and STATE_IDLE from bridge will reset state machine
                queue_playback_active = false;
                ESP_LOGI(TAG, "🔊 Playback complete - task exiting");
                break;  // Exit the while loop immediately
            }
        } else {
            // Timeout waiting for chunk - potential underrun
            if (queue_playback_active && total_chunks_played > 0) {
                queue_underrun_count++;
                ESP_LOGW(TAG, "⚠️ Queue underrun #%lu - no chunk available for 500ms", queue_underrun_count);

                // CRITICAL FIX: Write silence to prevent DMA from looping last chunk
                // This stops the "repeating audio" issue at the end
                static uint8_t silence_buffer[1440] = {0};  // Zero-filled PCM16 silence
                size_t silence_written = 0;
                i2s_channel_write(tx_handle, silence_buffer, sizeof(silence_buffer),
                                 &silence_written, pdMS_TO_TICKS(100));
            }
        }
    }

    // Disable I2S TX when done
    ESP_LOGI(TAG, "🔊 Playback stopped, disabling I2S TX");

    // CRITICAL FIX: Zero out DMA buffers before disable to prevent stale data replay
    // This prevents the "y y y y y" repeating bug where old samples loop
    ESP_LOGI(TAG, "🔊 Clearing DMA buffers to prevent stale data replay...");

    // Write silence to flush DMA buffers (8 descriptors * 512 frames = 4096 samples)
    const size_t silence_size = 4096 * 2;  // 16-bit samples
    int16_t *silence_buffer = calloc(silence_size / 2, sizeof(int16_t));  // Zero-initialized
    if (silence_buffer) {
        size_t written = 0;
        i2s_channel_write(tx_handle, silence_buffer, silence_size, &written, pdMS_TO_TICKS(500));
        free(silence_buffer);

        // INCREASED: Wait 400ms for silence to fully transmit (was 200ms)
        ESP_LOGI(TAG, "✅ DMA buffers cleared");
    } else {
        ESP_LOGW(TAG, "⚠️ Could not allocate silence buffer");
    }

    i2s_channel_disable(tx_handle);

    queue_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

void audio_playback_queue_start(void)
{
    if (queue_playback_active) {
        ESP_LOGW(TAG, "Playback already active");
        return;
    }

    ESP_LOGI(TAG, "🔊 Starting queue-based playback");

    // CRITICAL FIX: Clear any stale chunks from previous response before starting
    audio_chunk_t dummy;
    int cleared_count = 0;
    while (xQueueReceive(audio_playback_queue, &dummy, 0) == pdTRUE) {
        cleared_count++;
    }
    if (cleared_count > 0) {
        ESP_LOGI(TAG, "🗑️ Cleared %d stale chunks from queue before starting", cleared_count);
    }

    queue_playback_active = true;

    // Create playback task
    xTaskCreatePinnedToCore(queue_playback_task, "audio_play_queue",
                           8192, NULL, 6, &queue_playback_task_handle, 0);
}

void audio_playback_queue_stop(void)
{
    if (!queue_playback_active) {
        return;
    }

    ESP_LOGI(TAG, "🔊 Stopping queue-based playback");
    queue_playback_active = false;

    // Wait for task to finish (includes DMA buffer clearing)
    while (queue_playback_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Clear queue
    audio_chunk_t dummy;
    int cleared_count = 0;
    while (xQueueReceive(audio_playback_queue, &dummy, 0) == pdTRUE) {
        cleared_count++;
    }

    if (cleared_count > 0) {
        ESP_LOGI(TAG, "📊 Cleared %d unplayed chunks from queue", cleared_count);
    }

    ESP_LOGI(TAG, "✅ Playback stopped, queue cleared");
}

size_t audio_playback_queue_space(void)
{
    if (!audio_playback_queue) {
        return 0;
    }
    return uxQueueSpacesAvailable(audio_playback_queue);
}
