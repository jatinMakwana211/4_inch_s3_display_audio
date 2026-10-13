#include "wifi_ui.h"
#include "wifi_ui_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// FT6336U (FT5x06 compatible). Change only if the physical panel is rotated.
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0
static i2c_master_bus_handle_t touch_bus;
static i2c_master_dev_handle_t touch;
static esp_lcd_panel_handle_t lcd;
static const uint8_t *welcome_pixels;
static uint16_t *canvas;
static wifi_ui_view_t view;
static bool scan_after_failure;
extern const uint8_t ui_font[] asm("_binary_ui_font_bin_start");
extern const uint16_t ui_background[] asm("_binary_wifi_background_rgb565_start");
static const char *TAG="WIFI_UI";

static void load_agent_setting(void)
{
    nvs_handle_t handle;
    uint8_t enabled=0;
    if(nvs_open("agent",NVS_READONLY,&handle)==ESP_OK) {
        if(nvs_get_u8(handle,"enabled",&enabled)==ESP_OK) view.agent_enabled=enabled!=0;
        nvs_close(handle);
    }
}

static esp_err_t save_agent_setting(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err=nvs_open("agent",NVS_READWRITE,&handle);
    if(err!=ESP_OK) return err;
    err=nvs_set_u8(handle,"enabled",enabled);
    if(err==ESP_OK) err=nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus={
        .i2c_port=I2C_NUM_0, .sda_io_num=40, .scl_io_num=41,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true,
    };
    esp_err_t err=i2c_new_master_bus(&bus,&touch_bus);
    if(err!=ESP_OK) return err;
    i2c_device_config_t device={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x38,.scl_speed_hz=100000};
    err=i2c_master_bus_add_device(touch_bus,&device,&touch);
    if(err==ESP_OK) err=i2c_master_probe(touch_bus,0x38,100);
    if(err!=ESP_OK) {
        if(touch) i2c_master_bus_rm_device(touch);
        i2c_del_master_bus(touch_bus);
        touch=NULL; touch_bus=NULL;
    }
    return err;
}

// One coherent register read: touch count, XH, XL, YH, YL.
static esp_err_t touch_read(bool *down,int *x,int *y)
{
    const uint8_t reg=0x02;
    uint8_t data[5];
    esp_err_t err=i2c_master_transmit_receive(touch,&reg,1,data,sizeof(data),30);
    if(err!=ESP_OK) return err;
    *down=false;
    if((data[0]&0x0f)==0 || (data[0]&0x0f)>2 || (data[1]>>6)==1) return ESP_OK;
    int px=((data[1]&0x0f)<<8)|data[2],py=((data[3]&0x0f)<<8)|data[4];
    if(TOUCH_SWAP_XY) {int tmp=px;px=py;py=tmp;}
    if(TOUCH_MIRROR_X) px=479-px;
    if(TOUCH_MIRROR_Y) py=479-py;
    if(px<0 || px>=480 || py<0 || py>=480) return ESP_OK;
    *x=px; *y=py; *down=true;
    return ESP_OK;
}

