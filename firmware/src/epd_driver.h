#ifndef INKSIGHT_EPD_DRIVER_H
#define INKSIGHT_EPD_DRIVER_H

#include <Arduino.h>

// Initialize GPIO pins and SPI for EPD
void gpioInit();

// Initialize EPD controller (full refresh mode)
void epdInit();

// Initialize EPD controller in fast refresh mode
void epdInitFast();

// Full-screen display with full refresh (clears ghosting, has black-white flash)
void epdDisplay(const uint8_t *image);

// Deep clear: multi-cycle black/white flush then display image (eliminates stubborn ghosting)
void epdDisplayDeepClear(const uint8_t *image);

// Full-screen display with pre-packed 2bpp data (4-color panels)
void epdDisplay2bpp(const uint8_t *image2bpp);

// Streaming 2bpp upload for row-addressed panels (398 NZ07).
// Lets the caller push the frame into panel RAM while it is still downloading,
// so the MCU never has to hold the full 106 KB frame buffer. This is *not*
// partial refresh: the panel is refreshed once, full screen, by Flush().
bool epdStream2bppBegin();
void epdStream2bppRow(int y, const uint8_t *row2bpp);
void epdStream2bppFlush();   // full-screen refresh + power off
void epdStream2bppAbort();   // discard: restore window + power off, no refresh

// Full-screen display with fast refresh (reduced flashing)
void epdDisplayFast(const uint8_t *image);

// Partial display refresh for a rectangular region
bool epdSupportsPartialRefresh();
void epdPartialDisplay(uint8_t *data, int xStart, int yStart, int xEnd, int yEnd);
void epdPartialDisplayWithOld(uint8_t *data, const uint8_t *oldData, int xStart, int yStart, int xEnd, int yEnd);

// Put EPD into deep sleep mode
void epdSleep();

#endif // INKSIGHT_EPD_DRIVER_H
