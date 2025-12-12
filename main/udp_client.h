#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Server configuration
#define UDP_SERVER_IP "put a ip"
#define UDP_SERVER_PORT 8080
#define UDP_LOCAL_PORT 3333

// Message types for new architecture
typedef enum {
    UDP_MSG_AUDIO_DATA = 0x10,      // Audio data from ESP32
    UDP_MSG_PLAY_AUDIO = 0x20,      // Audio to play
    UDP_MSG_PLAY_AUDIO_LAST = 0x21, // ADD THIS - Last audio chunk
    UDP_MSG_STATE_IDLE = 0x30,      // State: IDLE
    UDP_MSG_STATE_USER_SPEAKING = 0x31,  // State: USER_SPEAKING
    UDP_MSG_STATE_AI_SPEAKING = 0x32,    // State: AI_SPEAKING
    UDP_MSG_INTERRUPT = 0x40,       // User interrupt signal
    UDP_MSG_PLAYBACK_COMPLETE = 0x50, // ADD THIS - Playback completed
    UDP_MSG_ERROR = 0xFF
} udp_message_type_t;

// Voice state enum (matching main.c)
typedef enum {
    STATE_IDLE,
    STATE_USER_SPEAKING,
    STATE_AI_SPEAKING
} voice_state_t;

// Maximum UDP payload size
#define UDP_MAX_PAYLOAD 2000

// Function prototypes
esp_err_t udp_client_init(void);
esp_err_t udp_send_audio_packet(const uint8_t *audio_data, size_t audio_len, uint32_t sequence);
esp_err_t udp_send_interrupt_signal(void);
esp_err_t udp_send_playback_complete(void);
bool udp_client_is_ready(void);
uint32_t udp_get_packets_sent(void);
uint32_t udp_get_packets_received(void);
void udp_client_deinit(void);
void udp_register_state_callback(void (*callback)(voice_state_t state));

#endif // UDP_CLIENT_H