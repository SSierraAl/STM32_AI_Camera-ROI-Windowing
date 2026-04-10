/**
 ******************************************************************************
 * @file    app_cam.c
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
#include <assert.h>
#include "cmw_camera.h"
#include "app_cam.h"
#include "app_config.h"
#include "stm32n6xx.h"
#include "utils.h"
#include "FreeRTOS.h"
#include "task.h"

/* Define sensor orientation */
#if CAMERA_SELFY == 1
#define SENSOR_IMX335_FLIP CMW_MIRRORFLIP_MIRROR
#define SENSOR_VD66GY_FLIP CMW_MIRRORFLIP_FLIP
#define SENSOR_VD55G1_FLIP CMW_MIRRORFLIP_FLIP
#define SENSOR_VD1943_FLIP CMW_MIRRORFLIP_MIRROR
#else
#define SENSOR_IMX335_FLIP CMW_MIRRORFLIP_NONE
#define SENSOR_VD66GY_FLIP CMW_MIRRORFLIP_FLIP_MIRROR
#define SENSOR_VD55G1_FLIP CMW_MIRRORFLIP_FLIP_MIRROR
#define SENSOR_VD1943_FLIP CMW_MIRRORFLIP_NONE
#endif

/* Define sensor width x height size. 0x0 means full frame */
#define SENSOR_WIDTH     0
#define SENSOR_HEIGHT    0

/* 
 * ROI CONFIGURATION - Multiple ROI positions for switching
 * Each ROI defines a region on the sensor (2592x1944) that outputs 640x480
 */
#define NUM_ROIS 8

/* ROI Definitions - 8 positions covering the sensor */
/* Full sensor is 2592x1944, we crop to 640x480 for high-res views */
static const ROI_Def_t ROIS[NUM_ROIS] = {
  {0,    0,    2592, 1944}, /* ROI 0: FULL SENSOR (full resolution, downscaled by DCMIPP) */
  {976,  732,  640, 480},  /* ROI 1: Center (high-res crop) */
  {0,    0,    640, 480},  /* ROI 2: Top-Left (high-res crop) */
  {1952, 0,    640, 480},  /* ROI 3: Top-Right (high-res crop) */
  {0,    1464, 640, 480},  /* ROI 4: Bottom-Left (high-res crop) */
  {1952, 1464, 640, 480},  /* ROI 5: Bottom-Right (high-res crop) */
  {0,    732,  640, 480},  /* ROI 6: Center-Left (high-res crop) */
  {1952, 732,  640, 480},  /* ROI 7: Center-Right (high-res crop) */
};

/* Current active ROI index */
static int current_roi_index = 0;

/* Flag to enable/disable automatic ROI switching */
static uint8_t roi_switcher_enabled = 0;

/* Flag to signal camera restart needed after ROI switch */
uint8_t roi_changed_flag = 0;

/* Flag to indicate streaming is active (set by app.c) */
uint8_t streaming_active_flag = 0;

/* Function pointer to restart camera pipeline (set by app.c) */
static void (*camera_restart_callback)(void) = NULL;

/* Set callback for camera restart */
void CAM_SetCameraRestartCallback(void (*callback)(void))
{
  camera_restart_callback = callback;
}

/* Trigger camera restart if callback is set */
void CAM_TriggerCameraRestart(void)
{
  if (camera_restart_callback != NULL) {
    printf("[CAM] Triggering camera restart...\r\n");
    camera_restart_callback();
  }
}

/* Switcher interval in milliseconds */
static uint32_t roi_switch_interval_ms = 2000;

/* FreeRTOS task handle for ROI switcher */
static TaskHandle_t roi_switcher_task_handle = NULL;
static StaticTask_t roi_switcher_task_buffer;
static StackType_t roi_switcher_task_stack[256];

/* Get current ROI index */
int CAM_GetCurrentROIIndex(void)
{
  return current_roi_index;
}

/* Get ROI definition by index */
const ROI_Def_t* CAM_GetROIDefinition(int index)
{
  if (index < 0 || index >= NUM_ROIS) {
    return NULL;
  }
  return &ROIS[index];
}

/* Get number of configured ROIs */
int CAM_GetNumROIs(void)
{
  return NUM_ROIS;
}

/* 
 * Switch to a specific ROI - updates the index for next camera init
 * The new ROI will be applied when streaming starts (next connection)
 */
int CAM_SwitchROI(int new_roi_index)
{
  if (new_roi_index < 0 || new_roi_index >= NUM_ROIS) {
    printf("[CAM] ERROR: Invalid ROI index %d\r\n", new_roi_index);
    return -1;
  }
  
  const ROI_Def_t *roi = &ROIS[new_roi_index];
  
  printf("[CAM] Switching to ROI #%d: x=%u, y=%u, size=%dx%d\r\n",
         new_roi_index, roi->x, roi->y, roi->width, roi->height);
  
  /* Just update the index - new ROI applies on next stream start */
  current_roi_index = new_roi_index;
  
  printf("[CAM] ROI #%d selected. Will apply on next connection.\r\n", new_roi_index);
  return 0;
}

