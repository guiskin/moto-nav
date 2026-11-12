#pragma once
#include "Arduino.h"
struct TwoWire { void setBufferSize(int){} bool begin(int,int,int){return true;} void beginTransmission(int){} size_t write(const uint8_t*,size_t n){return n;} int endTransmission(){return 1;} size_t requestFrom(int,size_t){return 0;} int read(){return 0;} };
extern TwoWire Wire;
