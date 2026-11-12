#pragma once
#include "Arduino.h"
#include "fontmetrics.h"
#include <string>
#include <vector>
extern const uint8_t u8g2_font_helvB14_tf[], u8g2_font_helvB18_tf[], u8g2_font_helvB24_tf[], u8g2_font_logisoso42_tr[], u8g2_font_logisoso92_tn[], u8g2_font_logisoso32_tr[];
struct TextRec { int x, y; uint16_t c; int f; std::string t; };
extern std::vector<TextRec> g_texts;
struct Arduino_DataBus {};
struct Arduino_ESP32QSPI : Arduino_DataBus { Arduino_ESP32QSPI(int,int,int,int,int,int){} };
struct Arduino_CO5300 { Arduino_CO5300(Arduino_DataBus*,int,int,int,int,int,int,int,int){} void setBrightness(int){} };
struct Arduino_Canvas {
  int W,H; uint16_t* fb; int cx=0, cy=0; uint16_t tc=0xFFFF; int charW=12;
  Arduino_Canvas(int w,int h,Arduino_CO5300*):W(w),H(h){ fb=(uint16_t*)calloc(w*h,2);} 
  bool begin(){return true;} void flush(){} void setUTF8Print(bool){} void setTextWrap(bool){}
  void px(int x,int y,uint16_t c){ if(x>=0&&y>=0&&x<W&&y<H) fb[y*W+x]=c; }
  void fillScreen(uint16_t c){ for(int i=0;i<W*H;i++) fb[i]=c; g_texts.clear(); }
  void fillRect(int x,int y,int w,int h,uint16_t c){ for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) px(i,j,c); }
  void drawRect(int x,int y,int w,int h,uint16_t c){ for(int i=x;i<x+w;i++){px(i,y,c);px(i,y+h-1,c);} for(int j=y;j<y+h;j++){px(x,j,c);px(x+w-1,j,c);} }
  void drawFastHLine(int x,int y,int w,uint16_t c){ for(int i=x;i<x+w;i++) px(i,y,c); }
  void fillRoundRect(int x,int y,int w,int h,int,uint16_t c){ fillRect(x,y,w,h,c); }
  void drawRoundRect(int x,int y,int w,int h,int,uint16_t c){ drawRect(x,y,w,h,c); }
  void drawLine(int x0,int y0,int x1,int y1,uint16_t c){ int n=max(abs(x1-x0),abs(y1-y0)); for(int i=0;i<=n;i++){ px(x0+(x1-x0)*i/max(n,1), y0+(y1-y0)*i/max(n,1), c);} }
  void fillCircle(int x,int y,int r,uint16_t c){ for(int j=-r;j<=r;j++) for(int i=-r;i<=r;i++) if(i*i+j*j<=r*r) px(x+i,y+j,c); }
  void drawCircle(int x,int y,int r,uint16_t c){ for(int a=0;a<720;a++){ float t=a*PI/360; px(x+lroundf(r*cosf(t)), y+lroundf(r*sinf(t)), c);} }
  void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
    int minx=max(0,min(x0,min(x1,x2))), maxx=min(W-1,max(x0,max(x1,x2))), miny=max(0,min(y0,min(y1,y2))), maxy=min(H-1,max(y0,max(y1,y2)));
    for(int y=miny;y<=maxy;y++) for(int x=minx;x<=maxx;x++){
      long d0=(long)(x1-x0)*(y-y0)-(long)(y1-y0)*(x-x0), d1=(long)(x2-x1)*(y-y1)-(long)(y2-y1)*(x-x1), d2=(long)(x0-x2)*(y-y2)-(long)(y0-y2)*(x-x2);
      bool neg=(d0<0)||(d1<0)||(d2<0), pos=(d0>0)||(d1>0)||(d2>0); if(!(neg&&pos)) px(x,y,c);} }
  int fi=2;
  void setFont(const uint8_t* f){ fi = f==u8g2_font_helvB14_tf?0: f==u8g2_font_helvB18_tf?1: f==u8g2_font_helvB24_tf?2: f==u8g2_font_logisoso42_tr?3: f==u8g2_font_logisoso32_tr?5:4; }
  void setTextColor(uint16_t c){ tc=c; }
  void setCursor(int x,int y){ cx=x; cy=y; }
  int getCursorX(){ return cx; }
  void print(const char* s){ int start=cx; const unsigned char*p=(const unsigned char*)s; while(*p){ unsigned cp=*p; if(cp>=0xC0 && p[1]){ cp=((cp&0x1F)<<6)|(p[1]&0x3F); p+=2;} else p++; if(cp>255) cp='?'; cx+=FONT_W[fi][cp]; } if(cy>-500) g_texts.push_back({start,cy,tc,fi,s}); }
};
