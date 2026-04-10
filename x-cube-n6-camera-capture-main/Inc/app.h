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

/* ROI Register Test Functions - for testing IMX335 sensor register access */
void App_TestIMX335Registers(void);
void App_SetIMX335ROI(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height);
void App_SetIMX335ROIArea3(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height);

/* Dynamic ROI Management Functions */
/* Add ROI configuration for capturing different regions/sizes */
int App_AddROIConfig(uint16_t x_start, uint16_t y_start, 
                     uint16_t out_width, uint16_t out_height);
int App_ActivateROI(int index);
void App_DeactivateROI(void);
void App_ListROIs(void);

/* Capture with ROI - applies ROI to sensor and returns output dimensions */
int App_CaptureWithROI(uint16_t x_start, uint16_t y_start, 
                       uint16_t width, uint16_t height,
                       uint16_t *out_width, uint16_t *out_height);

/* Dynamic ROI switching functions */
int App_ChangeROI(int index);              /* Switch to ROI by index */
void App_StartROISwitcher(int interval_ms); /* Start periodic ROI switcher */
void App_StopROISwitcher(void);            /* Stop periodic ROI switcher */

#endif