static void clear_password(void)
{
    volatile char *p=view.password;
    for(unsigned i=0;i<sizeof(view.password);++i) p[i]=0;
    view.show_password=false;
}
static void request_result(esp_err_t err)
{
    if(err!=ESP_OK) snprintf(view.notice,sizeof(view.notice),"Unable to start: %s",esp_err_to_name(err));
}
static void handle(ui_hit_t hit)
{
    view.notice[0]=0;
    switch(hit.action) {
    case UI_SETTINGS_OPEN:
        view.screen=UI_SETTINGS;
        break;
    case UI_AGENT_TOGGLE: {
        bool enabled=!view.agent_enabled;
        esp_err_t err=save_agent_setting(enabled);
        if(err==ESP_OK) view.agent_enabled=enabled;
        else request_result(err);
        break;
    }
    case UI_OPEN:
        scan_after_failure=false;
        if(view.wifi.connecting) view.screen=UI_CONNECTING;
        else if(view.wifi.connected) view.screen=UI_CONNECTED;
        else if(view.wifi.saved_available && !view.wifi.busy) {
            esp_err_t err=wifi_setup_connect_saved();
            view.screen=err==ESP_OK?UI_CONNECTING:UI_CONFIG;
            request_result(err);
        } else {
            view.screen=UI_CONFIG;
            if(!view.wifi.busy) {view.offset=0;request_result(wifi_setup_scan());}
        }
        break;
    case UI_BACK:
        if(view.screen==UI_PASSWORD) {view.screen=UI_CONFIG;view.show_password=false;}
        else if(view.screen==UI_AGENT) view.screen=UI_SETTINGS;
        else {view.screen=view.agent_enabled?UI_AGENT:UI_WELCOME;scan_after_failure=false;clear_password();}
        break;
    case UI_SCAN: view.offset=0; request_result(wifi_setup_scan()); break;
    case UI_CHOOSE:
        view.offset=0;
        if(!view.wifi.count)request_result(wifi_setup_scan());
        else snprintf(view.notice,sizeof(view.notice),"Choose a network from the list on the right.");
        break;
    case UI_PREVIOUS: if(view.offset>=WIFI_PAGE_ROWS) view.offset-=WIFI_PAGE_ROWS; break;
    case UI_NEXT: if(view.offset+WIFI_PAGE_ROWS<view.wifi.count) view.offset+=WIFI_PAGE_ROWS; break;
    case UI_SELECT:
        if(hit.value<0 || (unsigned)hit.value>=view.wifi.row_count) break;
        view.selected=view.wifi.rows[hit.value];
        clear_password();
        if(!view.selected.supported) snprintf(view.notice,sizeof(view.notice),"This network's security is not supported.");
        else if(!view.selected.open) view.screen=UI_PASSWORD;
        break;
    case UI_EDIT:
        if(!view.selected.ssid[0]) snprintf(view.notice,sizeof(view.notice),"Select a network from the list first.");
        else if(view.selected.supported && !view.selected.open) view.screen=UI_PASSWORD;
        break;
    case UI_CONNECT:
        if(!view.selected.ssid[0]) snprintf(view.notice,sizeof(view.notice),"Select a network from the list first.");
        else if(!view.selected.supported) snprintf(view.notice,sizeof(view.notice),"This network's security is not supported.");
        else if(!view.selected.open && strlen(view.password)<8) snprintf(view.notice,sizeof(view.notice),"Password needs at least 8 characters.");
        else {
            esp_err_t err=wifi_setup_connect(&view.selected,view.password);
            request_result(err);
            if(err==ESP_OK) {view.screen=UI_CONNECTING;clear_password();}
        }
        break;
    case UI_DONE: view.screen=UI_CONFIG; view.show_password=false; break;
    case UI_CONFIGURE: view.screen=UI_CONFIG; view.offset=0; request_result(wifi_setup_scan()); break;
    case UI_REFRESH: request_result(wifi_setup_refresh()); break;
    case UI_DISCONNECT: clear_password();request_result(wifi_setup_disconnect());break;
    default: wifi_ui_edit(&view,hit);break;
    }
}

