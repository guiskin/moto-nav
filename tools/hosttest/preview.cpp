#include "motonav_src.cpp"
#include <vector>
#include <string>
#include <cmath>
uint32_t g_millis = 0; SerialStub Serial; TwoWire Wire; UpdateClass Update; EspClass ESP; ble_hs_cfg_t ble_hs_cfg;
std::vector<TextRec> g_texts;
const uint8_t u8g2_font_logisoso42_tr[1]={0}, u8g2_font_logisoso92_tn[1]={0}, u8g2_font_helvB24_tf[1]={0}, u8g2_font_helvB18_tf[1]={0}, u8g2_font_helvB14_tf[1]={0}, u8g2_font_logisoso32_tr[1]={0};
struct L { uint8_t k; std::vector<std::pair<int,int>> p; };
void put16(std::vector<uint8_t>&b,uint16_t v){b.push_back(v&255);b.push_back(v>>8);}
void put32(std::vector<uint8_t>&b,uint32_t v){for(int i=0;i<4;i++)b.push_back((v>>(8*i))&255);}
void save(const char*base){ char fn[64]; snprintf(fn,64,"%s.ppm",base); FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",LCD_W,LCD_H); for(int i=0;i<LCD_W*LCD_H;i++){uint16_t c=gfx->fb[i]; fputc(((c>>11)&31)*255/31,f);fputc(((c>>5)&63)*255/63,f);fputc((c&31)*255/31,f);} fclose(f);
  snprintf(fn,64,"%s.txt",base); f=fopen(fn,"w"); for(auto&t:g_texts) fprintf(f,"%d\t%d\t%u\t%d\t%s\n",t.x,t.y,t.c,t.f,t.t.c_str()); fclose(f); }
void sendBlob(std::vector<uint8_t>& blob){ static uint16_t tid=1; tid++;
  { std::vector<uint8_t> p={0x10}; put16(p,tid); put32(p,blob.size()); handleMapPacket(p.data(),p.size()); }
  for(size_t off=0;off<blob.size();off+=200){ std::vector<uint8_t> p={0x11}; put16(p,tid); put32(p,off); size_t e=std::min(blob.size(),off+200); p.insert(p.end(),blob.begin()+off,blob.begin()+e); handleMapPacket(p.data(),p.size()); }
  { std::vector<uint8_t> p={0x12}; put16(p,tid); handleMapPacket(p.data(),p.size()); } }
void enc(std::vector<uint8_t>& b, std::vector<L>& ls){ put16(b,ls.size()); for(auto&l:ls){ b.push_back(l.k); put16(b,l.p.size()); put32(b,(uint32_t)l.p[0].first); put32(b,(uint32_t)l.p[0].second); for(size_t i=1;i<l.p.size();i++){ put16(b,(uint16_t)(int16_t)(l.p[i].first-l.p[i-1].first)); put16(b,(uint16_t)(int16_t)(l.p[i].second-l.p[i-1].second)); } } }
std::pair<int,int> R(double x,double y){ double a=0.21; return {(int)lround(x*cos(a)-y*sin(a)),(int)lround(x*sin(a)+y*cos(a))}; }
int main(){
  i2cMutex=xSemaphoreCreateMutex(); mapMutex=xSemaphoreCreateMutex();
  uint16_t session=777;
  std::vector<L> route={{0,{R(0,-300),R(0,420),R(260,420),R(520,420),R(700,560)}}};
  { std::vector<uint8_t> b={'M','N','R','2'}; put16(b,session); enc(b,route); sendBlob(b); }
  std::vector<L> ls; srand(3);
  for(int i=-8;i<=10;i++){ double x=i*105+((i*37)%23); L l{4,{}}; for(int y=-900;y<=1300;y+=220) l.p.push_back(R(x+((y*i)%17)*0.6,y)); if(i==0) l.k=3; ls.push_back(l);} 
  for(int j=-8;j<=12;j++){ double y=j*98+((j*53)%19); L l{4,{}}; for(int x=-900;x<=1200;x+=200) l.p.push_back(R(x,y+((x*j)%13)*0.5)); if(j==4) l.k=2; ls.push_back(l);} 
  { L l{2,{}}; for(int t=-6;t<=10;t++) l.p.push_back(R(520+t*90*0.79, 420+t*90*0.61)); ls.push_back(l);} 
  { L l{1,{}}; for(int t=0;t<=40;t++){ double a=-1.2+t*0.06; l.p.push_back(R(-1400+1700*cos(a), 300+1700*sin(a)*0.55)); } ls.push_back(l);} 
  { std::vector<uint8_t> b={'M','N','T','2'}; put16(b,session); put16(b,0); put16(b,0); enc(b,ls); sendBlob(b); }
  std::vector<uint8_t> nv={1,1,3,0}; put16(nv,90); put32(nv,180); put32(nv,3400); put16(nv,9); nv.push_back(42); nv.push_back(0); const char*st="Avenida Paulista"; nv.insert(nv.end(),st,st+strlen(st)); handleNavPacket(nv.data(),nv.size());
  { std::vector<uint8_t> s={0x04,50}; put16(s,0xFFFF); s.push_back(0); const char* sn="Rua Augusta"; s.insert(s.end(),sn,sn+strlen(sn)); handleStatusPacket(s.data(),s.size()); }
  NavData n=navShared; float speed=11.7f;
  for(g_millis=1000; g_millis<=24000; g_millis+=40){
    if(g_millis%1000==0){ float along=30+speed*(g_millis-1000)/1000.0f; float x,y; routePointAt(world.route,along,x,y); float h=routeBearingAt(world.route,along); if(h<0)h+=360;
      std::vector<uint8_t> p={0x02}; put16(p,session); put32(p,(uint32_t)(int32_t)lroundf(x*10)); put32(p,(uint32_t)(int32_t)lroundf(y*10)); put16(p,(uint16_t)lroundf(h*100)); put16(p,(uint16_t)lroundf(speed*100)); put32(p,(uint32_t)(int32_t)lroundf(along*10)); handlePosPacket(p.data(),p.size());
      { std::vector<uint8_t> s={0x04,50}; put16(s,0xFFFF); s.push_back(0); const char* sn="Rua Augusta"; s.insert(s.end(),sn,sn+strlen(sn)); handleStatusPacket(s.data(),s.size()); } }
    nightMode=true; drawMapScreen(n,posShared,g_millis);
  }
  save("night_map");
  nightMode=true; drawNavScreen(n); save("night_arrow");
  nightMode=false; drawMapScreen(n,posShared,g_millis); save("day_map");
  batteryPct=76;
  { std::vector<uint8_t> st={0x04,60}; put16(st,180); st.push_back(0); const char* sn="Rua Augusta"; st.insert(st.end(),sn,sn+strlen(sn)); handleStatusPacket(st.data(),st.size()); }
  lastRadarDist=0xFFFF; updateRadarFlash(g_millis); nightMode=false;
  drawMapScreen(n,posShared,g_millis); drawRadarFlash(g_millis); drawBatteryIcon(); save("radar_map");
  g_millis+=250; drawMapScreen(n,posShared,g_millis); drawRadarFlash(g_millis); drawBatteryIcon(); save("radar_map2");
  drawNavScreen(n); drawRadarFlash(g_millis); drawBatteryIcon(); save("radar_arrow");
  GpsData gg={}; gg.timeValid=true; gg.hour=10; gg.minute=28; drawIdleScreen(gg,true); drawBatteryIcon(); save("idle_batt");
}
