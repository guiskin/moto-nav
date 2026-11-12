#pragma once
#include "Wire.h"
#define AXP2101_SLAVE_ADDRESS 0x34
struct XPowersPMU { bool begin(TwoWire&,int,int,int){return false;} void enableBattDetection(){} void enableBattVoltageMeasure(){} bool isBatteryConnect(){return false;} int getBatteryPercent(){return 0;} bool isCharging(){return false;} };
