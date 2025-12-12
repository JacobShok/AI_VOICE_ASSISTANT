const dgram = require('dgram');
const WebSocket = require('ws');

// Configuration
const OPENAI_API_KEY = "";
const LISTEN_PORT = 8080;
const LISTEN_HOST = '0.0.0.0';
const OPENAI_REALTIME_URL = 'wss://api.openai.com/v1/realtime?model=gpt-realtime-2025-08-28';

// Message types
const UDP_MSG_PLAY_AUDIO = 0x20;
const UDP_MSG_PLAY_AUDIO_LAST = 0x21;
const UDP_MSG_STATE_IDLE = 0x30;
const UDP_MSG_STATE_AI_SPEAKING = 0x32;
const UDP_MSG_INTERRUPT = 0x40;
const UDP_MSG_PLAYBACK_COMPLETE = 0x50;

// Audio rechunking buffer - converts variable OpenAI chunks to fixed 1920-byte chunks
class AudioRechunker {
    constructor(chunkSize = 1440) {  // 40ms @ 24kHz
        this.chunkSize = chunkSize;
        this.buffer = Buffer.alloc(0);
        this.sequence = 0;
    }

    // Add variable-sized data from OpenAI
    addData(audioBuffer) {
        this.buffer = Buffer.concat([this.buffer, audioBuffer]);
    }

    // Extract ONE fixed-size chunk for ESP32
    getChunk() {
        if (this.buffer.length >= this.chunkSize) {
            const chunk = this.buffer.slice(0, this.chunkSize);
            this.buffer = this.buffer.slice(this.chunkSize);
            return chunk;
        }
        return null;
    }

    // Get any remaining partial chunk (at end of response)
    flush() {
        const chunks = [];

        // Extract all full-sized chunks first
        while (this.buffer.length >= this.chunkSize) {
            const chunk = this.buffer.slice(0, this.chunkSize);
            this.buffer = this.buffer.slice(this.chunkSize);
            chunks.push(chunk);
        }

        // Return any remaining partial chunk (less than chunkSize)
        if (this.buffer.length > 0) {
            chunks.push(this.buffer);
            this.buffer = Buffer.alloc(0);
        }

        return chunks;
    }

    reset() {
        this.buffer = Buffer.alloc(0);
        this.sequence = 0;
    }
}

console.log('='.repeat(60));
console.log('ESP32S3 Realtime Bridge - VAD Mode');
console.log('Continuous Recording + OpenAI Server VAD');
console.log('='.repeat(60));

const udpServer = dgram.createSocket('udp4');
let espClient = null;
let openaiWs = null;

// Packet counters
let packetsReceived = 0;
let packetsSent = 0;

// Audio pipeline - WALKIE-TALKIE STYLE (no timing control)
const audioRechunker = new AudioRechunker(1440);
let deltaCount = 0;
let isFirstChunk = true;  // Track first chunk for state transition

// Helper: Send state message to ESP32
function sendStateToESP32(state) {
    if (!espClient) return;

    const packet = Buffer.alloc(1);
    packet[0] = state;

    udpServer.send(packet, espClient.port, espClient.address, (err) => {
        if (err) {
            console.error(`❌ Failed to send state: ${err.message}`);
        }
    });

    const stateName = state === UDP_MSG_STATE_IDLE ? 'IDLE' :
                     state === UDP_MSG_STATE_AI_SPEAKING ? 'AI_SPEAKING' : 'UNKNOWN';
    console.log(`📡 Sent state to ESP32: ${stateName}`);
}

// Helper: Send audio chunk to ESP32
function sendAudioChunkToESP32(audioBuffer, isLast = false) {
    if (!espClient) return;

    const MAX_AUDIO_SIZE = 1440;

    if (audioBuffer.length > MAX_AUDIO_SIZE) {
        console.warn(`⚠️ Chunk oversized (${audioBuffer.length} bytes), truncating to ${MAX_AUDIO_SIZE}`);
        audioBuffer = audioBuffer.slice(0, MAX_AUDIO_SIZE);
    }

    const packet = Buffer.alloc(5 + audioBuffer.length);
    packet[0] = isLast ? UDP_MSG_PLAY_AUDIO_LAST : UDP_MSG_PLAY_AUDIO;

    // CRITICAL FIX: Capture sequence number BEFORE incrementing to fix logging bug
    const currentSeq = audioRechunker.sequence;
    packet.writeUInt32LE(audioRechunker.sequence++, 1);
    audioBuffer.copy(packet, 5);

    udpServer.send(packet, espClient.port, espClient.address, (err) => {
        if (err) {
            console.error(`❌ Failed to send chunk #${currentSeq}: ${err.message}`);
        } else {
            packetsSent++;

            // IMPROVED LOGGING: Log every chunk to detect packet loss
            const marker = isLast ? ' [LAST]' : '';
            if (currentSeq % 10 === 0 || isLast) {
                console.log(`📤 Sent chunk #${currentSeq} to ESP32 (${audioBuffer.length} bytes)${marker}`);
            }
        }
    });
}

