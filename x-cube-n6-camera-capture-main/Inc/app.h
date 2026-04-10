 /**
 ******************************************************************************
 * @file    app.h
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>

#define USE_DCACHE

void app_run(void);

/* ROI Configuration structure - must be visible to main.c */
typedef struct {
  uint16_t x_start;      /* ROI start X on sensor */
  uint16_t y_start;      /* ROI start Y on sensor */
  uint16_t width;        /* Output width */
  uint16_t height;       /* Output height */
  uint16_t sensor_width; /* Actual sensor readout width */
  uint16_t sensor_height;/* Actual sensor readout height */
  uint8_t  active;       /* Is this ROI configuration active */
} ROI_Config_t;

/* ROI Register Test Functions */
void App_TestIMX335Registers(void);
void App_SetIMX335ROI(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height);
void App_SetIMX335ROIArea3(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height);

/* Dynamic ROI Management Functions */
ROI_Config_t* App_GetCurrentROI(void);
int App_AddROIConfig(uint16_t x_start, uint16_t y_start, 
                     uint16_t out_width, uint16_t out_height);
int App_ActivateROI(int index);
void App_DeactivateROI(void);
void App_ListROIs(void);
int App_CaptureWithROI(uint16_t x_start, uint16_t y_start, 
                       uint16_t width, uint16_t height,
                       uint16_t *out_width, uint16_t *out_height);
int App_ChangeROI(int index);
void App_StartROISwitcher(int interval_ms);
void App_StopROISwitcher(void);

/* Camera Power and I2C functions */
int App_Camera_PowerOn(void);
int App_I2C_Init(void);
int App_IMX335_WriteReg(uint16_t reg, uint8_t *data, uint16_t len);
int App_IMX335_ReadReg(uint16_t reg, uint8_t *data, uint16_t len);

#endif
