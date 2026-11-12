#pragma once
#include "Arduino.h"
#define U_FLASH 0
struct UpdateClass { bool begin(size_t,int){return true;} size_t write(uint8_t*,size_t n){return n;} bool end(bool){return true;} void abort(){} bool isRunning(){return false;} int getError(){return 0;} };
extern UpdateClass Update;
struct EspClass { void restart(){} };
extern EspClass ESP;
