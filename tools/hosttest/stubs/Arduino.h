#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <algorithm>
using std::min; using std::max;
typedef bool boolean;
#define IRAM_ATTR
#define INPUT_PULLUP 2
#define FALLING 2
#define PI 3.14159265358979f
#define DEG_TO_RAD 0.017453292519943295f
#define RAD_TO_DEG 57.29577951308232f
#define RGB565_BLACK 0x0000
#define RGB565_WHITE 0xFFFF
template<class T, class L, class H> T constrain(T x, L lo, H hi){ return x<lo?(T)lo:(x>hi?(T)hi:x); }
extern uint32_t g_millis;
inline uint32_t millis(){ return g_millis; }
inline void delay(uint32_t){}
inline void pinMode(int,int){}
inline int digitalRead(int){ return 1; }
inline int digitalPinToInterrupt(int p){ return p; }
inline void attachInterrupt(int, void(*)(), int){}
struct SerialStub { void begin(int){} void println(const char*s){ puts(s);} };
extern SerialStub Serial;
struct StringStub{};
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x)
#define portEXIT_CRITICAL(x)
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
#define portMAX_DELAY 0xffffffff
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
inline SemaphoreHandle_t xSemaphoreCreateMutex(){ return (void*)1; }
inline SemaphoreHandle_t xSemaphoreCreateBinary(){ return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t){ return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t){ return 1; }
inline QueueHandle_t xQueueCreate(int,int){ return (void*)1; }
inline int xQueueSend(QueueHandle_t, const void*, uint32_t){ return 1; }
inline int xQueueReceive(QueueHandle_t, void*, uint32_t){ return 0; }
inline void vTaskDelay(uint32_t){}
inline int xTaskCreatePinnedToCore(void(*)(void*), const char*, int, void*, int, void*, int){ return 1; }