/* ROI Switcher Task - switches ROI every N seconds */
static void roi_switcher_task(void *argument)
{
  int next_roi = 0;
  
  printf("[CAM] ROI Switcher task started (interval: %lu ms)\r\n", roi_switch_interval_ms);
  
  while (roi_switcher_enabled) {
    /* Move to next ROI */
    next_roi++;
    if (next_roi >= NUM_ROIS) {
      next_roi = 0;
    }
    
    CAM_SwitchROI(next_roi);
    
    /* Signal that ROI changed - camera needs to restart */
    roi_changed_flag = 1;
    
    /* Wait for interval */
    vTaskDelay(pdMS_TO_TICKS(roi_switch_interval_ms));
  }
  
  printf("[CAM] ROI Switcher task stopped\r\n");
  vTaskDelete(NULL);
}

/* Start automatic ROI switching */
void CAM_StartROISwitcher(int interval_ms)
{
  if (roi_switcher_enabled) {
    printf("[CAM] ROI Switcher already running\r\n");
    return;
  }
  
  roi_switch_interval_ms = interval_ms;
  roi_switcher_enabled = 1;
  
  /* Create the switcher task */
  roi_switcher_task_handle = xTaskCreateStatic(
    roi_switcher_task,
    "roi_switcher",
    256,
    NULL,
    2,  /* Priority */
    roi_switcher_task_stack,
    &roi_switcher_task_buffer
  );
  
  printf("[CAM] ROI Switcher started: switches every %d ms\r\n", interval_ms);
}

/* Stop automatic ROI switching */
void CAM_StopROISwitcher(void)
{
  roi_switcher_enabled = 0;
  
  if (roi_switcher_task_handle != NULL) {
    vTaskDelete(roi_switcher_task_handle);
    roi_switcher_task_handle = NULL;
  }
  
  printf("[CAM] ROI Switcher stopped\r\n");
}

static const char *sensor_names[] = {
  "CMW_UNKNOWN",
  "CMW_VD66GY",
  "CMW_IMX335",
  "CMW_VD55G1",
  "CMW_VD1943",
};

static CMW_Sensor_Name_t sensor;
static int is_sensor_valid = 0;

static int CAM_getFlipMode(CMW_Sensor_Name_t sensor)
{
  int sensor_mirror_flip = CMW_MIRRORFLIP_NONE;
  int sensor_name_idx = 0;

  switch (sensor) {
  case CMW_VD66GY_Sensor:
    sensor_mirror_flip = SENSOR_VD66GY_FLIP;
    sensor_name_idx = 1;
    break;
  case CMW_IMX335_Sensor:
    sensor_mirror_flip = SENSOR_IMX335_FLIP;
    sensor_name_idx = 2;
    break;
  case CMW_VD55G1_Sensor:
    sensor_mirror_flip = SENSOR_VD55G1_FLIP;
    sensor_name_idx = 3;
    break;
  case CMW_VD1943_Sensor:
    sensor_mirror_flip = SENSOR_VD1943_FLIP;
    sensor_name_idx = 4;
    break;
  default:
    assert(0);
  }
  printf("Detected %s\n", sensor_names[sensor_name_idx]);

  return sensor_mirror_flip;
}

static int CAM_FormatToBpp(int dcmipp_output_format)
{
  int bpp = 0;

  switch (dcmipp_output_format)
  {
  case DCMIPP_PIXEL_PACKER_FORMAT_MONO_Y8_G8_1:
    bpp = 1;
    break;
  case DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1:
  case DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1:
    bpp = 2;
    break;
  case DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1:
    bpp = 3;
    break;
  default:
    assert(0);
  }

  return bpp;
}

/* Configure ROI crop based on current ROI index */
static void CAM_InitCropConfig(CMW_Manual_roi_area_t *roi, int sensor_width, int sensor_height, CAM_conf_t *conf)
{
  /* Get the current ROI definition */
  const ROI_Def_t *current_roi = &ROIS[current_roi_index];
  
  printf("[CAM] Configuring ROI crop (ROI #%d): x=%u, y=%u, sensor=%dx%d\r\n",
         current_roi_index, current_roi->x, current_roi->y, 
         current_roi->width, current_roi->height);
  
  /* 
   * Special handling for full sensor view (2592x1944)
   * For full sensor, we use the entire sensor and let DCMIPP downscale
   * For cropped ROIs, we use the crop window
   */
  if (current_roi->width >= 2000 && current_roi->height >= 1500) {
    /* Full sensor view - no crop, use entire sensor */
    printf("[CAM] FULL SENSOR mode - no crop, entire sensor used\r\n");
    roi->width = sensor_width;   /* Use actual sensor width */
    roi->height = sensor_height; /* Use actual sensor height */
    roi->offset_x = 0;
    roi->offset_y = 0;
  } else {
    /* Cropped ROI - use the specified window */
    roi->width = current_roi->width;
    roi->height = current_roi->height;
    roi->offset_x = current_roi->x;
    roi->offset_y = current_roi->y;
  }
  
  printf("[CAM] ROI crop configured: offset=(%u,%u), size=%dx%d\r\n", 
         roi->offset_x, roi->offset_y, roi->width, roi->height);
}

