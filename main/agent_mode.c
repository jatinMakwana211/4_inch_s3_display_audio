#include "agent_mode.h"
#include "audio_player.h"
#include "wifi_setup.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "agent_config.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include "mbedtls/base64.h"

#define FRAMES (AGENT_INPUT_SAMPLE_RATE / 50)
#define CAPTURE_FRAMES (AGENT_MIC_CAPTURE_SAMPLE_RATE / 50)
#define MIC_DECIMATION (AGENT_MIC_CAPTURE_SAMPLE_RATE / AGENT_INPUT_SAMPLE_RATE)
#if AGENT_MIC_CAPTURE_SAMPLE_RATE % AGENT_INPUT_SAMPLE_RATE != 0
#error "Microphone capture rate must be an integer multiple of upload rate"
#endif
#define SPEAKER_FRAMES (AGENT_OUTPUT_SAMPLE_RATE / 50)
#define SEND_FRAMES (FRAMES * 4)
#define MESSAGE_MAX (256 * 1024)
#define SPEAKER_BUFFER_BYTES (2 * 1024 * 1024)
#define SPEAKER_ECHO_TAIL_MS 300
#define SPEAKER_PREROLL_BYTES (AGENT_OUTPUT_SAMPLE_RATE * 2 * AGENT_SPEAKER_START_BUFFER_MS / 1000)
#define SPEAKER_REBUFFER_BYTES (AGENT_OUTPUT_SAMPLE_RATE * 2 * AGENT_SPEAKER_REBUFFER_MS / 1000)
// Leave room for a large incoming chunk when a long reply must start draining.
#define SPEAKER_HIGH_WATER_BYTES (SPEAKER_BUFFER_BYTES * 3 / 4)
static atomic_bool enabled;
static SemaphoreHandle_t lock;
static StreamBufferHandle_t speaker;
static agent_snapshot_t state;
static atomic_uint audio_epoch;
static atomic_uint mode_epoch;
static atomic_uint speaker_quiet_until_ms;
static atomic_bool assistant_receiving;
// ESP-IDF 5.4 exports this accessor from tcp_transport's private interface.
// Keep the declaration local rather than adding private IDF include paths.
extern int esp_transport_get_socket(esp_transport_handle_t t);
static StaticStreamBuffer_t speaker_control;
static volatile bool mic_overflow;
static bool new_response=true;
static bool new_input=true;
static const char *TAG="BONRIX_AGENT";
typedef struct { size_t bytes; int16_t pcm[FRAMES]; } mic_block_t;
typedef struct {
    int16_t pcm[SEND_FRAMES];
    unsigned char base64[SEND_FRAMES*2*4/3+4];
    char packet[SEND_FRAMES*2*4/3+128];
} send_batch_t;
static QueueHandle_t microphone;
static StaticQueue_t microphone_control;
static atomic_int mic_error;
static unsigned reply_audio_chunks,reply_text_chunks,suppressed_audio_chunks;
static size_t reply_audio_bytes;
static int64_t reply_audio_started;
static int64_t reply_audio_last;
static unsigned reply_audio_max_gap_ms;
static bool on_mic_overflow(i2s_chan_handle_t channel,i2s_event_data_t *event,void *ctx)
{
    mic_overflow=true;return false;
}

