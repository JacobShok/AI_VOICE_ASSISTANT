#include "udp_client.h"
#include "audio_handler.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "UDP_CLIENT";

// UDP socket
static int udp_socket = -1;
static struct sockaddr_in server_addr;
static bool is_initialized = false;

// Statistics
static uint32_t packets_sent = 0;
static uint32_t packets_received = 0;
static uint32_t last_received_seq = 0;
static uint32_t packets_lost = 0;

// State callback
static void (*state_change_callback)(voice_state_t state) = NULL;

// Receive buffer
#define RX_BUFFER_SIZE 2048
static uint8_t rx_buffer[RX_BUFFER_SIZE];

// UDP receive task - handles incoming audio and state changes
static void udp_receive_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UDP receive task started");
    
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    
    while (udp_socket >= 0) {
        int len = recvfrom(udp_socket, rx_buffer, RX_BUFFER_SIZE, 0,
                          (struct sockaddr *)&source_addr, &socklen);
        
        if (len > 0) {
            packets_received++;
            
            // Check message type
            uint8_t msg_type = rx_buffer[0];
            
            switch (msg_type) {
                case UDP_MSG_PLAY_AUDIO:
                    if (len >= 5) {
                        uint32_t seq = *(uint32_t *)&rx_buffer[1];
                        uint8_t *audio_data = &rx_buffer[5];
                        size_t audio_len = len - 5;

                        // PACKET LOSS DETECTION: Check for sequence number gaps
                        if (seq > 0 && last_received_seq > 0 && seq != last_received_seq + 1) {
                            uint32_t gap = seq - last_received_seq - 1;
                            packets_lost += gap;
                            ESP_LOGW(TAG, "⚠️ PACKET LOSS: Expected seq #%lu, got #%lu (lost %lu packets, total lost: %lu)",
                                     last_received_seq + 1, seq, gap, packets_lost);
                        }
                        last_received_seq = seq;

                        // Validate packet size
                        if (audio_len > 1440) {
                            ESP_LOGW(TAG, "⚠️ Received oversized packet #%lu: %zu bytes (max 1440), truncating", seq, audio_len);
                            audio_len = 1440;
                        }
                        if (audio_len == 0) {
                            ESP_LOGW(TAG, "⚠️ Received empty packet #%lu, skipping", seq);
                            break;
                        }

                        // CRITICAL FIX: Do NOT scale here - it blocks UDP receive and causes packet loss!
                        // Volume scaling is done in the playback task instead
                        audio_playback_queue_push(audio_data, audio_len, seq, false);
                    }
                    break;

                case UDP_MSG_PLAY_AUDIO_LAST:
                    if (len >= 5) {
                        uint32_t seq = *(uint32_t *)&rx_buffer[1];
                        uint8_t *audio_data = &rx_buffer[5];
                        size_t audio_len = len - 5;

                        // PACKET LOSS DETECTION: Check for sequence number gaps before LAST
                        if (seq > 0 && last_received_seq > 0 && seq != last_received_seq + 1) {
                            uint32_t gap = seq - last_received_seq - 1;
                            packets_lost += gap;
                            ESP_LOGW(TAG, "⚠️ PACKET LOSS BEFORE LAST: Expected seq #%lu, got #%lu (lost %lu packets, total lost: %lu)",
                                     last_received_seq + 1, seq, gap, packets_lost);
                        }
                        last_received_seq = seq;

                        // Validate packet size
                        if (audio_len > 1440) {
                            ESP_LOGW(TAG, "⚠️ Received oversized LAST packet #%lu: %zu bytes (max 1440), truncating", seq, audio_len);
                            audio_len = 1440;
                        }
                        if (audio_len == 0) {
                            ESP_LOGW(TAG, "⚠️ Received empty LAST packet #%lu, skipping", seq);
                            break;
                        }

                        ESP_LOGI(TAG, "📥 Received LAST chunk #%lu (%zu bytes) - Total packets lost this session: %lu", seq, audio_len, packets_lost);

                        // CRITICAL FIX: Do NOT scale here - it blocks UDP receive and causes packet loss!
                        // Volume scaling is done in the playback task instead
                        audio_playback_queue_push(audio_data, audio_len, seq, true);

                        // Reset packet loss tracking for next session
                        last_received_seq = 0;
                        packets_lost = 0;
                    }
                    break;
                    
                case UDP_MSG_STATE_IDLE:
                    ESP_LOGI(TAG, "📡 Received: STATE_IDLE");
                    if (state_change_callback) {
                        state_change_callback(STATE_IDLE);
                    }
                    break;
                    
                case UDP_MSG_STATE_AI_SPEAKING:
                    ESP_LOGI(TAG, "📡 Received: STATE_AI_SPEAKING");
                    if (state_change_callback) {
                        state_change_callback(STATE_AI_SPEAKING);
                    }
                    break;
                    
                default:
                    ESP_LOGD(TAG, "Unknown message type: 0x%02x", msg_type);
                    break;
            }
        } else if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    ESP_LOGI(TAG, "UDP receive task exiting");
    vTaskDelete(NULL);
}