static void CAM_EnableYuv(uint32_t Pipe)
{
  DCMIPP_ColorConversionConfTypeDef color_conf = {
    .ClampOutputSamples = ENABLE,
    .OutputSamplesType = DCMIPP_CLAMP_YUV,
    .RR = 131, .RG = -119, .RB = -12, .RA = 128,
    .GR =  55, .GG =  183, .GB =  18, .GA =   0,
    .BR = -30, .BG = -101, .BB = 131, .BA = 128,
  };
  int ret;

  /* only pipe1 can do yuv */
  assert(Pipe == DCMIPP_PIPE1);
  ret = HAL_DCMIPP_PIPE_SetYUVConversionConfig(CMW_CAMERA_GetDCMIPPHandle(), Pipe, &color_conf);
  assert(ret == HAL_OK);
  ret = HAL_DCMIPP_PIPE_EnableYUVConversion(CMW_CAMERA_GetDCMIPPHandle(), Pipe);
  assert(ret == HAL_OK);
}

static void DCMIPP_PipeInitCapture(CAM_conf_t *cam_conf, int sensor_width, int sensor_height, CAM_conf_t *conf)
{
  CMW_DCMIPP_Conf_t dcmipp_conf;
  uint32_t hw_pitch;
  int ret;

  assert(conf->capture_width >= conf->capture_height);

  dcmipp_conf.output_width = conf->capture_width;
  dcmipp_conf.output_height = conf->capture_height;
  dcmipp_conf.output_format = cam_conf->dcmipp_output_format;
  dcmipp_conf.output_bpp = CAM_FormatToBpp(cam_conf->dcmipp_output_format);
  dcmipp_conf.mode = CMW_Aspect_ratio_manual_roi;
  dcmipp_conf.enable_swap = cam_conf->is_rgb_swap;
  dcmipp_conf.enable_gamma_conversion = 0;
  CAM_InitCropConfig(&dcmipp_conf.manual_conf, sensor_width, sensor_height, conf);
  ret = CMW_CAMERA_SetPipeConfig(DCMIPP_PIPE1, &dcmipp_conf, &hw_pitch);
  assert(ret == HAL_OK);
  assert(hw_pitch == dcmipp_conf.output_width * dcmipp_conf.output_bpp);

  if (cam_conf->dcmipp_output_format == DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1)
    CAM_EnableYuv(DCMIPP_PIPE1);
}

void CAM_Init(CAM_conf_t *conf)
{
  CMW_CameraInit_t cam_conf;
  int ret;

  if (!is_sensor_valid) {
    is_sensor_valid = 1;
    ret = CMW_CAMERA_GetSensorName(&sensor);
    assert(ret == CMW_ERROR_NONE);
  }

  cam_conf.width = SENSOR_WIDTH;
  cam_conf.height = SENSOR_HEIGHT;
  cam_conf.fps = conf->fps;
  cam_conf.mirror_flip = CAM_getFlipMode(sensor);

  ret = CMW_CAMERA_Init(&cam_conf, NULL);
  assert(ret == CMW_ERROR_NONE);

  /* CMW_CAMERA_Init update width height */
  assert(cam_conf.width);
  assert(cam_conf.height);
  DCMIPP_PipeInitCapture(conf, cam_conf.width, cam_conf.height, conf);
}

void CAM_CapturePipe_Start(uint8_t *capture_pipe_dst, uint32_t cam_mode)
{
  int ret;

  ret = CMW_CAMERA_Start(DCMIPP_PIPE1, capture_pipe_dst, cam_mode);
  assert(ret == CMW_ERROR_NONE);
}

void CAM_IspUpdate(void)
{
  int ret;

  ret = CMW_CAMERA_Run();
  assert(ret == CMW_ERROR_NONE);
}

void CAM_Deinit()
{
  int ret;

  ret = CMW_CAMERA_DeInit();
  assert(ret == CMW_ERROR_NONE);
}

void CMW_CAMERA_PIPE_ErrorCallback(uint32_t pipe)
{
  /* FIXME : Need to tune sensor/ipplug so we can remove this implementation */
}