static void status(const char *text)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if(strcmp(state.status,text)) {snprintf(state.status,sizeof(state.status),"%s",text); ++state.revision;}
    xSemaphoreGive(lock);
}
void agent_mode_snapshot(agent_snapshot_t *out)
{
    if (!lock) { memset(out,0,sizeof(*out)); return; }
    xSemaphoreTake(lock,portMAX_DELAY); *out=state; xSemaphoreGive(lock);
}
void agent_mode_enable(bool on)
{
    if(atomic_exchange(&enabled,on)!=on) {
        atomic_fetch_add(&mode_epoch,1);
        atomic_fetch_add(&audio_epoch,1);
    }
}
static void flush_audio(void) { atomic_fetch_add(&audio_epoch,1); }
static bool microphone_paused(void)
{
    unsigned until=atomic_load(&speaker_quiet_until_ms);
    return atomic_load(&assistant_receiving) || xStreamBufferBytesAvailable(speaker)>0 ||
        (until && (int32_t)(until-(unsigned)(esp_timer_get_time()/1000))>0);
}
static bool send_text(esp_transport_handle_t ws,const char *s)
{
    int n=strlen(s);
    // ESP-IDF 5.4 masks the payload in place, even though the API takes const.
    char *writable=strdup(s);if(!writable)return false;
    int sent=esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,writable,n,1000);
    free(writable);
    if(sent!=n)ESP_LOGE(TAG,"WebSocket send failed: sent=%d expected=%d errno=%d",sent,n,errno);
    return sent==n;
}
static const char *field(cJSON *obj,const char *key)
{
    cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);
    return cJSON_IsString(v)?v->valuestring:"";
}
static void message(const char *text)
{
    cJSON *json=cJSON_Parse(text); if(!json){ESP_LOGW(TAG,"Invalid JSON received");return;}
    const char *type=field(json,"type");
    if(strcmp(type,"response.audio.delta") && strcmp(type,"response.audio_transcript.delta"))
        ESP_LOGI(TAG,"Server event: %.100s",type);
    if (!strcmp(type,"conversation.item.input_audio_transcription.completed") ||
        !strcmp(type,"conversation.item.input_audio_transcription.delta")) {
        bool complete=!strcmp(type,"conversation.item.input_audio_transcription.completed");
        const char *t=field(json,complete?"transcript":"delta");
        xSemaphoreTake(lock,portMAX_DELAY);
        if(complete)snprintf(state.user,sizeof(state.user),"%s",t);
        else {
            if(new_input){state.user[0]=0;new_input=false;}
            strncat(state.user,t,sizeof(state.user)-strlen(state.user)-1);
        }
        if(complete)new_input=true;
        ++state.revision; xSemaphoreGive(lock);
    } else if (!strcmp(type,"input_audio_buffer.speech_started")) {
        // This client is half duplex. Delayed VAD/echo must not flush a reply
        // that is still playing, or request a competing response cancellation.
        ESP_LOGD(TAG,"Speech-start notification: preserving speaker playback");
    } else if (!strcmp(type,"response.audio.delta")) {
        ++reply_audio_chunks;
        int64_t arrived=esp_timer_get_time();
        unsigned gap_ms=reply_audio_last?(unsigned)((arrived-reply_audio_last)/1000):0;
        if(reply_audio_chunks==1)reply_audio_started=arrived;
        reply_audio_last=arrived;
        if(gap_ms>reply_audio_max_gap_ms)reply_audio_max_gap_ms=gap_ms;
        agent_snapshot_t snap; agent_mode_snapshot(&snap);
        if(reply_audio_chunks==1)ESP_LOGI(TAG,"Assistant audio received: awake=%d",snap.awake);
        if(snap.awake) {
            const char *b64=field(json,"delta"); size_t count=0,capacity=strlen(b64)*3/4+3;
            unsigned char *pcm=heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            int decode_err=pcm?mbedtls_base64_decode(pcm,capacity,&count,(const unsigned char *)b64,strlen(b64)):-1;
            if(!decode_err && !(count&1)) {
                reply_audio_bytes+=count;
                ESP_LOGI(TAG,"PCM arrival: chunk=%u audio_ms=%u gap_ms=%u queued_ms=%u",reply_audio_chunks,
                         (unsigned)(count*1000/(AGENT_OUTPUT_SAMPLE_RATE*2)),gap_ms,
                         (unsigned)(xStreamBufferBytesAvailable(speaker)*1000/(AGENT_OUTPUT_SAMPLE_RATE*2)));
                if(count)atomic_store(&assistant_receiving,true);
                // Apply backpressure while playback makes room. Large server
                // bursts must not remove words from the middle of a reply.
                unsigned epoch=atomic_load(&audio_epoch);
                size_t offset=0;
                while(offset<count && atomic_load(&enabled) && epoch==atomic_load(&audio_epoch)) {
                    // Send complete samples even when only part of the burst fits.
                    size_t space=xStreamBufferSpacesAvailable(speaker)&~(size_t)1;
                    size_t bytes=count-offset;
                    if(bytes>space)bytes=space;
                    if(bytes)offset+=xStreamBufferSend(speaker,pcm+offset,bytes,0);
                    else vTaskDelay(pdMS_TO_TICKS(5));
                }
            } else ESP_LOGE(TAG,"Assistant PCM decode failed: error=%d bytes=%u",decode_err,(unsigned)count);
            free(pcm);
        } else ++suppressed_audio_chunks;
    } else if (!strcmp(type,"response.audio.done")) {
        // Audio completion can precede (or arrive without) response.done.
        // Buffered playback and the acoustic tail still gate microphone uploads.
        atomic_store(&assistant_receiving,false);
    } else if (!strcmp(type,"response.audio_transcript.delta") ||
               !strcmp(type,"response.audio_transcript.done")) {
        bool done=!strcmp(type,"response.audio_transcript.done");
        ++reply_text_chunks;
        xSemaphoreTake(lock,portMAX_DELAY);
        if(state.awake) {
            if(new_response){state.assistant[0]=0;new_response=false;}
            if(done && *field(json,"transcript"))snprintf(state.assistant,sizeof(state.assistant),"%s",field(json,"transcript"));
            else if(!done)strncat(state.assistant,field(json,"delta"),sizeof(state.assistant)-strlen(state.assistant)-1);
            ++state.revision;
        }
        xSemaphoreGive(lock);
    } else if (!strcmp(type,"response.created")) {
        new_response=true;
        xSemaphoreTake(lock,portMAX_DELAY); state.assistant[0]=0; ++state.revision; xSemaphoreGive(lock);
    } else if (!strcmp(type,"response.done")) {
        atomic_store(&assistant_receiving,false);
        agent_snapshot_t snap;agent_mode_snapshot(&snap);
        ESP_LOGI(TAG,"Reply finished: awake=%d audio=%u text=%u audio_suppressed=%u PCM=%u bytes audio_ms=%u receive_ms=%u",snap.awake,reply_audio_chunks,reply_text_chunks,suppressed_audio_chunks,
                 (unsigned)reply_audio_bytes,(unsigned)(reply_audio_bytes*1000/(AGENT_OUTPUT_SAMPLE_RATE*2)),
                 reply_audio_started?(unsigned)((esp_timer_get_time()-reply_audio_started)/1000):0);
        unsigned audio_ms=(unsigned)(reply_audio_bytes*1000/(AGENT_OUTPUT_SAMPLE_RATE*2));
        unsigned receive_ms=reply_audio_started?(unsigned)((esp_timer_get_time()-reply_audio_started)/1000):0;
        if(audio_ms && receive_ms>audio_ms) {
            ESP_LOGW(TAG,"PCM delivery below playback rate: delivery=%u%% max_chunk_gap_ms=%u; verify backend PCM rate and server/network pacing",
                     (unsigned)((uint64_t)audio_ms*100/receive_ms),reply_audio_max_gap_ms);
        }
        reply_audio_chunks=reply_text_chunks=suppressed_audio_chunks=0;
        reply_audio_bytes=0;reply_audio_started=0;
        reply_audio_last=0;reply_audio_max_gap_ms=0;
        new_response=true;
    } else if (!strcmp(type,"error")) {
        cJSON *error=cJSON_GetObjectItemCaseSensitive(json,"error");
        const char *detail=field(error,"message");
        if(!*detail)detail=field(json,"message");
        ESP_LOGE(TAG,"Server error: code=%s message=%.240s",field(error,"code"),detail);
        status(*detail?detail:"Agent server reported an error");
    } else if(*type)ESP_LOGD(TAG,"Server event: %s",type);
    cJSON_Delete(json);
}
static esp_err_t audio_channel(i2s_chan_handle_t *out,bool mic)
{
    // Independent clocks/slot widths: microphone on I2S0, speaker on I2S1.
    i2s_chan_config_t chan=I2S_CHANNEL_DEFAULT_CONFIG(mic?I2S_NUM_0:I2S_NUM_1,I2S_ROLE_MASTER);
    chan.dma_desc_num=8; chan.dma_frame_num=(mic?AGENT_MIC_CAPTURE_SAMPLE_RATE:AGENT_OUTPUT_SAMPLE_RATE)/100;
    chan.auto_clear_after_cb=!mic; // Underruns output silence, never stale PCM.
    esp_err_t err=i2s_new_channel(&chan,mic?NULL:out,mic?out:NULL);
    if(err!=ESP_OK)return err;
    i2s_std_config_t cfg={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(mic?AGENT_MIC_CAPTURE_SAMPLE_RATE:AGENT_OUTPUT_SAMPLE_RATE),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(mic?I2S_DATA_BIT_WIDTH_32BIT:I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=mic?19:46,.ws=mic?47:45,
                   .dout=mic?I2S_GPIO_UNUSED:42,.din=mic?20:I2S_GPIO_UNUSED},
    };
    err=i2s_channel_init_std_mode(*out,&cfg);
    if(err==ESP_OK && mic) {
        // INMP441 releases SD during the inactive slot. Keep it from floating.
        err=gpio_set_pull_mode(GPIO_NUM_20,GPIO_PULLDOWN_ONLY);
    }
    if(err==ESP_OK && mic) {
        i2s_event_callbacks_t callbacks={.on_recv_q_ovf=on_mic_overflow};
        err=i2s_channel_register_event_callback(*out,&callbacks,NULL);
        mic_overflow=false;
    }
    if(err==ESP_OK)err=i2s_channel_enable(*out);
    if(err!=ESP_OK){i2s_del_channel(*out);*out=NULL;}
    return err;
}
static void speaker_task(void *arg)
{
    i2s_chan_handle_t tx=NULL; unsigned epoch=0;
    bool playback_logged=false;
    bool priming=true;
    bool rebuffering=false;
    int64_t priming_since=0;
    int16_t mono[SPEAKER_FRAMES],stereo[SPEAKER_FRAMES*2];
    for(;;) {
        if(!atomic_load(&enabled)) {
            if(tx){i2s_channel_disable(tx);i2s_del_channel(tx);tx=NULL;}
            while(xStreamBufferReceive(speaker,mono,sizeof(mono),0));
            priming=true;rebuffering=false;priming_since=0;
            vTaskDelay(pdMS_TO_TICKS(30));continue;
        }
        unsigned next=atomic_load(&audio_epoch);
        if(epoch!=next){while(xStreamBufferReceive(speaker,mono,sizeof(mono),0));epoch=next;playback_logged=false;priming=true;rebuffering=false;priming_since=0;}
        if(priming) {
            size_t available=xStreamBufferBytesAvailable(speaker);
            if(!available){vTaskDelay(pdMS_TO_TICKS(5));continue;}
            if(!priming_since)priming_since=esp_timer_get_time();
            // Complete-reply mode trades initial latency for uninterrupted speech.
            // Drain very long replies at high water so the producer cannot deadlock.
            size_t target=AGENT_BUFFER_COMPLETE_REPLY?SPEAKER_HIGH_WATER_BYTES:
                (rebuffering?SPEAKER_REBUFFER_BYTES:SPEAKER_PREROLL_BYTES);
            bool start_deadline=!AGENT_BUFFER_COMPLETE_REPLY && !rebuffering &&
                esp_timer_get_time()-priming_since>=AGENT_SPEAKER_START_MAX_WAIT_MS*1000LL;
            if(available<target && atomic_load(&assistant_receiving) && !start_deadline) {
                vTaskDelay(pdMS_TO_TICKS(5));continue;
            }
            ESP_LOGI(TAG,"Speaker buffer ready: buffered_ms=%u complete=%d",(unsigned)(available*1000/(AGENT_OUTPUT_SAMPLE_RATE*2)),!atomic_load(&assistant_receiving));
            priming=false;priming_since=0;
        }
        size_t bytes=xStreamBufferReceive(speaker,mono,sizeof(mono),pdMS_TO_TICKS(20));
        if(!bytes){
            rebuffering=atomic_load(&assistant_receiving);
            if(rebuffering)ESP_LOGW(TAG,"Speaker underrun; rebuffering %d ms",AGENT_SPEAKER_REBUFFER_MS);
            priming=true;priming_since=0;continue;
        }
        if(!tx && audio_channel(&tx,false)!=ESP_OK){status("Speaker I2S initialization failed");vTaskDelay(pdMS_TO_TICKS(500));continue;}
        if(epoch!=atomic_load(&audio_epoch) || !atomic_load(&enabled))continue;
        int volume=is_audio_enabled()?get_volume():0;
        if(!playback_logged){ESP_LOGI(TAG,"Speaker PCM playback started: rate=%d Hz volume=%d audio_enabled=%d",AGENT_OUTPUT_SAMPLE_RATE,volume,is_audio_enabled());playback_logged=true;}
        for(size_t i=0;i<bytes/2;++i)stereo[i*2]=stereo[i*2+1]=(int32_t)mono[i]*volume/42;
        size_t offset=0,total=bytes*2;
        while(offset<total && atomic_load(&enabled) && epoch==atomic_load(&audio_epoch)) {
            size_t written=0;
            // Half duplex: keep sending silence while the speaker is audible,
            // including a short acoustic tail, to avoid cancelling our own reply.
            if(volume>0)atomic_store(&speaker_quiet_until_ms,(unsigned)(esp_timer_get_time()/1000)+SPEAKER_ECHO_TAIL_MS+20);
            esp_err_t err=i2s_channel_write(tx,(char *)stereo+offset,total-offset,&written,100);
            offset+=written;
            if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT){ESP_LOGE(TAG,"Speaker write failed: %s",esp_err_to_name(err));break;}
            if(!written){ESP_LOGW(TAG,"Speaker write stalled; PCM bytes remaining=%u",(unsigned)(total-offset));vTaskDelay(pdMS_TO_TICKS(1));}
        }
    }
}
static void microphone_task(void *arg)
{
    i2s_chan_handle_t rx=(i2s_chan_handle_t)arg;
    int32_t raw[CAPTURE_FRAMES*2];mic_block_t block;unsigned dropped=0,overflows=0;
    size_t pending=0,warmup=AGENT_MIC_CAPTURE_SAMPLE_RATE/10;
    int64_t sample_sum=0;unsigned sample_count=0;
    int slot=AGENT_MIC_SLOT<0?0:AGENT_MIC_SLOT;
    uint64_t level[2]={0,0};unsigned measured=0,peak=0,clipped=0;
    int64_t last_level=esp_timer_get_time();
    for(;;) {
        size_t got=0;
        esp_err_t err=i2s_channel_read(rx,raw,sizeof(raw),&got,1000);
        if(err!=ESP_OK || !got || got%8) {
            ESP_LOGE(TAG,"Microphone read failed: %s bytes=%u",esp_err_to_name(err),(unsigned)got);
            atomic_store(&mic_error,err==ESP_OK?ESP_FAIL:err);
            // The owner deletes this task before releasing its RX channel.
            vTaskSuspend(NULL);continue;
        }
        if(mic_overflow) {
            mic_overflow=false;
            if(++overflows==1 || overflows%100==0)ESP_LOGW(TAG,"Microphone DMA overflows=%u; continuing capture",overflows);
        }
        size_t frames=got/8;
        if(warmup){warmup=frames>=warmup?0:warmup-frames;continue;}
        // Always drain I2S, but do not queue silence or speaker echo for upload.
        if(microphone_paused()) {
            pending=0;sample_sum=0;sample_count=0;
            level[0]=level[1]=0;measured=peak=clipped=0;
            last_level=esp_timer_get_time();continue;
        }
        uint64_t energy[2]={0,0};
        for(size_t i=0;i<frames;++i)for(int ch=0;ch<2;++ch) {
            int32_t s=raw[i*2+ch]>>16;
            energy[ch]+=s<0?-s:s;
        }
        // Require a clear difference to avoid switching channels on quiet noise.
        if(AGENT_MIC_SLOT<0 && energy[1-slot]>energy[slot]*4 && energy[1-slot]>frames*8) {
            slot=1-slot;pending=0;sample_sum=0;sample_count=0;
            ESP_LOGI(TAG,"Microphone active slot: %s",slot?"right":"left");
        }
        level[0]+=energy[0];level[1]+=energy[1];measured+=frames;
        for(size_t i=0;i<frames;++i) {
            // Low-pass by averaging before decimation, preserving state across
            // short I2S reads. Scale and clip only after reducing the sample rate.
            sample_sum+=raw[i*2+slot]>>8;
            if(++sample_count<MIC_DECIMATION)continue;
            int32_t s=(int32_t)(sample_sum*AGENT_MIC_GAIN/(256*MIC_DECIMATION));
            sample_sum=0;sample_count=0;
            if(s>32767){s=32767;++clipped;}else if(s<-32768){s=-32768;++clipped;}
            unsigned magnitude=s<0?-s:s;if(magnitude>peak)peak=magnitude;
            block.pcm[pending++]=(int16_t)s;
            // Short successful I2S reads are assembled into complete blocks.
            if(pending==FRAMES) {
                block.bytes=sizeof(block.pcm);pending=0;
                if(xQueueSend(microphone,&block,0)!=pdTRUE) {
                    if(++dropped==1 || dropped%100==0)ESP_LOGW(TAG,"Network audio queue full; dropped blocks=%u",dropped);
                }
            }
        }
        if(esp_timer_get_time()-last_level>=2000000 && measured) {
            ESP_LOGI(TAG,"Microphone levels: slot=%s left_avg=%u right_avg=%u pcm_peak=%u clipped=%u",
                slot?"right":"left",(unsigned)(level[0]/measured),(unsigned)(level[1]/measured),peak,clipped);
            if(peak<32)ESP_LOGW(TAG,"Microphone signal near silence; check VDD/GND, BCLK=19 WS=47 SD=20 and L/R wiring");
            level[0]=level[1]=0;measured=peak=clipped=0;last_level=esp_timer_get_time();
        }
    }
}
static bool session_url(char *url,size_t size)
{
    ESP_LOGI(TAG,"Session init URL: %s",AGENT_SESSION_INIT_URL);
    esp_http_client_config_t cfg={.url=AGENT_SESSION_INIT_URL,
        .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=10000};
    esp_http_client_handle_t client=esp_http_client_init(&cfg); if(!client)return false;
    cJSON *config=cJSON_CreateObject();
    cJSON *user=config?cJSON_AddObjectToObject(config,"userData"):NULL;
    bool configured=user && cJSON_AddStringToObject(user,"name",AGENT_USER_NAME) &&
        cJSON_AddStringToObject(user,"email",AGENT_USER_EMAIL) &&
        cJSON_AddStringToObject(user,"mobile",AGENT_USER_MOBILE) &&
        cJSON_AddStringToObject(config,"websiteName",AGENT_WEBSITE_NAME) &&
        cJSON_AddStringToObject(config,"language",AGENT_LANGUAGE) &&
        cJSON_AddStringToObject(config,"fileName",AGENT_CONTEXT_FILE);
    char *body=configured?cJSON_PrintUnformatted(config):NULL;
    cJSON_Delete(config);
    if(!body){esp_http_client_cleanup(client);return false;}
    esp_http_client_set_method(client,HTTP_METHOD_POST);
    esp_http_client_set_header(client,"Content-Type","application/json");
    bool ok=false; char *response=malloc(8192); size_t used=0;
    if(!response){free(body);esp_http_client_cleanup(client);return false;}
    esp_err_t open_err=esp_http_client_open(client,strlen(body));
    if(open_err!=ESP_OK)ESP_LOGE(TAG,"Session HTTP open failed: %s",esp_err_to_name(open_err));
    if(open_err==ESP_OK && esp_http_client_write(client,body,strlen(body))==(int)strlen(body)) {
        int64_t headers=esp_http_client_fetch_headers(client);
        int n;
        while(used<8191 && (n=esp_http_client_read(client,response+used,8191-used))>0)used+=n;
        response[used]=0;
        int http_status=esp_http_client_get_status_code(client);
        ESP_LOGI(TAG,"Session init HTTP=%d response bytes=%u",http_status,(unsigned)used);
        if(headers>=0 && esp_http_client_is_complete_data_received(client) && http_status==200) {
            cJSON *json=cJSON_Parse(response); const char *u=field(json,"websocketUrl");
            ok=!strncmp(u,"wss://",6) && strlen(u)<size;
            if(ok)strcpy(url,u);
            else ESP_LOGE(TAG,"Session response missing a valid wss websocketUrl");
            cJSON_Delete(json);
        } else ESP_LOGE(TAG,"Session init rejected (HTTP %d)",http_status);
    }
    free(body);free(response);esp_http_client_close(client);esp_http_client_cleanup(client);return ok;
}
static void agent_task(void *arg)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_init();
    for(;;) {
        setup_snapshot_t wifi; wifi_setup_snapshot(0,&wifi);
        if(!atomic_load(&enabled) || !wifi.connected){status(atomic_load(&enabled)?"Connect Wi-Fi to use Agent Mode":"Agent Mode off");vTaskDelay(pdMS_TO_TICKS(500));continue;}
        if(time(NULL)<1700000000){status("Synchronizing clock for secure connection");vTaskDelay(pdMS_TO_TICKS(1000));continue;}
        status("Connecting to Bonrix...");
        unsigned generation=atomic_load(&mode_epoch);
        char url[2048],host[256];
        if(!session_url(url,sizeof(url))){status("Session failed; retrying in 5 seconds");vTaskDelay(pdMS_TO_TICKS(5000));continue;}
        if(!atomic_load(&enabled) || generation!=atomic_load(&mode_epoch))continue;
        char *authority=url+6,*path=strchr(authority,'/');
        size_t hostlen=path?(size_t)(path-authority):strlen(authority);
        if(hostlen>=sizeof(host))continue;
        memcpy(host,authority,hostlen);host[hostlen]=0;int port=443;
        char *colon=strchr(host,':');if(colon){*colon++=0;port=atoi(colon);}
        esp_transport_handle_t ssl=esp_transport_ssl_init(),ws=NULL;
        i2s_chan_handle_t rx=NULL; char *incoming=NULL; size_t used=0,frame_read=0;
        TaskHandle_t capture=NULL;
        send_batch_t *batch=NULL;
        wifi_ps_type_t previous_ps=WIFI_PS_MIN_MODEM;
        bool changed_ps=false;
        const char *reason="Agent screen closed";
        char failure[96];unsigned sent_chunks=0,received_messages=0;
        if(!ssl){reason="TLS allocation failed";goto cleanup;}
        esp_transport_ssl_crt_bundle_attach(ssl,esp_crt_bundle_attach);
        ws=esp_transport_ws_init(ssl);if(!ws){reason="WebSocket allocation failed";goto cleanup;}
        esp_transport_ws_set_path(ws,path?path:"/");
        esp_transport_ws_set_headers(ws,"Origin: " AGENT_WEBSOCKET_ORIGIN "\r\n");
        if(esp_transport_connect(ws,host,port,10000)<0) {
            snprintf(failure,sizeof(failure),"WebSocket handshake failed (HTTP %d)",esp_transport_ws_get_upgrade_request_status(ws));
            reason=failure;goto cleanup;
        }
        ESP_LOGI(TAG,"WebSocket connected: host=%s port=%d HTTP=%d",host,port,esp_transport_ws_get_upgrade_request_status(ws));
        if(!atomic_load(&enabled) || generation!=atomic_load(&mode_epoch))goto cleanup;
        // WS headers and payloads are separate TLS writes in IDF 5.4.
        // Disable Nagle so small headers do not wait for delayed TCP ACKs.
        int socket_fd=esp_transport_get_socket(ws),no_delay=1;
        if(socket_fd<0 || setsockopt(socket_fd,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof(no_delay))!=0) {
            reason="WebSocket low-latency socket setup failed";goto cleanup;
        }
        ESP_LOGI(TAG,"Audio transport: TCP_NODELAY enabled; half duplex; microphone=%d Hz speaker=%d Hz PCM",AGENT_INPUT_SAMPLE_RATE,AGENT_OUTPUT_SAMPLE_RATE);
        // Interactive audio needs low latency rather than modem sleep.
        if(esp_wifi_get_ps(&previous_ps)==ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE)==ESP_OK)changed_ps=true;
        // LCD control CS remains high; RGB refresh never writes control SDA.
        gpio_set_level(GPIO_NUM_39,1);gpio_set_level(GPIO_NUM_48,0);
        esp_err_t audio_err=audio_channel(&rx,true);
        if(audio_err!=ESP_OK){snprintf(failure,sizeof(failure),"Microphone init: %s",esp_err_to_name(audio_err));reason=failure;goto cleanup;}
        incoming=heap_caps_malloc(MESSAGE_MAX+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!incoming){reason="Receive buffer allocation failed";goto cleanup;}
        batch=heap_caps_malloc(sizeof(*batch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!batch){reason="Send buffer allocation failed";goto cleanup;}
        xQueueReset(microphone);atomic_store(&mic_error,ESP_OK);
        atomic_store(&assistant_receiving,false);
        if(xTaskCreate(microphone_task,"agent_mic",8192,rx,6,&capture)!=pdPASS){reason="Microphone task allocation failed";goto cleanup;}
        xSemaphoreTake(lock,portMAX_DELAY);state.awake=true;state.user[0]=state.assistant[0]=0;++state.revision;xSemaphoreGive(lock);
        flush_audio(); status("Listening - speak to Bonrix");
        new_response=true;
        new_input=true;
        reply_audio_chunks=reply_text_chunks=suppressed_audio_chunks=0;
        reply_audio_bytes=0;reply_audio_started=0;
        reply_audio_last=0;reply_audio_max_gap_ms=0;
        if(!send_text(ws,"{\"type\":\"trigger.greeting\"}")){reason="Startup greeting send failed";goto cleanup;}
        ESP_LOGI(TAG,"Agent started automatically; startup greeting sent; speaker enabled");
        mic_block_t block;size_t batch_bytes=0;
        int64_t blocked_since=0,last_block_log=0,last_upload_log=esp_timer_get_time();
        bool was_paused=false;
        while(atomic_load(&enabled) && generation==atomic_load(&mode_epoch)) {
            wifi_setup_snapshot(0,&wifi);if(!wifi.connected){reason="Wi-Fi disconnected";break;}
            if(atomic_load(&mic_error)!=ESP_OK){reason="Microphone read failed";break;}
            bool paused=microphone_paused();
            if(paused!=was_paused) {
                status(paused?"Agent speaking - please wait":"Listening - speak to Bonrix");
                ESP_LOGI(TAG,"Microphone uploads %s",paused?"paused for speaker playback":"resumed; listening");
                was_paused=paused;
            }
            if(paused) {
                // Drop pre-reply capture rather than sending delayed speech
                // after playback. No new blocks are queued while paused.
                xQueueReset(microphone);batch_bytes=0;blocked_since=0;
            }
            // Fill an entire packet from queued capture blocks before receiving
            // more server audio. Only the first block may wait, and briefly.
            while(!microphone_paused() && batch_bytes<sizeof(batch->pcm) && xQueueReceive(microphone,&block,batch_bytes?0:pdMS_TO_TICKS(5))==pdTRUE) {
                // Capture produces complete 20 ms blocks at the upload rate.
                if(block.bytes!=FRAMES*2){reason="Unexpected microphone block size";goto cleanup;}
                memcpy((char *)batch->pcm+batch_bytes,block.pcm,block.bytes);
                batch_bytes+=block.bytes;
            }
            if(batch_bytes==sizeof(batch->pcm)) {
                // Poll before starting a frame. A not-ready socket is safe to
                // wait on; a failed/partial frame write must never be resent.
                int ready=esp_transport_poll_write(ws,0);
                if(ready<0){reason="WebSocket write poll failed";break;}
                if(!ready) {
                    int64_t now=esp_timer_get_time();
                    if(!blocked_since)blocked_since=now;
                    if(now-last_block_log>1000000){ESP_LOGW(TAG,"WebSocket backpressure: blocked %u ms",(unsigned)((now-blocked_since)/1000));last_block_log=now;}
                    if(now-blocked_since>10000000){reason="WebSocket blocked for 10 seconds";break;}
                } else {
                blocked_since=0;
                size_t encoded=0;
                if(mbedtls_base64_encode(batch->base64,sizeof(batch->base64),&encoded,(unsigned char *)batch->pcm,batch_bytes)){reason="Audio encoding failed";break;}
                batch->base64[encoded]=0;
                snprintf(batch->packet,sizeof(batch->packet),"{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",batch->base64);
                if(!send_text(ws,batch->packet)){reason="WebSocket audio send failed";break;}
                batch_bytes=0;
                if(++sent_chunks==1)ESP_LOGI(TAG,"First microphone audio sent (%d Hz PCM16 mono, 80 ms packets, %u bytes)",AGENT_INPUT_SAMPLE_RATE,(unsigned)sizeof(batch->pcm));
                if(esp_timer_get_time()-last_upload_log>=2000000) {
                    ESP_LOGI(TAG,"Microphone upload: packets=%u total_audio_ms=%u",sent_chunks,sent_chunks*80);
                    last_upload_log=esp_timer_get_time();
                }
                }
            }
            // Backpressure polling must yield so Wi-Fi/TCP workers can run.
            if(blocked_since)vTaskDelay(pdMS_TO_TICKS(5));
            // While half-duplex playback pauses uploads, prioritize draining
            // incoming audio. Retain the smaller budget while capturing speech.
            int64_t receive_started=esp_timer_get_time();
            bool receiving_reply=microphone_paused();
            int read_limit=receiving_reply?64:4;
            int64_t receive_budget=receiving_reply?30000:10000;
            for(int reads=0;reads<read_limit && esp_timer_get_time()-receive_started<receive_budget;++reads) {
                int ready=esp_transport_poll_read(ws,receiving_reply && reads==0?5:0);
                if(ready<0){reason="WebSocket socket poll failed";goto cleanup;}
                if(!ready)break;
                char chunk[4096];int n=esp_transport_read(ws,chunk,sizeof(chunk),100);
                int op=esp_transport_ws_get_read_opcode(ws);
                if(op==WS_TRANSPORT_OPCODES_CLOSE){reason="Server closed WebSocket (see transport log)";goto cleanup;}
                if(n<0){reason="WebSocket receive failed";goto cleanup;}
                if(n==0)continue;
                if(op!=WS_TRANSPORT_OPCODES_TEXT && op!=WS_TRANSPORT_OPCODES_CONT)continue;
                if(used+(size_t)n>MESSAGE_MAX){reason="Server message exceeds receive limit";goto cleanup;}
                memcpy(incoming+used,chunk,n);used+=n;frame_read+=n;
                if(frame_read==(size_t)esp_transport_ws_get_read_payload_len(ws)) {
                    frame_read=0;
                    if(esp_transport_ws_get_fin_flag(ws)){incoming[used]=0;message(incoming);used=0;++received_messages;}
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
cleanup:
        ESP_LOGW(TAG,"Session ended: %s; audio chunks=%u messages=%u errno=%d",reason,sent_chunks,received_messages,errno);
        if(capture)vTaskDelete(capture);
        xSemaphoreTake(lock,portMAX_DELAY);state.awake=false;++state.revision;xSemaphoreGive(lock);
        atomic_store(&assistant_receiving,false);
        flush_audio();free(incoming);free(batch);
        if(rx){i2s_channel_disable(rx);i2s_del_channel(rx);gpio_set_direction(GPIO_NUM_19,GPIO_MODE_OUTPUT);gpio_set_level(GPIO_NUM_19,0);}
        if(ws){esp_transport_close(ws);esp_transport_destroy(ws);}if(ssl)esp_transport_destroy(ssl);
        if(changed_ps)esp_wifi_set_ps(previous_ps);
        if(atomic_load(&enabled)){char notice[96];snprintf(notice,sizeof(notice),"%.70s; retrying...",reason);status(notice);}
        else status("Agent Mode off");
        for(int i=0;i<50 && atomic_load(&enabled);++i)vTaskDelay(pdMS_TO_TICKS(100));
    }
}
esp_err_t agent_mode_start(void)
{
    if(lock)return ESP_OK;
    lock=xSemaphoreCreateMutex();
    if(!lock)return ESP_ERR_NO_MEM;
    status("Agent Mode off");
    uint8_t *storage=heap_caps_malloc(SPEAKER_BUFFER_BYTES+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(storage)speaker=xStreamBufferCreateStatic(SPEAKER_BUFFER_BYTES+1,2,storage,&speaker_control);
    uint8_t *mic_storage=heap_caps_malloc(32*sizeof(mic_block_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(mic_storage)microphone=xQueueCreateStatic(32,sizeof(mic_block_t),mic_storage,&microphone_control);
    TaskHandle_t playback=NULL;
    if(!speaker || !microphone)goto failed;
    if(xTaskCreate(speaker_task,"agent_speaker",6144,NULL,5,&playback)!=pdPASS)goto failed;
    if(xTaskCreate(agent_task,"bonrix_agent",32768,NULL,4,NULL)!=pdPASS)goto failed;
    return ESP_OK;
failed:
    if(playback)vTaskDelete(playback);
    if(speaker)vStreamBufferDelete(speaker);
    if(microphone)vQueueDelete(microphone);
    free(storage);free(mic_storage);
    speaker=NULL;microphone=NULL;
    vSemaphoreDelete(lock);lock=NULL;
    return ESP_ERR_NO_MEM;
}
