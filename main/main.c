#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "wifi_handler.h"
#include "udp_client.h"
#include "audio_handler.h"

// loggin tag
static const char *TAG = "VOICE_ASSISTANT";

// GPIO definitions
#define LED_GPIO        2    // Built-in LED

// State management (voice_state_t is defined in udp_client.h)
static volatile voice_state_t current_state = STATE_IDLE;
static SemaphoreHandle_t state_mutex = NULL;

// RMS thresholds change later
#define RMS_THRESHOLD_NORMAL    100    // Normal speaking threshold
#define RMS_THRESHOLD_INTERRUPT 400   // Interrupt threshold

// Timing helpers
static inline int64_t get_time_ms(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// state handler function
static void set_voice_state(voice_state_t new_state)
{
    // wait forever for mutex
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    
    if (current_state != new_state) {
        ESP_LOGI(TAG, "🔄 State change: %s → %s", 
                 current_state == STATE_IDLE ? "IDLE" :
                 current_state == STATE_USER_SPEAKING ? "USER_SPEAKING" : "AI_SPEAKING",
                 new_state == STATE_IDLE ? "IDLE" :
                 new_state == STATE_USER_SPEAKING ? "USER_SPEAKING" : "AI_SPEAKING");
        
        voice_state_t old_state = current_state;
        current_state = new_state;
        
        // Handle state transitions
        switch (new_state) {
            // waiting for user to speak
            case STATE_IDLE:
                // Stop any active playback
                audio_playback_queue_stop();
                break;
            
            // user is speaking
            case STATE_USER_SPEAKING:
                // If interrupting AI, stop playback and send interrupt
                if (old_state == STATE_AI_SPEAKING) {
                    ESP_LOGI(TAG, "🛑 User interrupting AI - stopping playback");
                    audio_playback_queue_stop();
                    udp_send_interrupt_signal();
                }
                break;
                
            // AI is speaking
            case STATE_AI_SPEAKING:
                // Start playback queue
                audio_playback_queue_start();
                break;
        }
    }
    
    // give mutex when done for next state handling, ensures thread safe state handling
    xSemaphoreGive(state_mutex);
}

// simply a state getter function, thread safe due to mutex
static voice_state_t get_voice_state(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    voice_state_t state = current_state;
    xSemaphoreGive(state_mutex);
    return state;
}

// Enhanced audio monitoring task with state machine
static void voice_assistant_task(void *pvParameters)
{
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "🎙️ Voice Assistant Task Started");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "RMS Normal Threshold: %d", RMS_THRESHOLD_NORMAL);
    ESP_LOGI(TAG, "RMS Interrupt Threshold: %d", RMS_THRESHOLD_INTERRUPT);
    ESP_LOGI(TAG, "Silence Duration: %d ms", SILENCE_DURATION_MS);
    ESP_LOGI(TAG, "========================================\n");

    // Start I2S streaming once
    esp_err_t ret = audio_start_streaming();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start streaming");
        vTaskDelete(NULL);
        return;
    }

    uint8_t chunk_buffer[AUDIO_CHUNK_SIZE_OUTPUT]; // unsinged 8 bit int, simply a arr of audio
    int64_t silence_start = 0; // singed 64 bit int
    uint32_t sequence = 0; // unsinged 32 bit int, simply to track packets

    while (1) {
        // Capture one 40ms chunk
        size_t bytes_captured = 0;
        ret = audio_capture_chunk_to_buffer(chunk_buffer, &bytes_captured);

        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        // Calculate RMS
        int16_t *samples = (int16_t *)chunk_buffer;
        size_t sample_count = bytes_captured / 2;
        uint32_t rms = audio_calculate_rms(samples, sample_count);

        // Get current state
        voice_state_t state = get_voice_state();

        switch (state) {
            case STATE_IDLE:
                if (rms > RMS_THRESHOLD_NORMAL) {
                    ESP_LOGI(TAG, "\n🎙️ Audio detected (RMS=%lu) - USER_SPEAKING", rms);
                    set_voice_state(STATE_USER_SPEAKING);
                    silence_start = 0;
                    sequence = 0;

                    // Send this first chunk
                    udp_send_audio_packet(chunk_buffer, bytes_captured, sequence++);
                }
                break;

            case STATE_USER_SPEAKING:
                // Check for silence to return to IDLE
                if (rms < AUDIO_RMS_STOP_THRESHOLD) {
                    if (silence_start == 0) {
                        silence_start = get_time_ms();
                    } else if (get_time_ms() - silence_start > SILENCE_DURATION_MS) {
                        ESP_LOGI(TAG, "🔇 Silence detected - returning to IDLE");
                        ESP_LOGI(TAG, "Total chunks sent: %lu (%.2f seconds)\n",
                                 sequence, (float)sequence / 25.0f);
                        set_voice_state(STATE_IDLE);
                        silence_start = 0;
                        continue; // Don't send this chunk
                    }
                } else {
                    silence_start = 0; // Reset silence timer
                }

                // Send audio chunk
                udp_send_audio_packet(chunk_buffer, bytes_captured, sequence++);

                // Log every second
                if (sequence % 25 == 0) {
                    ESP_LOGI(TAG, "📤 Streaming: %lu chunks, RMS=%lu", sequence, rms);
                }
                break;

            case STATE_AI_SPEAKING:
                // Check for interrupt (high RMS during AI speech)
                if (rms > RMS_THRESHOLD_INTERRUPT) {
                    ESP_LOGI(TAG, "⚡ Interrupt detected (RMS=%lu) - USER_SPEAKING", rms);
                    set_voice_state(STATE_USER_SPEAKING);
                    sequence = 0;

                    // Send this interrupt chunk
                    udp_send_audio_packet(chunk_buffer, bytes_captured, sequence++);
                }
                // In AI_SPEAKING state, we don't send audio unless interrupting
                break;
        }

        // Natural 40ms pacing from I2S
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "ESP32-S3 Voice Assistant - State Machine Architecture");
    ESP_LOGI(TAG, "============================================================\n");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create state mutex
    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex!");
        return;
    }

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    ret = wifi_connect_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed!");
        return;
    }

    // Wait for WiFi
    while (!wifi_is_connected()) {
        ESP_LOGI(TAG, "Waiting for WiFi...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Initialize UDP with state callback
    ESP_LOGI(TAG, "Initializing UDP client...");
    ret = udp_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UDP initialization failed!");
        return;
    }

    // Register state callback for UDP
    udp_register_state_callback(set_voice_state);

    // Initialize Audio
    ESP_LOGI(TAG, "Initializing Audio...");
    ret = audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio initialization failed!");
        return;
    }

    // Initialize queue-based playback system
    ESP_LOGI(TAG, "Initializing queue-based playback...");
    ret = audio_playback_queue_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Queue initialization failed!");
        return;
    }

    // Quick tests - DISABLED
    // ESP_LOGI(TAG, "Testing microphone...");
    // audio_test_microphone_quick();

    // ESP_LOGI(TAG, "Testing speaker...");
    // audio_test_tx_with_known_sample();

    // ESP_LOGI(TAG, "Testing abrupt ending (verifying no repeating bug)...");
    // audio_test_abrupt_ending();

    // Create voice assistant task
    xTaskCreatePinnedToCore(voice_assistant_task, "voice_assist", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "\n============================================================");
    ESP_LOGI(TAG, "✅ Voice Assistant Ready!");
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "Architecture: State Machine with Interrupt Support");
    ESP_LOGI(TAG, "States: IDLE ↔ USER_SPEAKING ↔ AI_SPEAKING");
    ESP_LOGI(TAG, "Features:");
    ESP_LOGI(TAG, "  • Normal speaking: RMS > %d", RMS_THRESHOLD_NORMAL);
    ESP_LOGI(TAG, "  • Interrupt AI: RMS > %d", RMS_THRESHOLD_INTERRUPT);
    ESP_LOGI(TAG, "  • Bidirectional UDP communication");
    ESP_LOGI(TAG, "  • Queue-based audio playback");
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "Server: %s:%d", UDP_SERVER_IP, UDP_SERVER_PORT);
    ESP_LOGI(TAG, "============================================================\n");

    // LED indicator
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 3; i++) {
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "🎙️ System ready - State: IDLE\n");
}