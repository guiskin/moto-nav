#include "motonav_src.cpp"
#include <vector>
uint32_t g_millis = 0; SerialStub Serial; TwoWire Wire; UpdateClass Update; EspClass ESP; ble_hs_cfg_t ble_hs_cfg;
const uint8_t u8g2_font_logisoso42_tr[1]={0}, u8g2_font_logisoso92_tn[1]={0}, u8g2_font_helvB24_tf[1]={0}, u8g2_font_helvB18_tf[1]={0}, u8g2_font_helvB14_tf[1]={0};
struct L { uint8_t k; std::vector<std::pair<int,int>> p; };
void put16(std::vector<uint8_t>&b,uint16_t v){b.push_back(v&255);b.push_back(v>>8);}
void put32(std::vector<uint8_t>&b,uint32_t v){for(int i=0;i<4;i++)b.push_back((v>>(8*i))&255);}
void savePPM(const char*fn){ FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",LCD_W,LCD_H); for(int i=0;i<LCD_W*LCD_H;i++){uint16_t c=gfx->fb[i]; uint8_t r=((c>>11)&31)*255/31,g=((c>>5)&63)*255/63,bl=(c&31)*255/31; fputc(r,f);fputc(g,f);fputc(bl,f);} fclose(f);}
int main(){
  std::vector<L> lines;
  L route{0,{{0,0},{0,400},{300,400},{300,800},{-200,1100}}};
  uint16_t id=77; uint16_t session=4242;
  auto send=[&](std::vector<uint8_t>& blob){ static uint16_t tid=1; tid++;
    { std::vector<uint8_t> p={0x10}; put16(p,tid); put32(p,blob.size()); handleMapPacket(p.data(),p.size()); }
    for(size_t off=0;off<blob.size();off+=175){ std::vector<uint8_t> p={0x11}; put16(p,tid); put32(p,off); size_t e=std::min(blob.size(),off+175); p.insert(p.end(),blob.begin()+off,blob.begin()+e); handleMapPacket(p.data(),p.size()); }
    { std::vector<uint8_t> p={0x12}; put16(p,tid); handleMapPacket(p.data(),p.size()); } };
  auto enc=[&](std::vector<uint8_t>& b, std::vector<L>& ls){ put16(b,ls.size()); for(auto&l:ls){ b.push_back(l.k); put16(b,l.p.size()); put32(b,(uint32_t)l.p[0].first); put32(b,(uint32_t)l.p[0].second); for(size_t i=1;i<l.p.size();i++){ put16(b,(uint16_t)(int16_t)(l.p[i].first-l.p[i-1].first)); put16(b,(uint16_t)(int16_t)(l.p[i].second-l.p[i-1].second)); } } };
  { std::vector<L> r={route}; std::vector<uint8_t> b={'M','N','R','2'}; put16(b,session); enc(b,r); send(b); }
  for(int tx=-1;tx<=1;tx++) for(int ty=-1;ty<=2;ty++){
    std::vector<L> ls; int x0=tx*800,y0=ty*800;
    for(int x=x0;x<x0+800;x+=100){ L l{(uint8_t)(x==0?2:4),{{x,y0},{x,y0+800}}}; ls.push_back(l);} 
    for(int y=y0;y<y0+800;y+=100){ L l{(uint8_t)(y==600?1:4),{{x0,y},{x0+800,y}}}; ls.push_back(l);} 
    std::vector<uint8_t> b={'M','N','T','2'}; put16(b,session); put16(b,(uint16_t)(int16_t)tx); put16(b,(uint16_t)(int16_t)ty); enc(b,ls); send(b); }
  id=session;
  int used=0; for(int i=0;i<MAX_TILES;i++) used+=world.tiles[i].used;
  printf("session=%d has=%d route n=%u tiles=%d progress=%d\n", world.session, world.hasSession, world.route.n, used, (int)mapProgress);
  std::vector<uint8_t> nv={1,1,3,0}; put16(nv,90); put32(nv,120); put32(nv,1500); put16(nv,4); nv.push_back(40); nv.push_back(0); const char*st="Rua Augusta"; nv.insert(nv.end(),st,st+strlen(st)); handleNavPacket(nv.data(),nv.size());
  { std::vector<uint8_t> a={0x03,1}; put16(a,(uint16_t)(int16_t)-70); put16(a,140); handleAlertPacket(a.data(),a.size()); }
  NavData n = navShared;
  float speed=11.0f; int saved=0; int shots[]={2000,20000,38000,60000,85000};
  for(g_millis=1000; g_millis<=90000; g_millis+=40){
    if(g_millis%1000==0){ float along=speed*(g_millis-1000)/1000.0f; float x,y; routePointAt(world.route,along,x,y); float h=routeBearingAt(world.route,along); if(h<0)h+=360;
      std::vector<uint8_t> p={0x02}; put16(p,id); put32(p,(uint32_t)(int32_t)lroundf(x*10)); put32(p,(uint32_t)(int32_t)lroundf(y*10)); put16(p,(uint16_t)lroundf(h*100)); put16(p,(uint16_t)lroundf(speed*100)); put32(p,(uint32_t)(int32_t)lroundf(along*10)); handlePosPacket(p.data(),p.size()); }
    { std::vector<uint8_t> a={0x03,1}; put16(a,(uint16_t)(int16_t)-70); put16(a,140); handleAlertPacket(a.data(),a.size()); }
    PosTarget t=posShared; drawMapScreen(n,t,g_millis);
    for(int s:shots) if((int)g_millis==s){ char fn[64]; snprintf(fn,64,"frame_%d.ppm",s); savePPM(fn); printf("t=%d cam x=%.1f y=%.1f along=%.1f heading=%.1f mpp=%.2f\n",s,cam.x,cam.y,cam.along,cam.heading,cam.mpp); }
  }
  { std::vector<uint8_t> a={0x03,1}; put16(a,(uint16_t)(int16_t)110); put16(a,90); handleAlertPacket(a.data(),a.size()); }
  approaching=true; approachStart=millis(); drawMapScreen(n,posShared,g_millis); drawAlertRing(); savePPM("alert_map.ppm");
  { std::vector<uint8_t> st={0x04,60}; put16(st,240); st.push_back(0); const char* sn="Av. Paulista"; st.insert(st.end(),sn,sn+strlen(sn)); handleStatusPacket(st.data(),st.size()); }
  n.speedKmh=72; drawNavScreen(n); drawAlertRing(); savePPM("nav.ppm");
  { Toast t={}; t.active=true; t.until=millis()+5000; strcpy(t.app,"WhatsApp"); strcpy(t.title,"Maria Silva"); strcpy(t.msg,"Chegando em 10 minutos, me espera na portaria do prédio"); toastShared=t; }
  drawMapScreen(n,posShared,g_millis); drawToast(); savePPM("toast.ppm");
  toastShared.active=false; musicShared.available=true; musicShared.playing=true; strcpy(musicShared.title,"Born to Be Wild"); strcpy(musicShared.artist,"Steppenwolf"); drawMusicScreen(); savePPM("music.ppm");
  otaProgress=42; drawOtaScreen(); savePPM("ota.ppm");
  batteryPct=76; GpsData g={}; drawIdleScreen(g,true); savePPM("idle.ppm");
  n.flags=0x02; nightMode=true; drawMapScreen(n,posShared,g_millis); savePPM("cruise.ppm");
}