// Send chunks with rate limiting to prevent UDP packet loss
async function blastAvailableChunks() {
    let chunksSent = 0;
    const startSeq = audioRechunker.sequence;

    // Extract and send chunks with a tiny delay to prevent overwhelming ESP32
    while (audioRechunker.buffer.length >= audioRechunker.chunkSize) {
        const chunk = audioRechunker.getChunk();
        if (chunk) {
            sendAudioChunkToESP32(chunk, false);
            chunksSent++;

            // CRITICAL FIX: Add 5ms delay AFTER EVERY packet to prevent UDP buffer overflow
            // WiFi + FreeRTOS on ESP32 needs significant time between packets
            // At 40ms per chunk playback, 5ms per chunk = 12.5% overhead (necessary for reliability)
            // This reduces send rate from 500pkt/s to 200pkt/s - within ESP32 WiFi capacity
            await new Promise(resolve => setTimeout(resolve, 30));
        }
    }

    if (chunksSent > 0) {
        console.log(`⚡ BLASTED ${chunksSent} chunks (#${startSeq}-#${audioRechunker.sequence - 1}) - ${chunksSent * 1440} bytes`);
    }

    return chunksSent;
}

// Handle audio completion - send any remaining partial chunk
async function finishAudioStream() {
    console.log('🎵 Audio response completed');

    // Flush any remaining chunks
    const remainingChunks = audioRechunker.flush();

    if (remainingChunks.length > 0) {
        console.log(`📤 Flushing ${remainingChunks.length} remaining chunks`);

        // Send remaining chunks with rate limiting to prevent packet loss
        for (let i = 0; i < remainingChunks.length; i++) {
            const isLast = (i === remainingChunks.length - 1);
            sendAudioChunkToESP32(remainingChunks[i], isLast);

            // Add 5ms delay after every flush chunk to prevent packet loss (matching blastAvailableChunks)
            if (i < remainingChunks.length - 1) {
                await new Promise(resolve => setTimeout(resolve, 5));
            }
        }
    } else {
        // CRITICAL FIX: ESP32 rejects empty packets
        // Instead, send a minimal silent packet (8 samples = 16 bytes of PCM16 silence)
        console.log('📤 No remaining chunks, sending silent LAST packet');
        const silentPacket = Buffer.alloc(16, 0);  // 16 bytes of silence (8 samples @ 16-bit)
        sendAudioChunkToESP32(silentPacket, true);
    }

    console.log('⏳ Waiting for ESP32 playback complete...');

    // Fallback timeout - increased to 3 minutes to handle longer responses
    // At 40ms per chunk, 180s = 4500 chunks = ~648KB of audio
    const idleTimeout = setTimeout(() => {
        console.log('⏰ Timeout (3min) - sending IDLE');
        sendStateToESP32(UDP_MSG_STATE_IDLE);
        audioRechunker.reset();
        isFirstChunk = true;
    }, 180000);

    if (!global.playbackTimeouts) global.playbackTimeouts = new Map();
    global.playbackTimeouts.set('current', idleTimeout);
}

// Stop pipeline (used for interrupts)
function stopAudioPipeline() {
    console.log('🛑 Stopping audio pipeline (interrupted)');
    audioRechunker.reset();
    isFirstChunk = true;
}

function connectToOpenAI() {
    console.log('\n🔗 Connecting to OpenAI Realtime API...');

    openaiWs = new WebSocket(OPENAI_REALTIME_URL, {
        headers: {
            'Authorization': `Bearer ${OPENAI_API_KEY}`,
            'OpenAI-Beta': 'realtime=v1'
        }
    });

    openaiWs.on('open', () => {
        console.log('✅ Connected to OpenAI Realtime API');
    });

    openaiWs.on('message', (data) => {
        handleOpenAIMessage(data);
    });

    openaiWs.on('close', (code, reason) => {
        console.log(`❌ OpenAI connection closed: ${code} ${reason}`);
        openaiWs = null;
        setTimeout(() => {
            console.log('🔄 Reconnecting to OpenAI...');
            connectToOpenAI();
        }, 5000);
    });

    openaiWs.on('error', (error) => {
        console.error('❌ OpenAI WebSocket error:', error.message);
    });
}

