// Host tests exercise the production renderer and hit testing with sample data.
#include "wifi_ui_view.h"
#include <string.h>
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

__declspec(dllexport) int host_tests(void)
{
    wifi_ui_view_t v={0};
    CHECK(wifi_ui_hit(&v,441,30).action==UI_OPEN);
    CHECK(wifi_ui_hit(&v,240,240).action==UI_NONE);
    CHECK(wifi_ui_hit(&v,480,30).action==UI_NONE);
    CHECK(wifi_ui_hit(&v,369,35).action==UI_SETTINGS_OPEN);
    v.screen=UI_SETTINGS;v.wifi.busy=true;
    CHECK(wifi_ui_hit(&v,380,202).action==UI_AGENT_TOGGLE);
    CHECK(wifi_ui_hit(&v,35,45).action==UI_BACK);
    v.screen=UI_AGENT;
    CHECK(wifi_ui_hit(&v,441,30).action==UI_OPEN);
    CHECK(wifi_ui_hit(&v,369,35).action==UI_SETTINGS_OPEN);
    CHECK(wifi_ui_hit(&v,35,45).action==UI_BACK);
    v.wifi.busy=false;
    v.screen=UI_CONFIG;v.wifi.row_count=4;v.wifi.count=8;
    for(int i=0;i<4;++i) {
        ui_hit_t hit=wifi_ui_hit(&v,300,204+i*44);
        CHECK(hit.action==UI_SELECT && hit.value==i);
    }
    CHECK(wifi_ui_hit(&v,100,365).action==UI_NONE);
    memcpy(v.selected.ssid,"Test",5);v.selected.supported=true;v.selected.open=true;
    CHECK(wifi_ui_hit(&v,100,365).action==UI_CONNECT);
    CHECK(wifi_ui_hit(&v,100,212).action==UI_CHOOSE);
    CHECK(wifi_ui_hit(&v,280,380).action==UI_NONE);
    CHECK(wifi_ui_hit(&v,430,380).action==UI_NEXT);
    v.offset=4;CHECK(wifi_ui_hit(&v,280,380).action==UI_PREVIOUS);
    CHECK(wifi_ui_hit(&v,430,380).action==UI_NONE);v.offset=0;
    ui_touch_state_t touch={0};
    CHECK(wifi_ui_touch(&v,&touch,true,100,365,100).action==UI_NONE);
    CHECK(v.pressed.action==UI_CONNECT);
    CHECK(wifi_ui_touch(&v,&touch,true,102,367,500).action==UI_NONE);
    CHECK(wifi_ui_touch(&v,&touch,false,0,0,520).action==UI_NONE);
    CHECK(wifi_ui_touch(&v,&touch,false,0,0,540).action==UI_CONNECT);
    CHECK(v.pressed.action==UI_NONE);
    CHECK(wifi_ui_touch(&v,&touch,false,0,0,560).action==UI_NONE);
    wifi_ui_touch(&v,&touch,true,100,365,600);
    wifi_ui_touch(&v,&touch,true,140,365,630); // Cancel dragging even within a large button.
    wifi_ui_touch(&v,&touch,false,0,0,650);
    CHECK(wifi_ui_touch(&v,&touch,false,0,0,680).action==UI_NONE);
    wifi_ui_touch(&v,&touch,true,100,365,700);
    wifi_ui_touch(&v,&touch,false,0,0,712); // Transient no-touch report must not double activate.
    wifi_ui_touch(&v,&touch,true,100,365,720);
    wifi_ui_touch(&v,&touch,false,0,0,760);
    CHECK(wifi_ui_touch(&v,&touch,false,0,0,780).action==UI_CONNECT);
    v.wifi.busy=true;
    CHECK(wifi_ui_hit(&v,100,365).action==UI_NONE);
    CHECK(wifi_ui_hit(&v,439,160).action==UI_NONE);
    CHECK(wifi_ui_hit(&v,40,35).action==UI_BACK);
    v.wifi.busy=false;v.screen=UI_PASSWORD;
    bool reachable[127]={0};reachable[' ']=true;
    for(unsigned mode=0;mode<3;++mode) {
        v.keyboard_page=mode==1;v.caps=mode==2;
        for(int y=239;y<407;++y) for(int x=0;x<480;++x) {
            ui_hit_t hit=wifi_ui_hit(&v,x,y);
            if(hit.action==UI_KEY) {CHECK(hit.value>=33 && hit.value<=126);reachable[hit.value]=true;}
        }
    }
    for(int i=32;i<=126;++i) CHECK(reachable[i]);
    for(int i=0;i<80;++i)wifi_ui_edit(&v,(ui_hit_t){UI_KEY,'x'});
    CHECK(strlen(v.password)==63 && v.password[63]==0);
    wifi_ui_edit(&v,(ui_hit_t){UI_SPACE,0});CHECK(strlen(v.password)==63);
    for(int i=0;i<80;++i)wifi_ui_edit(&v,(ui_hit_t){UI_DELETE,0});
    CHECK(strlen(v.password)==0);
    wifi_ui_edit(&v,(ui_hit_t){UI_KEY,'A'});wifi_ui_edit(&v,(ui_hit_t){UI_SPACE,0});
    CHECK(!strcmp(v.password,"A "));
    wifi_ui_edit(&v,(ui_hit_t){UI_REVEAL,0});CHECK(v.show_password);
    v.screen=UI_CONNECTED;v.wifi.connected=true;
    CHECK(wifi_ui_hit(&v,60,385).action==UI_CONFIGURE);
    CHECK(wifi_ui_hit(&v,230,385).action==UI_REFRESH);
    CHECK(wifi_ui_hit(&v,410,385).action==UI_DISCONNECT);
    v.wifi.busy=true;CHECK(wifi_ui_hit(&v,410,385).action==UI_NONE);
    v.screen=UI_CONNECTING;
    CHECK(wifi_ui_hit(&v,40,35).action==UI_BACK);
    CHECK(wifi_ui_hit(&v,240,300).action==UI_NONE);
    v.wifi=(setup_snapshot_t){.busy=true,.connecting=true,.stage=SETUP_ASSOCIATING,.connect_remaining_ms=15000};
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONNECTING);
    v.wifi.connect_remaining_ms=1;
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONNECTING);
    v.wifi.connect_remaining_ms=0; // Return immediately, even while radio cleanup is busy.
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONFIG);
    v.wifi.connected=true;v.wifi.stage=SETUP_SAVING;
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONNECTING);
    v.wifi.stage=SETUP_READY; // Flash finished; the worker has not cleared busy yet.
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONNECTING);
    v.wifi.busy=false;v.wifi.connecting=false;v.wifi.stage=SETUP_READY;
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONNECTED);
    v.wifi.connected=false;v.wifi.stage=SETUP_FAILED;
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONFIG);
    v.wifi.connecting=true;v.wifi.connect_remaining_ms=10000;
    CHECK(wifi_ui_connection_screen(&v.wifi)==UI_CONFIG);
    return 0;
}
__declspec(dllexport) void host_render(int screen,const uint8_t *font,const uint16_t *background,uint16_t *pixels)
{
    wifi_ui_view_t v={.screen=screen};
    if(screen==UI_SETTINGS || screen==UI_AGENT) {
        v.agent_enabled=true;
        strcpy(v.agent.status,"Listening - speak to Bonrix");
        strcpy(v.agent.user,"Hello Bonrix, how can you help me today?");
        strcpy(v.agent.assistant,"Hello! I can answer your questions. What would you like to know?");
    }
    memcpy(v.selected.ssid,"Bonrix_Office",14);v.selected.supported=true;
    memcpy(v.password,"sample-password",16);
    v.wifi.row_count=4;v.wifi.count=7;
    const char *names[]={"Bonrix_Office","Bonrix_Guest","Home_Network","TP-Link_1234"};
    for(int i=0;i<4;++i) {
        memcpy(v.wifi.rows[i].ssid,names[i],strlen(names[i])+1);
        v.wifi.rows[i].rssi=-45-i*8;v.wifi.rows[i].supported=true;
    }
    v.wifi.connected=true;v.wifi.rssi=-45;v.wifi.connected_seconds=3723;
    memcpy(v.wifi.ssid,"Bonrix_Office",14);
    memcpy(v.wifi.ip,"192.168.1.45",13);memcpy(v.wifi.mask,"255.255.255.0",14);
    memcpy(v.wifi.gateway,"192.168.1.1",12);
    memcpy(v.wifi.dns[0],"8.8.8.8",8);memcpy(v.wifi.dns[1],"8.8.4.4",8);
    const char *message=screen==3?"Connected successfully":"Select a network. 2.4 GHz Wi-Fi only.";
    memcpy(v.wifi.message,message,strlen(message)+1);
    if(screen==UI_CONNECTING) {
        v.wifi.connected=false;v.wifi.busy=true;v.wifi.connecting=true;
        v.wifi.using_saved_credentials=true;v.wifi.stage=SETUP_GETTING_IP;
        v.wifi.connect_remaining_ms=9000;
        memcpy(v.wifi.connecting_ssid,"Bonrix_Office",14);
    }
    wifi_ui_render(pixels,font,background,&v);
}