static void ui_task(void *arg)
{
    bool dirty=true;
    ui_touch_state_t touch_state={0};
    unsigned failures=0,seen_connection=0,revision=0,last_seconds=0;
    for(;;) {
        agent_mode_enable(view.agent_enabled && view.screen==UI_AGENT);
        agent_snapshot_t agent;
        agent_mode_snapshot(&agent);
        if(agent.revision!=view.agent.revision) {view.agent=agent;if(view.screen==UI_AGENT)dirty=true;}
        wifi_setup_snapshot(view.offset,&view.wifi);
        if(view.screen==UI_CONNECTING) {
            ui_screen_t next=wifi_ui_connection_screen(&view.wifi);
            if(next!=UI_CONNECTING) {
                view.screen=next;
                clear_password();
                if(next==UI_CONFIG) {
                    view.offset=0;
                    scan_after_failure=true;
                    const char *message=view.wifi.message;
                    if(view.wifi.connecting)message=view.wifi.connect_remaining_ms==0?
                        "Could not connect in 15s. Choose a network.":"Unable to connect. Choose a network.";
                    snprintf(view.notice,sizeof(view.notice),"%s",message);
                }
                dirty=true;
            }
        }
        // Show the form at the deadline; allow the worker to finish disconnecting
        // before starting a fresh scan. Back always keeps the welcome screen.
        if(scan_after_failure && !view.wifi.busy) {
            scan_after_failure=false;
            if(view.screen==UI_CONFIG) {
                request_result(wifi_setup_scan());
                wifi_setup_snapshot(view.offset,&view.wifi);
                dirty=true;
            }
        }
        if(view.wifi.connected && !view.wifi.busy && view.wifi.connection_id!=seen_connection) {
            seen_connection=view.wifi.connection_id;
            clear_password();
            if(view.screen==UI_CONNECTING || view.screen==UI_CONFIG) view.screen=UI_CONNECTED;
            dirty=true;
        }
        if(view.wifi.revision!=revision) {
            revision=view.wifi.revision;
            if(view.screen!=UI_WELCOME) dirty=true;
        }
        if(view.screen==UI_CONNECTED && last_seconds!=view.wifi.connected_seconds) {
            last_seconds=view.wifi.connected_seconds; dirty=true;
        }
        bool down=false;
        int x=0,y=0;
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        esp_err_t err=touch_read(&down,&x,&y);
        if(err==ESP_OK) {
            failures=0;
            ui_hit_t before=view.pressed;
            ui_hit_t hit=wifi_ui_touch(&view,&touch_state,down,x,y,now);
            if(before.action!=view.pressed.action || before.value!=view.pressed.value)dirty=true;
            if(hit.action!=UI_NONE) {handle(hit);wifi_setup_snapshot(view.offset,&view.wifi);dirty=true;}
        } else if(++failures==20) {
            ESP_LOGW(TAG,"Touch read failed: %s; check GPIO40/41 wiring",esp_err_to_name(err));
            // Cancel a stale press but require release before a new action.
            touch_state.cancelled=true;
            view.pressed=(ui_hit_t){0};dirty=true;
        }
        unsigned phase=(now/120)%8;
        if(view.wifi.busy && (view.screen==UI_CONFIG || view.screen==UI_CONNECTING) && view.spinner_phase!=phase) {view.spinner_phase=phase;dirty=true;}
        if(dirty) {
            const void *pixels=canvas;
            if(view.screen==UI_WELCOME) wifi_ui_welcome(canvas,ui_font,welcome_pixels,&view);
            else wifi_ui_render(canvas,ui_font,ui_background,&view);
            err=esp_lcd_panel_draw_bitmap(lcd,0,0,480,480,pixels);
            if(err!=ESP_OK) ESP_LOGE(TAG,"Drawing failed: %s",esp_err_to_name(err));
            dirty=false;
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

esp_err_t wifi_ui_start(esp_lcd_panel_handle_t panel,const uint8_t *welcome)
{
    if(canvas) return ESP_OK;
    esp_err_t err=touch_init();
    if(err!=ESP_OK) return err;
    canvas=heap_caps_malloc(480*480*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!canvas) {err=ESP_ERR_NO_MEM;goto cleanup;}
    err=wifi_setup_start();
    if(err!=ESP_OK) goto cleanup;
    lcd=panel; welcome_pixels=welcome;
    load_agent_setting();
    err=agent_mode_start();
    if(err!=ESP_OK) {request_result(err);ESP_LOGE(TAG,"Agent startup failed: %s",esp_err_to_name(err));}
    view.screen=view.agent_enabled?UI_AGENT:UI_WELCOME;
    if(xTaskCreate(ui_task,"wifi_touch_ui",8192,NULL,3,NULL)!=pdPASS) {err=ESP_ERR_NO_MEM;goto cleanup;}
    return ESP_OK;
cleanup:
    free(canvas);canvas=NULL;
    i2c_master_bus_rm_device(touch);i2c_del_master_bus(touch_bus);
    touch=NULL;touch_bus=NULL;
    return err;
}
