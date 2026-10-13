#pragma once
#define AGENT_SESSION_INIT_URL "https://api.test.ai-ivrs.bonrix.in/new/api/session/init"
#define AGENT_WEBSOCKET_ORIGIN "https://test.ai-ivrs.bonrix.in"
#define AGENT_WEBSITE_NAME "demo"
#define AGENT_LANGUAGE "English"
#define AGENT_CONTEXT_FILE ""
// Configure upload and playback independently to match the backend contract.
#define AGENT_INPUT_SAMPLE_RATE 24000
// Capture rate must be an integer multiple of the upload rate.
#define AGENT_MIC_CAPTURE_SAMPLE_RATE 24000
// -1 detects the active I2S slot; 0 forces left (L/R=GND), 1 forces right.
#define AGENT_MIC_SLOT 0
#define AGENT_MIC_GAIN 4
#if AGENT_MIC_SLOT < -1 || AGENT_MIC_SLOT > 1
#error "AGENT_MIC_SLOT must be -1 (auto), 0 (left), or 1 (right)"
#endif
#if AGENT_MIC_GAIN < 1 || AGENT_MIC_GAIN > 128
#error "AGENT_MIC_GAIN must be between 1 and 128"
#endif
// The working session endpoint's browser reference uses PCM16 mono at 24 kHz.
#define AGENT_OUTPUT_SAMPLE_RATE 24000
// Stream while the response is arriving instead of waiting for response.done.
// A short jitter buffer absorbs bursty delivery; underruns retain queued PCM
// and refill before resuming. Short completed replies play immediately.
#define AGENT_BUFFER_COMPLETE_REPLY 0
#define AGENT_SPEAKER_START_BUFFER_MS 1200
#define AGENT_SPEAKER_REBUFFER_MS 1400
// Bound initial waiting even when the backend delivers slower than real time.
#define AGENT_SPEAKER_START_MAX_WAIT_MS 1800
// Session identity matching the supplied working browser example.
// JSON encoding safely escapes these identity fields.
#define AGENT_USER_NAME "Jatin"
#define AGENT_USER_EMAIL "jatinmakwana211@gmail.com"
#define AGENT_USER_MOBILE "8128416007"