esp_err_t udp_client_init(void)
{
    ESP_LOGI(TAG, "Initializing UDP client...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "UDP client already initialized");
        return ESP_OK;
    }
    
    // Create UDP socket
    udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;  // 500ms
    setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // CRITICAL FIX: Increase UDP receive buffer to prevent packet loss
    // With 1440-byte chunks sent every 40ms, we need buffer for ~100 chunks = 144KB
    int rx_buffer_size = 256 * 1024;  // 256KB receive buffer
    if (setsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF, &rx_buffer_size, sizeof(rx_buffer_size)) < 0) {
        ESP_LOGW(TAG, "Failed to set SO_RCVBUF: errno %d", errno);
    } else {
        ESP_LOGI(TAG, "📦 UDP receive buffer set to %d KB", rx_buffer_size / 1024);
    }
    
    // Bind to local port
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(UDP_LOCAL_PORT);
    
    int err = bind(udp_socket, (struct sockaddr *)&local_addr, sizeof(local_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: errno %d", errno);
        close(udp_socket);
        udp_socket = -1;
        return ESP_FAIL;
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_SERVER_PORT);
    inet_pton(AF_INET, UDP_SERVER_IP, &server_addr.sin_addr);
    
    ESP_LOGI(TAG, "📡 Server: %s:%d", UDP_SERVER_IP, UDP_SERVER_PORT);
    
    // Start receive task
    xTaskCreate(udp_receive_task, "udp_rx", 4096, NULL, 5, NULL);
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ UDP client initialized");
    
    return ESP_OK;
}

esp_err_t udp_send_audio_packet(const uint8_t *audio_data, size_t audio_len, uint32_t sequence)
{
    if (!is_initialized || udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Build packet: [sequence][audio_data]
    size_t packet_size = sizeof(uint32_t) + audio_len;
    uint8_t *packet = malloc(packet_size);
    if (!packet) {
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(packet, &sequence, sizeof(uint32_t));
    memcpy(packet + sizeof(uint32_t), audio_data, audio_len);
    
    int sent = sendto(udp_socket, packet, packet_size, 0,
                     (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    free(packet);
    
    if (sent < 0) {
        ESP_LOGE(TAG, "sendto failed: errno %d", errno);
        return ESP_FAIL;
    }
    
    packets_sent++;
    
    // Log every 25 packets
    if (sequence % 25 == 0) {
        ESP_LOGI(TAG, "📤 Sent packet #%lu (%d bytes)", sequence, sent);
    }
    
    return ESP_OK;
}

esp_err_t udp_send_interrupt_signal(void)
{
    if (!is_initialized || udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t interrupt_msg = UDP_MSG_INTERRUPT;

    int sent = sendto(udp_socket, &interrupt_msg, 1, 0,
                     (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send interrupt: errno %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "⚡ Sent interrupt signal to server");
    return ESP_OK;
}

esp_err_t udp_send_playback_complete(void)
{
    if (!is_initialized || udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Define playback complete message type (you can add this to the header)
    uint8_t complete_msg = 0x50;  // New message type for playback complete

    int sent = sendto(udp_socket, &complete_msg, 1, 0,
                     (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send playback complete: errno %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ Sent playback complete signal to server");
    return ESP_OK;
}



void udp_register_state_callback(void (*callback)(voice_state_t state))
{
    state_change_callback = callback;
    ESP_LOGI(TAG, "State callback registered");
}

bool udp_client_is_ready(void)
{
    return is_initialized && udp_socket >= 0;
}

uint32_t udp_get_packets_sent(void)
{
    return packets_sent;
}

uint32_t udp_get_packets_received(void)
{
    return packets_received;
}

void udp_client_deinit(void)
{
    is_initialized = false;
    
    if (udp_socket >= 0) {
        close(udp_socket);
        udp_socket = -1;
    }
    
    packets_sent = 0;
    packets_received = 0;
    state_change_callback = NULL;
    
    ESP_LOGI(TAG, "UDP client deinitialized");
}