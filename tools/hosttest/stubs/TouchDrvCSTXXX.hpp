#pragma once
#include "Wire.h"
#define CST92XX_SLAVE_ADDRESS 0x5A
struct TouchDrvCST92xx { void setPins(int,int){} bool begin(TwoWire&, uint8_t){return false;} uint8_t getPoint(int16_t*,int16_t*,uint8_t){return 0;} void setMaxCoordinates(int,int){} void setMirrorXY(bool,bool){} };
