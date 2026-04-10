/**
 ******************************************************************************
 * @file    app_cam.h
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
#ifndef APP_CAM
#define APP_CAM

#include <stdint.h>

typedef struct {
  int capture_width;
  int capture_height;
  int fps;
  int dcmipp_output_format;
  int is_rgb_swap;
} CAM_conf_t;

void CAM_Init(CAM_conf_t *conf);
void CAM_CapturePipe_Start(uint8_t *capture_pipe_dst, uint32_t cam_mode);
void CAM_IspUpdate(void);
void CAM_Deinit(void);

/* ROI Management Functions */

/* ROI Definition structure */
typedef struct {
  uint32_t x;         /* X start position on sensor */
  uint32_t y;         /* Y start position on sensor */
  uint32_t width;     /* Output width */
  uint32_t height;    /* Output height */
} ROI_Def_t;

/* Get current ROI index (0-7) */
int CAM_GetCurrentROIIndex(void);

/* Get ROI definition by index */
const ROI_Def_t* CAM_GetROIDefinition(int index);

/* Get number of configured ROIs */
int CAM_GetNumROIs(void);

/* Switch to a specific ROI (call when streaming is STOPPED) */
int CAM_SwitchROI(int new_roi_index);

/* Start automatic ROI switching (call after camera init) */
void CAM_StartROISwitcher(int interval_ms);

/* Stop automatic ROI switching */
void CAM_StopROISwitcher(void);

/* Set callback for camera restart (called by app.c) */
void CAM_SetCameraRestartCallback(void (*callback)(void));

/* Trigger camera restart if callback is set */
void CAM_TriggerCameraRestart(void);

/* External flag to track ROI changes */
extern uint8_t roi_changed_flag;

#endif