function configureSession() {
    const sessionConfig = {
        type: 'session.update',
        session: {
            modalities: ['text', 'audio'],
            instructions: 'CRITICAL RULES SYSTEM WILL FAIL IF NOT FOLLOWED EXACTLY: You are a real time Farsi to English voice translator. Your role is ESSENTIAL and must be performed precisely. YOUR CORE FUNCTION: 1. Listen for Farsi speech from OTHER PEOPLE not me 2. Translate their Farsi into English for me 3. Suggest a relevant Farsi response I can say back CRITICAL: DO NOT translate when I repeat the Farsi phrases you just taught me. You must remember each suggestion you give me and IGNORE IT when I say it back. If I say something in Farsi that is VERY SIMILAR to your suggestion even if not exactly the same you must recognize it as me speaking and stay silent. Use reasoning to determine if the words are close enough to what you suggested. Only translate NEW Farsi speech from the other person. When you need to stay silent simply say staying silent and nothing else. RESPONSE FORMAT use this exact structure every time: Translation: English translation of what they said Suggestion: Keep this SHORT with minimal filler words. Format is English phrase then Farsi phrase. Examples: yes bale, no thank you na moteshakeram, I want a latte man ye latte mikham, hot coffee ghahve dagh. CONTEXT: I am an English speaker in Iran trying to order coffee. Everyone around me speaks Farsi. I need help understanding them and responding appropriately. My goal is to successfully complete a coffee order. EXAMPLE FLOW: Barista says chi mikhay? You respond Translation: What do you want? Suggestion: I want a latte man ye latte mikham. I then say man ye latte mikham lotfan. You simply say staying silent because this is very similar to what you taught me. Barista says khameh mikhay? You respond Translation: Do you want cream? Suggestion: yes with cream bale ba khameh. REMEMBER: Track every phrase you teach me and stay silent when I use it or something very similar. Use reasoning to identify when I am speaking versus when the other person is speaking. Only translate new Farsi from others. When staying silent only say staying silent. Keep suggestions SHORT no filler words just English then Farsi.',
            voice: 'sage',
            input_audio_format: 'pcm16',
            output_audio_format: 'pcm16',
            input_audio_transcription: {
                model: 'whisper-1'
            },
            turn_detection: {
                type: 'server_vad',
                threshold: 0.01,
                prefix_padding_ms: 300,
                silence_duration_ms: 10,
                create_response: true
            },
            temperature: 0.8,
            max_response_output_tokens: 4096
        }
    };

    console.log('⚙️ Configuring OpenAI session with server_vad...');
    openaiWs.send(JSON.stringify(sessionConfig));
}

function handleOpenAIMessage(data) {
    try {
        const message = JSON.parse(data.toString());

        switch (message.type) {
            case 'session.created':
                console.log('✅ OpenAI session created');
                configureSession();
                break;

            case 'session.updated':
                console.log('✅ OpenAI session configured with VAD');
                console.log('🎤 Ready to receive audio from ESP32\n');
                break;

            case 'input_audio_buffer.speech_started':
                console.log('🎙️ OpenAI VAD: Speech detected');
                break;

            case 'input_audio_buffer.speech_stopped':
                console.log('🤐 OpenAI VAD: Speech ended (auto-committing)');
                break;

            case 'input_audio_buffer.committed':
                console.log('✅ Audio buffer committed by VAD');
                break;

            case 'conversation.item.created':
                console.log('📝 Conversation item created');
                break;

            case 'response.created':
                console.log('🤖 Response generation started');
                // Reset state for new response
                audioRechunker.reset();
                isFirstChunk = true;
                deltaCount = 0;
                break;

            case 'response.output_item.added':
                console.log('📝 Output item added to response');
                break;

            case 'response.audio.delta':
                if (message.delta) {
                    const audioBuffer = Buffer.from(message.delta, 'base64');

                    // Log OpenAI data receipt (every 5th packet)
                    if (++deltaCount % 5 === 0) {
                        console.log(`📥 OpenAI delta #${deltaCount}: ${audioBuffer.length} bytes`);
                    }

                    // WALKIE-TALKIE APPROACH: Add data and blast immediately
                    audioRechunker.addData(audioBuffer);

                    // Send AI_SPEAKING state on first chunk
                    if (isFirstChunk) {
                        console.log('🔊 First audio delta - starting stream');
                        sendStateToESP32(UDP_MSG_STATE_AI_SPEAKING);
                        isFirstChunk = false;
                    }

                    // Blast all available chunks immediately (no timer, no delay)
                    blastAvailableChunks();
                }
                break;

            case 'response.audio.done':
                finishAudioStream();
                break;

            case 'response.audio_transcript.delta':
                if (message.delta) {
                    process.stdout.write(message.delta);
                }
                break;

            case 'response.audio_transcript.done':
                if (message.transcript) {
                    console.log(`\n🤖 Teddy said: "${message.transcript}"`);
                }
                break;

            case 'response.content_part.done':
                console.log('✅ Content part complete');
                break;

            case 'response.output_item.done':
                console.log('✅ Output item complete');
                break;

            case 'response.done':
                console.log('✅ Response fully complete');
                console.log('---\nReady for next interaction\n');
                break;

            case 'response.cancelled':
                console.log('⚠️ Response interrupted by user');
                stopAudioPipeline();
                break;

            case 'conversation.item.input_audio_transcription.completed':
                if (message.transcript) {
                    console.log(`📝 User said: "${message.transcript}"`);
                }
                break;

            case 'error':
                console.error('❌ OpenAI error:', JSON.stringify(message.error, null, 2));
                break;

            case 'rate_limits.updated':
                break;

            default:
                break;
        }
    } catch (error) {
        console.error('Error processing OpenAI message:', error);
    }
}

