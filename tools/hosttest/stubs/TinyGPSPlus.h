#pragma once
#include <cstdint>
struct TGVal { bool isValid(){return false;} uint32_t age(){return 99999;} int hour(){return 0;} int minute(){return 0;} double kmph(){return 0;} int value(){return 0;} };
struct TinyGPSPlus { TGVal time, location, speed, satellites; bool encode(char){return false;} };
