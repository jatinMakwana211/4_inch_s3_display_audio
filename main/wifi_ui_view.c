#include "wifi_ui_view.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RGB(r,g,b) ((((r)>>3)<<11) | (((g)>>2)<<5) | ((b)>>3))
#define BLUE RGB(0,76,224)
#define NAVY RGB(5,34,102)
#define INK RGB(22,44,68)
#define MUTED RGB(103,126,150)
#define PALE RGB(237,248,255)
#define BORDER RGB(204,229,247)
#define WHITE 0xffff
#define GREEN RGB(0,164,66)
#define RED RGB(208,40,58)
typedef enum { BODY, TITLE, BOLD, SMALL } font_t;
typedef struct { int x,y,w,h; } area_t;
static const area_t back={16,22,54,58}, choose={24,189,198,48},
    edit={24,280,198,48}, connect={24,343,198,48}, scan={417,140,44,44},
    previous={250,362,48,38}, next={413,362,48,38},
    configure={12,373,162,44}, refresh={181,373,122,44}, disconnect={310,373,158,44};
static uint16_t *fb;
static const uint8_t *atlas;
static const wifi_ui_view_t *view;
static const int font_w[]={18,26,20,16},font_h[]={22,32,24,20};
static const unsigned font_offset[]={0,37715,116850,162545};
static const char *keys[3][4]={
    {"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"},
    {"!@#$%^&*()","-_=+[]{}\\|",";:'\",.<>/?","`~"},
    {"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"},
};
static uint16_t blend(uint16_t a,uint16_t b,unsigned alpha)
{
    unsigned r=(((a>>11)&31)*(255-alpha)+((b>>11)&31)*alpha)/255;
    unsigned g=(((a>>5)&63)*(255-alpha)+((b>>5)&63)*alpha)/255;
    unsigned bl=((a&31)*(255-alpha)+(b&31)*alpha)/255;
    return (r<<11)|(g<<5)|bl;
}
static void pixel(int x,int y,uint16_t c,unsigned alpha)
{
    if(x>=0 && x<480 && y>=0 && y<480) fb[y*480+x]=alpha==255?c:blend(fb[y*480+x],c,alpha);
}
static void rect(int x,int y,int w,int h,uint16_t c)
{
    for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixel(xx,yy,c,255);
}
static void rounded(area_t a,int radius,uint16_t left,uint16_t right)
{
    for(int x=0;x<a.w;++x) {
        uint16_t c=blend(left,right,(unsigned)x*255/(a.w>1?a.w-1:1));
        for(int y=0;y<a.h;++y) {
            int dx=x<radius?radius-x:x>=a.w-radius?x-(a.w-radius-1):0;
            int dy=y<radius?radius-y:y>=a.h-radius?y-(a.h-radius-1):0;
            int alpha=(radius*radius-dx*dx-dy*dy+radius)*255/(radius*2);
            if(alpha>0)pixel(a.x+x,a.y+y,c,alpha>255?255:alpha);
        }
    }
}
static void disc(int x,int y,int r,uint16_t c)
{
    rounded((area_t){x-r,y-r,r*2+1,r*2+1},r,c,c);
}
static void line(int x0,int y0,int x1,int y1,int width,uint16_t c)
{
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for(;;) {
        disc(x0,y0,width,c);
        if(x0==x1 && y0==y1)break;
        int twice=2*err;
        if(twice>=dy){err+=dy;x0+=sx;}
        if(twice<=dx){err+=dx;y0+=sy;}
    }
}
static unsigned ascii(char c){unsigned n=(unsigned char)c;return n<32 || n>126?'?':n;}
static int text_width(const char *s,font_t f)
{
    int w=0;const uint8_t *adv=atlas+font_offset[f];
    for(;*s;++s)w+=adv[ascii(*s)-32];
    return w;
}
static void glyph(int x,int y,unsigned ch,font_t f,uint16_t c,int right)
{
    int w=font_w[f],h=font_h[f];
    const uint8_t *mask=atlas+font_offset[f]+95+(ch-32)*w*h;
    for(int yy=0;yy<h;++yy)for(int xx=0;xx<w && x+xx<right;++xx)
        if(mask[yy*w+xx])pixel(x+xx,y+yy,c,mask[yy*w+xx]);
}
static void label(int x,int y,int width,const char *s,font_t f,uint16_t c,bool center)
{
    int total=text_width(s,f),right=x+width;
    bool shorten=total>width;
    int dots=text_width("...",f);
    if(center && !shorten)x+=(width-total)/2;
    const uint8_t *adv=atlas+font_offset[f];
    for(;*s;++s) {
        unsigned ch=ascii(*s),w=adv[ch-32];
        if(x+(int)w>right-(shorten?dots:0))break;
        glyph(x,y,ch,f,c,right);x+=w;
    }
    if(shorten)for(int i=0;i<3;++i){glyph(x,y,'.',f,c,right);x+=adv['.'-32];}
}
static bool pressed(ui_action_t action,int value)
{
    return view->pressed.action==action && view->pressed.value==value;
}
static void surface(area_t a,uint16_t fill,bool active)
{
    rounded(a,10,active?BLUE:BORDER,active?BLUE:BORDER);
    rounded((area_t){a.x+1,a.y+1,a.w-2,a.h-2},9,active?RGB(213,235,255):fill,active?RGB(227,244,255):fill);
}
static void button(area_t a,const char *s,ui_action_t action,int value,bool primary,bool enabled)
{
    bool down=enabled && pressed(action,value);
    if(primary) {
        rounded((area_t){a.x,a.y+3,a.w,a.h},12,RGB(205,230,252),RGB(226,243,255));
        rounded(a,12,!enabled?RGB(151,189,224):down?RGB(0,51,165):RGB(0,84,245),!enabled?RGB(175,211,236):down?RGB(0,128,208):RGB(0,176,245));
    } else surface(a,WHITE,down);
    label(a.x+6,a.y+(a.h-24)/2,a.w-12,s,BOLD,primary?WHITE:enabled?BLUE:MUTED,true);
}
static void arrow(int x,int y,bool left,uint16_t c)
{
    line(x+(left?4:-4),y-5,x+(left?-2:2),y,1,c);
    line(x+(left?-2:2),y,x+(left?4:-4),y+5,1,c);
}
static void wifi_icon(int cx,int cy,int radius,uint16_t c)
{
    static const int16_t arc[][2]={{-707,-707},{-643,-766},{-574,-819},{-500,-866},{-423,-906},{-342,-940},{-259,-966},{-174,-985},{-87,-996},{0,-1000},{87,-996},{174,-985},{259,-966},{342,-940},{423,-906},{500,-866},{574,-819},{643,-766},{707,-707}};
    for(int band=0;band<3;++band) {
        int r=radius-band*(radius/3),w=radius>40?radius/12:radius>20?2:1;
        int ox=0,oy=0;
        for(unsigned i=0;i<sizeof(arc)/sizeof(arc[0]);++i) {
            int x=cx+arc[i][0]*r/1000,y=cy+arc[i][1]*r/1000;
            if(i)line(ox,oy,x,y,w,c);
            ox=x;oy=y;
        }
    }
    disc(cx,cy-2,radius>40?radius/7:radius>20?6:2,c);
}
static void lock_icon(int x,int y,uint16_t c)
{
    rounded((area_t){x-5,y-8,11,14},5,c,c);
    rounded((area_t){x-3,y-6,7,10},3,PALE,PALE);
    rounded((area_t){x-7,y,15,12},3,c,c);
    disc(x,y+4,1,WHITE);rect(x,y+4,1,4,WHITE);
}
static void eye_icon(int x,int y,bool visible,uint16_t c)
{
    line(x-10,y,x-4,y-5,1,c);line(x-4,y-5,x+4,y-5,1,c);line(x+4,y-5,x+10,y,1,c);
    line(x-10,y,x-4,y+5,1,c);line(x-4,y+5,x+4,y+5,1,c);line(x+4,y+5,x+10,y,1,c);
    disc(x,y,3,c);
    if(!visible)line(x-9,y+9,x+9,y-9,1,c);
}
static void refresh_icon(int x,int y,uint16_t c)
{
    static const int16_t arc[][2]={{766,643},{574,819},{342,940},{87,996},{-174,985},{-423,906},{-643,766},{-819,574},{-940,342},{-996,87},{-985,-174},{-906,-423},{-766,-643},{-574,-819},{-342,-940},{-87,-996},{174,-985},{423,-906},{643,-766},{819,-574}};
    int ox=0,oy=0;
    for(unsigned i=0;i<sizeof(arc)/sizeof(arc[0]);++i) {
        int xx=x+arc[i][0]*11/1000,yy=y+arc[i][1]*11/1000;
        if(i)line(ox,oy,xx,yy,1,c);
        ox=xx;oy=yy;
    }
    line(x+8,y-7,x+9,y-14,1,c);line(x+8,y-7,x+1,y-8,1,c);
}
static void spinner(int x,int y)
{
    const int px[]={0,7,10,7,0,-7,-10,-7},py[]={-10,-7,0,7,10,7,0,-7};
    for(unsigned i=0;i<8;++i)disc(x+px[i],y+py[i],2,blend(PALE,BLUE,45+((i+view->spinner_phase)%8)*30));
}
static void back_button(void)
{
    surface((area_t){16,22,46,46},PALE,pressed(UI_BACK,0));
    line(30,45,48,45,2,BLUE);line(30,45,37,38,2,BLUE);line(30,45,37,52,2,BLUE);
    label(12,70,54,"Back",BODY,INK,true);
}
static void header(const char *title,const char *subtitle)
{
    back_button();
    wifi_icon(240,54,33,BLUE);
    label(78,65,348,title,TITLE,NAVY,true);
    label(75,102,355,subtitle,BODY,MUTED,true);
}
static void card(area_t a,bool tint)
{
    rounded((area_t){a.x,a.y+3,a.w,a.h},14,RGB(225,242,253),RGB(225,242,253));
    rounded(a,14,BORDER,BORDER);
    rounded((area_t){a.x+1,a.y+1,a.w-2,a.h-2},13,tint?RGB(244,251,255):WHITE,tint?RGB(233,247,255):WHITE);
}
static void password_label(int x,int y,int width)
{
    char visible[64];size_t n=strlen(view->password);
    if(view->show_password)memcpy(visible,view->password,n+1);
    else{memset(visible,'*',n);visible[n]=0;}
    const char *tail=visible;
    while(*tail && text_width(tail,BODY)>width)++tail;
    label(x,y,width,n?tail:"Enter password...",BODY,n?INK:MUTED,false);
    memset(visible,0,sizeof(visible));
}
static bool can_connect(void)
{
    return !view->wifi.busy && view->selected.ssid[0] && view->selected.supported &&
        (view->selected.open || strlen(view->password)>=8);
}
static void status(int y)
{
    const char *s=view->notice[0]?view->notice:view->wifi.message;
    if(!*s)return;
    label(20,y,440,s,SMALL,view->notice[0]?RED:MUTED,true);
}
static void config_screen(void)
{
    header("Wi-Fi Configuration","Connect to your Wi-Fi network");
    card((area_t){12,136,222,265},true);card((area_t){244,136,224,265},false);
    wifi_icon(34,177,13,BLUE);label(54,153,169,"Select Network",BOLD,INK,false);
    surface(choose,WHITE,pressed(UI_CHOOSE,0));
    label(35,201,155,view->selected.ssid[0]?view->selected.ssid:"Choose a network...",BODY,view->selected.ssid[0]?BLUE:MUTED,false);
    line(201,210,206,215,1,BLUE);line(206,215,211,210,1,BLUE);
    lock_icon(34,257,BLUE);label(54,246,162,"Password",BOLD,INK,false);
    surface(edit,WHITE,pressed(UI_EDIT,0));
    if(view->selected.open && view->selected.ssid[0])label(34,294,177,"No password needed",BODY,MUTED,false);
    else {
        password_label(34,294,141);
        if(pressed(UI_REVEAL,0))rounded((area_t){180,284,38,40},8,PALE,PALE);
        eye_icon(201,304,view->show_password,MUTED);
    }
    button(connect,view->wifi.busy?"Please wait...":"Connect",UI_CONNECT,0,true,can_connect());
    label(256,151,161,"Available Networks",BODY,INK,false);
    if(view->wifi.busy)spinner(439,162);
    else {
        if(pressed(UI_SCAN,0))rounded(scan,10,PALE,PALE);
        refresh_icon(439,162,BLUE);
    }
    for(unsigned i=0;i<view->wifi.row_count;++i) {
        const setup_network_t *n=&view->wifi.rows[i];int y=184+i*44;
        bool chosen=!strcmp(n->ssid,view->selected.ssid) && n->auth==view->selected.auth;
        if(chosen || pressed(UI_SELECT,i))rounded((area_t){249,y,214,43},8,pressed(UI_SELECT,i)?RGB(212,234,255):PALE,PALE);
        uint16_t color=n->rssi>=-65?BLUE:RGB(143,173,201);
        wifi_icon(267,y+27,13,color);
        label(286,y+2,148,n->ssid,BODY,INK,false);
        char strength[32];snprintf(strength,sizeof(strength),"%s  %d dBm",!n->supported?"Unsupported":n->open?"Open":"Secured",n->rssi);
        label(286,y+23,147,strength,SMALL,MUTED,false);
        arrow(449,y+20,false,chosen?BLUE:MUTED);
        if(i<3)rect(256,y+43,200,1,RGB(233,243,251));
    }
    if(!view->wifi.row_count) {
        wifi_icon(355,250,23,RGB(166,196,221));
        label(254,269,204,view->wifi.busy?"Finding networks...":"No networks found",BODY,MUTED,true);
        label(254,293,204,"2.4 GHz Wi-Fi",SMALL,MUTED,true);
    }
    bool prev=view->offset>0 && !view->wifi.busy,nxt=view->offset+4<view->wifi.count && !view->wifi.busy;
    if(pressed(UI_PREVIOUS,0))rounded(previous,8,PALE,PALE);
    if(pressed(UI_NEXT,0))rounded(next,8,PALE,PALE);
    arrow(274,381,true,prev?BLUE:BORDER);arrow(437,381,false,nxt?BLUE:BORDER);
    char page[40];snprintf(page,sizeof(page),"%u / %u",view->wifi.count?view->offset/4+1:0,(view->wifi.count+3)/4);
    label(300,370,110,page,SMALL,MUTED,true);
    status(405);
}
static void keyboard_screen(void)
{
    header("Wi-Fi Password","Enter your password, then tap Done");
    label(24,131,430,view->selected.ssid,BOLD,BLUE,false);
    surface((area_t){16,160,448,48},WHITE,pressed(UI_EDIT,0));password_label(28,173,360);
    if(pressed(UI_REVEAL,0))rounded((area_t){410,164,48,40},8,PALE,PALE);
    eye_icon(435,184,view->show_password,BLUE);
    char length[48];snprintf(length,sizeof(length),"%u / 63 characters",(unsigned)strlen(view->password));
    label(22,212,430,length,SMALL,MUTED,false);
    unsigned mode=view->keyboard_page?1:view->caps?2:0;
    for(int row=0;row<4;++row) {
        const char *s=keys[mode][row];int count=strlen(s),start=(480-count*46)/2;
        for(int col=0;col<count;++col) {
            char key[2]={s[col],0};
            button((area_t){start+col*46,239+row*43,43,39},key,UI_KEY,s[col],false,true);
        }
    }
    button((area_t){8,418,70,48},view->caps?"CAPS":"Shift",UI_CAPS,0,false,true);
    button((area_t){84,418,64,48},view->keyboard_page?"ABC":"#+=",UI_SYMBOLS,0,false,true);
    button((area_t){154,418,122,48},"Space",UI_SPACE,0,false,true);
    button((area_t){282,418,86,48},"Delete",UI_DELETE,0,false,true);
    button((area_t){374,418,98,48},"Done",UI_DONE,0,true,true);
}
static void connected_screen(void)
{
    header(view->wifi.connected?"Wi-Fi Connected":"Wi-Fi Disconnected","");
    rounded((area_t){121,103,238,27},13,view->wifi.connected?GREEN:RED,view->wifi.connected?RGB(0,192,80):RED);
    label(127,105,226,view->wifi.connected?"Connected successfully":"Connection lost",BOLD,WHITE,true);
    card((area_t){12,139,456,223},false);
    char signal[40],elapsed[32];
    snprintf(signal,sizeof(signal),"%d dBm (%s)",view->wifi.rssi,view->wifi.rssi>=-50?"Excellent":view->wifi.rssi>=-65?"Good":view->wifi.rssi>=-75?"Fair":"Weak");
    unsigned seconds=view->wifi.connected_seconds;
    snprintf(elapsed,sizeof(elapsed),"%02u:%02u:%02u",seconds/3600,(seconds/60)%60,seconds%60);
    const char *labels[]={"Network (SSID)","Signal strength","IP address","Subnet mask","Gateway","DNS primary","DNS backup","Connected for"};
    const char *values[]={view->wifi.ssid,signal,view->wifi.ip,view->wifi.mask,view->wifi.gateway,view->wifi.dns[0],view->wifi.dns[1],elapsed};
    for(int i=0;i<8;++i) {
        int y=145+i*27;
        disc(30,y+10,9,PALE);
        if(i==0)wifi_icon(30,y+16,10,BLUE);
        else label(21,y,19,i==2?"IP":i==7?"T":"+",SMALL,BLUE,true);
        label(47,y,138,labels[i],BODY,INK,false);
        label(196,y,260,view->wifi.connected?values[i]:"--",BODY,i==1?GREEN:NAVY,false);
        if(i<7)rect(22,y+25,436,1,PALE);
    }
    bool ready=!view->wifi.busy;
    button(configure,"Configure Wi-Fi",UI_CONFIGURE,0,true,ready);
    button(refresh,"Refresh",UI_REFRESH,0,false,ready);
    surface(disconnect,RGB(255,239,241),pressed(UI_DISCONNECT,0));
    label(316,383,146,"Disconnect",BOLD,view->wifi.busy?MUTED:RED,true);
    status(421);
}
static void connecting_screen(void)
{
    back_button();
    wifi_icon(240,135,86,BLUE);
    label(30,154,420,"Connecting to Wi-Fi",TITLE,NAVY,true);
    label(30,192,420,"Please wait...",BODY,MUTED,true);
    card((area_t){44,224,392,158},true);
    char network[48];
    snprintf(network,sizeof(network),"Network: %s",view->wifi.connecting_ssid);
    const char *steps[]={view->wifi.using_saved_credentials?"Saved credentials loaded":"Credentials ready",
        network,"Connecting to network...","Obtaining IP address...",
        view->wifi.using_saved_credentials?"Finalizing connection...":"Saving Wi-Fi credentials..."};
    unsigned active=view->wifi.stage==SETUP_PREPARING?0:
        view->wifi.stage==SETUP_ASSOCIATING?2:view->wifi.stage==SETUP_GETTING_IP?3:4;
    for(unsigned i=0;i<5;++i) {
        int y=239+i*28;
        bool done=i<active || i==1 || view->wifi.stage==SETUP_READY;
        if(done) {
            disc(70,y+10,10,GREEN);
            line(66,y+10,69,y+13,1,WHITE);line(69,y+13,75,y+6,1,WHITE);
        } else if(i==active)spinner(70,y+10);
        else {disc(70,y+10,10,BORDER);disc(70,y+10,8,PALE);}
        label(94,y,328,steps[i],BODY,done || i==active?NAVY:MUTED,false);
    }
    rounded((area_t){52,395,376,10},5,BORDER,BORDER);
    // This is stage progress, not an invented network transfer percentage.
    rounded((area_t){52,395,60+(int)active*70,10},5,BLUE,RGB(0,183,245));
    char remaining[48];
    if(view->wifi.connected)snprintf(remaining,sizeof(remaining),"Finishing setup...");
    else snprintf(remaining,sizeof(remaining),"%u seconds remaining",(unsigned)((view->wifi.connect_remaining_ms+999)/1000));
    label(30,411,420,remaining,SMALL,MUTED,true);
}
ui_screen_t wifi_ui_connection_screen(const setup_snapshot_t *wifi)
{
    if(wifi->connected && !wifi->busy)return UI_CONNECTED;
    // An IP accepted before the deadline may still be committing to flash.
    if(wifi->connecting && ((wifi->connect_remaining_ms>0 && wifi->stage!=SETUP_FAILED) ||
        (wifi->connected && (wifi->stage==SETUP_SAVING || wifi->stage==SETUP_READY))))return UI_CONNECTING;
    return UI_CONFIG;
}
static void settings_icon(void)
{
    int x=372,y=34;
    surface((area_t){343,7,58,54},WHITE,pressed(UI_SETTINGS_OPEN,0));
    disc(x,y,15,BLUE);
    const int dx[]={0,14,20,14,0,-14,-20,-14},dy[]={-20,-14,0,14,20,14,0,-14};
    for(int i=0;i<8;++i)line(x+dx[i]*3/4,y+dy[i]*3/4,x+dx[i],y+dy[i],3,BLUE);
    disc(x,y,7,WHITE);
}
static void wrapped(int x,int y,int width,int rows,const char *text,uint16_t color)
{
    const uint8_t *adv=atlas+font_offset[BODY];
    char lines[4][128];int count=0;
    if(rows>4)rows=4;
    while(*text) {
        int w=0,n=0,space=-1;
        while(text[n] && text[n]!='\n' && n<127) {
            int a=adv[ascii(text[n])-32];if(w+a>width)break;
            w+=a;if(text[n]==' ')space=n;++n;
        }
        if(text[n] && text[n]!='\n' && space>0)n=space;
        if(!n && *text!='\n')n=1;
        if(count==rows){memmove(lines,lines+1,(rows-1)*sizeof(lines[0]));--count;}
        memcpy(lines[count],text,n);lines[count++][n]=0;text+=n;
        if(*text=='\n')++text;
        while(*text==' ')++text;
    }
    for(int row=0;row<count;++row)label(x,y+row*25,width,lines[row],BODY,color,false);
}
static void settings_screen(void)
{
    header("Settings","Voice assistant preferences");
    card((area_t){24,155,432,90},false);
    label(44,187,240,"Agent Mode",BOLD,INK,false);
    uint16_t color=view->agent_enabled?RGB(158,43,255):RGB(211,225,235);
    rounded((area_t){340,180,94,44},22,color,color);
    disc(view->agent_enabled?412:362,202,18,WHITE);
    label(view->agent_enabled?345:381,193,40,view->agent_enabled?"ON":"OFF",SMALL,WHITE,true);
    wrapped(40,274,400,3,"Turn on, then tap Back to open the Agent screen.",MUTED);
    if(view->notice[0])wrapped(40,380,400,3,view->notice,RED);
}
static void agent_screen(void)
{
    back_button();settings_icon();
    wifi_icon(443,48,27,BLUE);
    label(88,31,245,"Bonrix Agent",TITLE,NAVY,true);
    wrapped(24,105,432,2,view->agent.status[0]?view->agent.status:"Connecting to Bonrix...",BLUE);
    card((area_t){20,169,440,112},false);
    label(36,181,400,"You",BOLD,BLUE,false);
    wrapped(36,214,408,2,view->agent.user[0]?view->agent.user:"No conversation active",INK);
    card((area_t){20,294,440,161},true);
    label(36,306,400,"Bonrix",BOLD,BLUE,false);
    wrapped(36,339,408,4,view->agent.assistant,INK);
}
void wifi_ui_welcome(uint16_t *pixels,const uint8_t *font,const uint8_t *welcome,const wifi_ui_view_t *v)
{
    fb=pixels; atlas=font; view=v;
    memcpy(fb,welcome,480*480*sizeof(*fb));
    settings_icon();
}
void wifi_ui_render(uint16_t *pixels,const uint8_t *font,const uint16_t *background,const wifi_ui_view_t *v)
{
    fb=pixels;atlas=font;view=v;
    memcpy(fb,background,480*480*sizeof(*fb));
    if(v->screen==UI_CONFIG)config_screen();
    else if(v->screen==UI_PASSWORD){rect(0,232,480,248,RGB(232,243,253));keyboard_screen();}
    else if(v->screen==UI_CONNECTED)connected_screen();
    else if(v->screen==UI_CONNECTING)connecting_screen();
    else if(v->screen==UI_SETTINGS)settings_screen();
    else if(v->screen==UI_AGENT)agent_screen();

}
static bool in(int x,int y,area_t a){return x>=a.x && x<a.x+a.w && y>=a.y && y<a.y+a.h;}
ui_hit_t wifi_ui_hit(const wifi_ui_view_t *v,int x,int y)
{
    if(v->screen==UI_WELCOME || v->screen==UI_AGENT) {
        if(in(x,y,(area_t){343,7,58,54}))return (ui_hit_t){UI_SETTINGS_OPEN,0};
        if(in(x,y,(area_t){405,0,75,70}))return (ui_hit_t){UI_OPEN,0};
        if(v->screen==UI_WELCOME)return (ui_hit_t){UI_NONE,0};
    }
    if(in(x,y,back))return (ui_hit_t){UI_BACK,0};
    if(v->screen==UI_SETTINGS)return (ui_hit_t){in(x,y,(area_t){340,180,94,44})?UI_AGENT_TOGGLE:UI_NONE,0};
    if(v->screen==UI_AGENT)return (ui_hit_t){UI_NONE,0};
    if(v->screen==UI_PASSWORD) {
        if(in(x,y,(area_t){410,160,54,48}))return (ui_hit_t){UI_REVEAL,0};
        unsigned mode=v->keyboard_page?1:v->caps?2:0;
        for(int row=0;row<4;++row) {
            const char *s=keys[mode][row];int count=strlen(s),start=(480-count*46)/2;
            for(int col=0;col<count;++col)if(in(x,y,(area_t){start+col*46,239+row*43,43,39}))return (ui_hit_t){UI_KEY,s[col]};
        }
        if(in(x,y,(area_t){8,418,70,48}))return (ui_hit_t){UI_CAPS,0};
        if(in(x,y,(area_t){84,418,64,48}))return (ui_hit_t){UI_SYMBOLS,0};
        if(in(x,y,(area_t){154,418,122,48}))return (ui_hit_t){UI_SPACE,0};
        if(in(x,y,(area_t){282,418,86,48}))return (ui_hit_t){UI_DELETE,0};
        if(in(x,y,(area_t){374,418,98,48}))return (ui_hit_t){UI_DONE,0};
    }
    if(v->wifi.busy)return (ui_hit_t){UI_NONE,0};
    if(v->screen==UI_CONFIG) {
        if(in(x,y,scan))return (ui_hit_t){UI_SCAN,0};
        if(in(x,y,choose))return (ui_hit_t){UI_CHOOSE,0};
        if(in(x,y,edit))return (ui_hit_t){x>=178 && !v->selected.open?UI_REVEAL:UI_EDIT,0};
        if(in(x,y,connect) && v->selected.ssid[0] && v->selected.supported && (v->selected.open || strlen(v->password)>=8))return (ui_hit_t){UI_CONNECT,0};
        if(in(x,y,previous) && v->offset>0)return (ui_hit_t){UI_PREVIOUS,0};
        if(in(x,y,next) && v->offset+4<v->wifi.count)return (ui_hit_t){UI_NEXT,0};
        for(unsigned i=0;i<v->wifi.row_count;++i)if(in(x,y,(area_t){249,184+i*44,214,44}))return (ui_hit_t){UI_SELECT,i};
    }
    if(v->screen==UI_CONNECTED) {
        if(in(x,y,configure))return (ui_hit_t){UI_CONFIGURE,0};
        if(in(x,y,refresh))return (ui_hit_t){UI_REFRESH,0};
        if(in(x,y,disconnect) && v->wifi.connected)return (ui_hit_t){UI_DISCONNECT,0};
    }
    return (ui_hit_t){UI_NONE,0};
}
static bool same(ui_hit_t a,ui_hit_t b){return a.action==b.action && a.value==b.value;}
ui_hit_t wifi_ui_touch(wifi_ui_view_t *v,ui_touch_state_t *t,bool down,int x,int y,uint32_t ms)
{
    ui_hit_t none={UI_NONE,0};
    if(down) {
        ui_hit_t hit=wifi_ui_hit(v,x,y);
        if(!t->held) {
            *t=(ui_touch_state_t){.held=true,.x=x,.y=y,.began=ms,.screen=v->screen,.target=hit};
        } else if(v->screen!=t->screen || !same(hit,t->target) || abs(x-t->x)>18 || abs(y-t->y)>18)t->cancelled=true;
        t->releasing=false;
        v->pressed=t->cancelled?none:t->target;
    } else if(t->held) {
        if(!t->releasing){t->releasing=true;t->released=ms;}
        if((uint32_t)(ms-t->released)>=16) {
            ui_hit_t result=(!t->cancelled && v->screen==t->screen && (uint32_t)(ms-t->began)>=16 &&
                same(wifi_ui_hit(v,t->x,t->y),t->target))?t->target:none;
            *t=(ui_touch_state_t){0};v->pressed=none;return result;
        }
    }
    return none;
}
void wifi_ui_edit(wifi_ui_view_t *v,ui_hit_t hit)
{
    size_t length=strlen(v->password);
    if(hit.action==UI_KEY && length<63 && hit.value>=33 && hit.value<=126){v->password[length]=hit.value;v->password[length+1]=0;}
    else if(hit.action==UI_SPACE && length<63){v->password[length]=' ';v->password[length+1]=0;}
    else if(hit.action==UI_DELETE && length)v->password[length-1]=0;
    else if(hit.action==UI_CAPS)v->caps=!v->caps;
    else if(hit.action==UI_SYMBOLS)v->keyboard_page=!v->keyboard_page;
    else if(hit.action==UI_REVEAL)v->show_password=!v->show_password;
}