// Handle interrupt signal from ESP32
function handleInterruptFromESP32() {
    console.log('⚡ INTERRUPT received from ESP32');

    stopAudioPipeline();

    if (openaiWs && openaiWs.readyState === WebSocket.OPEN) {
        openaiWs.send(JSON.stringify({ type: 'response.cancel' }));
        openaiWs.send(JSON.stringify({ type: 'input_audio_buffer.clear' }));
        console.log('📡 Sent cancel & clear to OpenAI');
    }
}

// UDP message handler
udpServer.on('message', (msg, rinfo) => {
    packetsReceived++;

    // Track ESP32 client
    if (!espClient || espClient.address !== rinfo.address) {
        espClient = { address: rinfo.address, port: rinfo.port };
        console.log(`\n📱 ESP32 connected: ${rinfo.address}:${rinfo.port}`);
    }

    // Check for interrupt message
    if (msg.length === 1 && msg[0] === UDP_MSG_INTERRUPT) {
        handleInterruptFromESP32();
        return;
    }

    // Check for playback complete message
    if (msg.length === 1 && msg[0] === UDP_MSG_PLAYBACK_COMPLETE) {
        console.log('✅ Received PLAYBACK_COMPLETE from ESP32');

        // Cancel timeout
        if (global.playbackTimeouts && global.playbackTimeouts.has('current')) {
            clearTimeout(global.playbackTimeouts.get('current'));
            global.playbackTimeouts.delete('current');
        }

        // Send IDLE state
        sendStateToESP32(UDP_MSG_STATE_IDLE);
        return;
    }

    // Audio packet: [4-byte sequence][audio data]
    if (msg.length >= 4) {
        const sequence = msg.readUInt32LE(0);
        const audioData = msg.slice(4);

        // Forward to OpenAI
        if (openaiWs && openaiWs.readyState === WebSocket.OPEN) {
            const base64Audio = audioData.toString('base64');
            openaiWs.send(JSON.stringify({
                type: 'input_audio_buffer.append',
                audio: base64Audio
            }));

            if (sequence % 25 === 0) {
                console.log(`📥 Packet #${sequence} → OpenAI (${audioData.length} bytes)`);
            }
        } else {
            if (packetsReceived % 100 === 0) {
                console.warn('⚠️ OpenAI not connected, dropping packets');
            }
        }
    }
});

udpServer.on('listening', () => {
    const address = udpServer.address();
    console.log(`\n✅ UDP server listening: ${address.address}:${address.port}`);
    console.log('Waiting for ESP32 connection...\n');
    connectToOpenAI();
});

udpServer.on('error', (err) => {
    console.error('❌ UDP server error:', err);
});

udpServer.bind(LISTEN_PORT, LISTEN_HOST);

// Statistics logging
setInterval(() => {
    if (packetsReceived > 0 || packetsSent > 0) {
        console.log(`📊 Stats: ${packetsReceived} received, ${packetsSent} sent`);
    }
}, 30000);

// Graceful shutdown
process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down...');
    if (openaiWs) {
        openaiWs.close();
    }
    udpServer.close(() => {
        console.log('👋 Goodbye!');
        process.exit(0);
    });
});

console.log('🚀 Bridge server running in VAD mode!\n');