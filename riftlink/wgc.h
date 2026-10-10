#pragma once
#include <windows.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int wgc_start(HWND hwnd, char* err, size_t errLen);
int wgc_frame(const uint32_t** px, int* w, int* h);  /* newest frame since the last call: BGRA, top-down */
void wgc_stop(void);
#ifdef __cplusplus
}
#endif
