 /**
 ******************************************************************************
 * @file    app.c
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

#include "app.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "app_cam.h"
#include "app_config.h"
#include "app_jpg.h"
#include "cmw_camera.h"
#include "stm32n6xx_hal.h"
#include "ulist.h"
#ifdef STM32N6570_DK_REV
#include "stm32n6570_discovery.h"
#else
#include "stm32n6xx_nucleo.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "utils.h"
#include "uvcl.h"
#include "app_cam.h"
/* ROI test support */

/* Disable jpeg support for nucleo due to lack of memory */
#ifdef STM32N6570_NUCLEO_REV
#define DISABLE_JPEG 1
#endif

#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "dev"
#endif

#define CAPT_BUFFER_NB                2
#define JPEG_BUFFER_NB                CAPT_BUFFER_NB

#define FREERTOS_PRIORITY(p) ((UBaseType_t)((int)tskIDLE_PRIORITY + configMAX_PRIORITIES / 2 + (p)))

struct list_protected {
  struct ulist head;
  SemaphoreHandle_t lock;
  StaticSemaphore_t lock_buffer;
  SemaphoreHandle_t sem;
  StaticSemaphore_t sem_buffer;
  int level_dbg;
};

struct buffer {
  struct ulist list;
  uint8_t *buffer;
  int size;
  int len;
  int is_jpeg;
};

struct streaming_ctx {
  int counter;
  int is_streaming;
  UVCL_StreamConf_t stream;
};

struct streaming_req {
  struct streaming_ctx ctx;
  SemaphoreHandle_t lock;
  StaticSemaphore_t lock_buffer;
};

/* threads */
 /* uvc thread */
static StaticTask_t uvc_thread;
static StackType_t uvc_thread_stack[2 * configMINIMAL_STACK_SIZE];
  /* isp thread */
static StaticTask_t isp_thread;
static StackType_t isp_thread_stack[2 * configMINIMAL_STACK_SIZE];
static SemaphoreHandle_t isp_sem;
static StaticSemaphore_t isp_sem_buffer;
/* capture thread */
static StaticTask_t capture_thread;
static StackType_t capture_thread_stack[configMINIMAL_STACK_SIZE];
static SemaphoreHandle_t capture_sem;
static StaticSemaphore_t capture_sem_buffer;

/* capture buffers */
static uint8_t capture_buffer[CAPT_BUFFER_NB][MAX_IMG_FRAME_SIZE] ALIGN_32 IN_PSRAM;
static struct buffer capture[CAPT_BUFFER_NB];
#ifndef DISABLE_JPEG
/* jpg */
static uint8_t jpeg_buffer[JPEG_BUFFER_NB][MAX_IMG_FRAME_SIZE] ALIGN_32 IN_PSRAM;
static struct buffer jpeg[JPEG_BUFFER_NB];
#endif

/* uvc */
static struct uvcl_callbacks uvcl_cbs;

/* list heads */
struct list_protected capt_free_buffers;
struct list_protected capt_capturing_buffers;
struct list_protected capt_ready_buffers;
struct list_protected uvc_in_use_buffers;
struct list_protected jpeg_free_buffers;

static struct streaming_req streaming_req;

static int sr_init(struct streaming_req *sr)
{
  sr->ctx.counter = 0;
  sr->ctx.is_streaming = 0;
  sr->lock = xSemaphoreCreateMutexStatic(&sr->lock_buffer);
  assert(sr->lock);

  return 0;
}

static void sr_lock(struct streaming_req *sr)
{
  int ret;

  ret = xSemaphoreTake(sr->lock, portMAX_DELAY);
  assert(ret == pdTRUE);
}

static void sr_unlock(struct streaming_req *sr)
{
  int ret;

  ret = xSemaphoreGive(sr->lock);
  assert(ret == pdTRUE);
}

static void sr_set_streaming_active(struct streaming_req *sr, UVCL_StreamConf_t *stream)
{
  sr_lock(sr);
  sr->ctx.counter++;
  sr->ctx.is_streaming = 1;
  sr->ctx.stream = *stream;
  sr_unlock(sr);
}

static void sr_set_streaming_inactive(struct streaming_req *sr)
{
  sr_lock(sr);
  sr->ctx.counter++;
  sr->ctx.is_streaming = 0;
  sr_unlock(sr);
}

static int sr_is_streaming(struct streaming_req *sr, struct streaming_ctx *ctx)
{
  int res;

  sr_lock(sr);
  res = sr->ctx.is_streaming;
  if (res)
    *ctx = sr->ctx;
  sr_unlock(sr);

  return res;
}

static int sr_is_valid(struct streaming_req *sr, struct streaming_ctx *ctx)
{
  return sr->ctx.counter == ctx->counter;
}

static int lp_init(struct list_protected *lp)
{
  ulist_init_head(&lp->head);
  lp->lock = xSemaphoreCreateMutexStatic(&lp->lock_buffer);
  assert(lp->lock);
  lp->sem = xSemaphoreCreateCountingStatic(CAPT_BUFFER_NB, 0, &lp->sem_buffer);
  assert(lp->sem);
  lp->level_dbg = 0;

  return 0;
}

static void lp_lock(struct list_protected *lp)
{
  int ret;

  ret = xSemaphoreTake(lp->lock, portMAX_DELAY);
  assert(ret == pdTRUE);
}

static void lp_unlock(struct list_protected *lp)
{
  int ret;

  ret = xSemaphoreGive(lp->lock);
  assert(ret == pdTRUE);
}

static int lp_push(struct list_protected *lp, struct buffer *buffer)
{
  int ret;

  assert(!xPortIsInsideInterrupt());

  lp_lock(lp);
  ulist_add_tail(&buffer->list, &lp->head);
  lp->level_dbg++;
  lp_unlock(lp);

  ret = xSemaphoreGive(lp->sem);
  assert(ret == pdTRUE);

  return 0;
}

static struct buffer *lp_pop(struct list_protected *lp, int is_blocking)
{
  struct buffer *res;
  int ret;

  assert(!xPortIsInsideInterrupt());

  ret = xSemaphoreTake(lp->sem, is_blocking ? portMAX_DELAY : 0);
  if (ret == pdFALSE)
    return NULL;

  lp_lock(lp);
  res = ulist_entry(lp->head.next, struct buffer, list);
  lp->level_dbg--;
  assert(res);
  ulist_del(&res->list);
  lp_unlock(lp);

  return res;
}

static struct buffer *lp_remove_buffer(struct list_protected *lp, uint8_t *buffer)
{
  struct buffer *res = NULL;
  struct buffer *current;
  struct buffer *tmp;
  int ret;

  assert(!xPortIsInsideInterrupt());

  ret = xSemaphoreTake(lp->sem, 0);
  assert(ret == pdTRUE);

  lp_lock(lp);
  ulist_for_each_entry_safe(current, tmp, &lp->head, list) {
    if (current->buffer != buffer)
      continue;
    res = current;
    assert(res);
    ulist_del(&res->list);
    break;
  }
  lp->level_dbg--;
  lp_unlock(lp);

  return res;
}

static int uvcl_payload_to_dcmipp_type(int payload_type)
{
  switch (payload_type) {
  case UVCL_PAYLOAD_UNCOMPRESSED_YUY2:
    return DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1;
    break;
  case UVCL_PAYLOAD_FB_JPEG:
  case UVCL_PAYLOAD_JPEG:
    return DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1;
    break;
  case UVCL_PAYLOAD_FB_RGB565:
    return DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
    break;
  case UVCL_PAYLOAD_FB_BGR3:
    return DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1;
    break;
  case UVCL_PAYLOAD_FB_GREY:
    return DCMIPP_PIXEL_PACKER_FORMAT_MONO_Y8_G8_1;
    break;
  case UVCL_PAYLOAD_FB_GREY_D3DFMT_L8:
    return DCMIPP_PIXEL_PACKER_FORMAT_MONO_Y8_G8_1;
    break;
  default:
    assert(0);
  }

  return DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1;
}

static int uvcl_payload_to_bpp(int payload_type)
{
  switch (payload_type) {
  case UVCL_PAYLOAD_UNCOMPRESSED_YUY2:
    return 2;
    break;
  case UVCL_PAYLOAD_FB_JPEG:
  case UVCL_PAYLOAD_JPEG:
    return 1;
    break;
  case UVCL_PAYLOAD_FB_RGB565:
    return 2;
    break;
  case UVCL_PAYLOAD_FB_BGR3:
    return 3;
    break;
  case UVCL_PAYLOAD_FB_GREY:
    return 1;
    break;
  case UVCL_PAYLOAD_FB_GREY_D3DFMT_L8:
    return 1;
    break;
  default:
    assert(0);
  }

  return DCMIPP_PIXEL_PACKER_FORMAT_YUV422_1;
}

static const char *uvcl_payload_to_name(int payload_type)
{
  switch (payload_type) {
  case UVCL_PAYLOAD_UNCOMPRESSED_YUY2:
    return "YUV2";
    break;
  case UVCL_PAYLOAD_FB_JPEG:
    return "JPEG_FB";
    break;
  case UVCL_PAYLOAD_JPEG:
    return "JPEG";
    break;
  case UVCL_PAYLOAD_FB_RGB565:
    return "RGB565";
    break;
  case UVCL_PAYLOAD_FB_BGR3:
    return "BGR3";
    break;
  case UVCL_PAYLOAD_FB_GREY:
    return "GREY";
    break;
  case UVCL_PAYLOAD_FB_GREY_D3DFMT_L8:
    return "GREY_L8";
    break;
  default:
    assert(0);
  }

  return "UNKNOWN";
}

/* ROI Register Test Functions - Direct I2C access to IMX335 sensor */

#include "cmw_io.h"
#include "imx335.h"
#include "imx335_reg.h"

/* Static variable to track I2C initialization */
static int i2c_initialized = 0;
static int camera_powered = 0;

/* Dynamic ROI Management System */
/* Supports multiple ROI configurations with different output sizes */

#define MAX_ROI_CONFIGS 8

/* ROI configuration storage - ROI_Config_t is defined in app.h */
static ROI_Config_t roi_configs[MAX_ROI_CONFIGS];
static int roi_config_count = 0;
static int current_roi_index = -1;

/* Forward declarations for functions used before definition */
int App_IMX335_WriteReg(uint16_t reg, uint8_t *data, uint16_t len);
int App_IMX335_ReadReg(uint16_t reg, uint8_t *data, uint16_t len);

/* Get current ROI configuration */
ROI_Config_t* App_GetCurrentROI(void)
{
  if (current_roi_index >= 0 && current_roi_index < roi_config_count) {
    return &roi_configs[current_roi_index];
  }
  return NULL;
}

/* Add a new ROI configuration */
int App_AddROIConfig(uint16_t x_start, uint16_t y_start, 
                     uint16_t out_width, uint16_t out_height)
{
  if (roi_config_count >= MAX_ROI_CONFIGS) {
    printf("[ROI] ERROR: Maximum ROI configs reached (%d)\r\n", MAX_ROI_CONFIGS);
    return -1;
  }
  
  ROI_Config_t *cfg = &roi_configs[roi_config_count];
  cfg->x_start = x_start;
  cfg->y_start = y_start;
  cfg->width = out_width;
  cfg->height = out_height;
  /* Sensor readout size matches output for now */
  cfg->sensor_width = out_width;
  cfg->sensor_height = out_height;
  cfg->active = 0;
  
  roi_config_count++;
  printf("[ROI] Added config #%d: ROI(%u,%u) -> Output %dx%d\r\n",
         roi_config_count-1, x_start, y_start, out_width, out_height);
  return roi_config_count - 1;
}

/* Activate a ROI configuration by index */
int App_ActivateROI(int index)
{
  if (index < 0 || index >= roi_config_count) {
    printf("[ROI] ERROR: Invalid ROI index %d\r\n", index);
    return -1;
  }
  
  current_roi_index = index;
  ROI_Config_t *cfg = &roi_configs[index];
  cfg->active = 1;
  
  printf("[ROI] Activated ROI #%d: x=%u, y=%u, out=%dx%d\r\n",
         index, cfg->x_start, cfg->y_start, cfg->width, cfg->height);
  return 0;
}

/* Deactivate current ROI (use full sensor) */
void App_DeactivateROI(void)
{
  if (current_roi_index >= 0 && current_roi_index < roi_config_count) {
    roi_configs[current_roi_index].active = 0;
  }
  current_roi_index = -1;
  printf("[ROI] ROI deactivated (full sensor)\r\n");
}

/* 
 * Apply ROI to sensor registers BEFORE CMW middleware initialization.
 * This MUST be called before CAM_Init() to configure the sensor properly.
 * 
 * The CMW middleware will then use this ROI configuration for streaming.
 */
static int App_ApplyROIToSensor(ROI_Config_t *cfg)
{
  uint8_t data[4];
  uint16_t x_end, y_end;
  uint32_t vmax, shutter;
  uint8_t hold, mode;
  int retry_count;
  int max_retries = 3;
  
  if (cfg == NULL) {
    return -1;
  }
  
  printf("[ROI] Configuring sensor for ROI: x=%u, y=%u, w=%u, h=%u\r\n", 
         cfg->x_start, cfg->y_start, cfg->sensor_width, cfg->sensor_height);
  
  x_end = cfg->x_start + cfg->sensor_width - 1;
  y_end = cfg->y_start + cfg->sensor_height - 1;
  
  /* Calculate VMAX for sensor height + blanking (100 lines for stability) */
  vmax = cfg->sensor_height + 100;
  shutter = vmax - 100;
  if (shutter < 9) shutter = 9;
  
  /* Put sensor in STANDBY mode with retry */
  mode = 0x01;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3000, &mode, 1) == 0) break;
    HAL_Delay(5);
  }
  HAL_Delay(10);
  
  /* Set HOLD with retry */
  hold = 1;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3001, &hold, 1) == 0) break;
    HAL_Delay(5);
  }
  
  /* Write AREA3 registers (ROI window on sensor) with retry */
  data[0] = cfg->x_start & 0xFF;
  data[1] = (cfg->x_start >> 8) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3074, data, 2) == 0) break;
    HAL_Delay(5);
  }
  
  data[0] = cfg->y_start & 0xFF;
  data[1] = (cfg->y_start >> 8) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3076, data, 2) == 0) break;
    HAL_Delay(5);
  }
  
  data[0] = x_end & 0xFF;
  data[1] = (x_end >> 8) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3078, data, 2) == 0) break;
    HAL_Delay(5);
  }
  
  data[0] = y_end & 0xFF;
  data[1] = (y_end >> 8) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x307A, data, 2) == 0) break;
    HAL_Delay(5);
  }
  
  /* Write VMAX with retry */
  data[0] = vmax & 0xFF;
  data[1] = (vmax >> 8) & 0xFF;
  data[2] = (vmax >> 16) & 0xFF;
  data[3] = (vmax >> 24) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3030, data, 4) == 0) break;
    HAL_Delay(5);
  }
  
  /* Write SHUTTER with retry */
  data[0] = shutter & 0xFF;
  data[1] = (shutter >> 8) & 0xFF;
  data[2] = (shutter >> 16) & 0xFF;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3058, data, 3) == 0) break;
    HAL_Delay(5);
  }
  
  /* Clear HOLD to apply with retry */
  hold = 0;
  for (retry_count = 0; retry_count < max_retries; retry_count++) {
    if (App_IMX335_WriteReg(0x3001, &hold, 1) == 0) break;
    HAL_Delay(5);
  }
  HAL_Delay(20);
  
  printf("[ROI] Sensor configured for ROI output: %dx%d\r\n",
         cfg->sensor_width, cfg->sensor_height);
  
  return 0;
}

/* 
 * Dynamic ROI capture function
 * This function:
 * 1. Applies ROI to sensor
 * 2. Reconfigures camera pipeline for new resolution
 * 3. Returns the output dimensions to use
 */
int App_CaptureWithROI(uint16_t x_start, uint16_t y_start, 
                       uint16_t width, uint16_t height,
                       uint16_t *out_width, uint16_t *out_height)
{
  /* Add this as a new ROI config if not exists */
  int idx = App_AddROIConfig(x_start, y_start, width, height);
  if (idx < 0) return -1;
  
  /* Activate it */
  if (App_ActivateROI(idx) < 0) return -1;
  
  ROI_Config_t *cfg = &roi_configs[idx];
  
  /* Apply to sensor */
  if (App_ApplyROIToSensor(cfg) < 0) return -1;
  
  /* Return output dimensions */
  *out_width = cfg->width;
  *out_height = cfg->height;
  
  return 0;
}

/* List all configured ROIs */
void App_ListROIs(void)
{
  printf("\r\n=== Configured ROIs ===\r\n");
  for (int i = 0; i < roi_config_count; i++) {
    ROI_Config_t *cfg = &roi_configs[i];
    printf("ROI #%d: [%s] x=%u, y=%u, out=%dx%d, sensor=%dx%d\r\n",
           i, cfg->active ? "ACTIVE" : "----",
           cfg->x_start, cfg->y_start,
           cfg->width, cfg->height,
           cfg->sensor_width, cfg->sensor_height);
  }
  printf("=========================\r\n");
}

/* 
 * Dynamic ROI switching by stopping and restarting the camera.
 * This properly reconfigures the sensor between ROI changes.
 * 
 * Usage: Call this function when streaming is STOPPED (not during streaming)
 */
int App_ChangeROI(int index)
{
  if (index < 0 || index >= roi_config_count) {
    printf("[ROI] ERROR: Invalid ROI index %d\r\n", index);
    return -1;
  }
  
  ROI_Config_t *cfg = &roi_configs[index];
  uint8_t data[4];
  uint8_t hold, mode;
  
  printf("[ROI] Changing to ROI #%d: x=%u, y=%u, size=%dx%d\r\n", 
         index, cfg->x_start, cfg->y_start, cfg->sensor_width, cfg->sensor_height);
  
  /* 
   * CRITICAL: Camera must be STOPPED before changing ROI.
   * The sensor must be in a known state for register writes.
   */
  
  /* Put sensor in STANDBY */
  mode = 0x01;
  App_IMX335_WriteReg(0x3000, &mode, 1);
  HAL_Delay(20);
  
  /* Set HOLD to latch changes */
  hold = 1;
  App_IMX335_WriteReg(0x3001, &hold, 1);
  
  /* Write AREA3 registers */
  data[0] = cfg->x_start & 0xFF;
  data[1] = (cfg->x_start >> 8) & 0xFF;
  App_IMX335_WriteReg(0x3074, data, 2);
  
  data[0] = cfg->y_start & 0xFF;
  data[1] = (cfg->y_start >> 8) & 0xFF;
  App_IMX335_WriteReg(0x3076, data, 2);
  
  uint16_t x_end = cfg->x_start + cfg->sensor_width - 1;
  uint16_t y_end = cfg->y_start + cfg->sensor_height - 1;
  
  data[0] = x_end & 0xFF;
  data[1] = (x_end >> 8) & 0xFF;
  App_IMX335_WriteReg(0x3078, data, 2);
  
  data[0] = y_end & 0xFF;
  data[1] = (y_end >> 8) & 0xFF;
  App_IMX335_WriteReg(0x307A, data, 2);
  
  /* Calculate and write VMAX */
  uint32_t vmax = cfg->sensor_height + 100;
  data[0] = vmax & 0xFF;
  data[1] = (vmax >> 8) & 0xFF;
  data[2] = (vmax >> 16) & 0xFF;
  data[3] = (vmax >> 24) & 0xFF;
  App_IMX335_WriteReg(0x3030, data, 4);
  
  /* Clear HOLD to apply */
  hold = 0;
  App_IMX335_WriteReg(0x3001, &hold, 1);
  HAL_Delay(30);
  
  /* Return to streaming mode */
  mode = 0x00;
  App_IMX335_WriteReg(0x3000, &mode, 1);
  HAL_Delay(50);
  
  /* Update active status */
  if (current_roi_index >= 0 && current_roi_index < roi_config_count) {
    roi_configs[current_roi_index].active = 0;
  }
  current_roi_index = index;
  cfg->active = 1;
  
  printf("[ROI] ROI #%d configured successfully! (VMAX=%lu)\r\n", index, vmax);
  printf("[ROI] Now restart streaming to see the new ROI\r\n");
  
  return 0;
}

/* Global flag for ROI switcher task */
static uint8_t roi_switcher_enabled = 0;
static int roi_switcher_interval_ms = 2000;  /* Default 2 seconds */
static TaskHandle_t roi_switcher_task_handle = NULL;
static StaticTask_t roi_switcher_task_buffer;
static StackType_t roi_switcher_task_stack[256];

/* ROI Switcher Task - switches ROI position periodically */
static void roi_switcher_task(void *argument)
{
  int current_roi = 0;
  
  printf("[ROI] Switcher task started (interval: %d ms)\r\n", roi_switcher_interval_ms);
  
  while (roi_switcher_enabled) {
    /* Switch to next ROI */
    current_roi++;
    if (current_roi >= roi_config_count) {
      current_roi = 0;
    }
    
    App_ChangeROI(current_roi);
    
    /* Wait for interval */
    vTaskDelay(pdMS_TO_TICKS(roi_switcher_interval_ms));
  }
  
  printf("[ROI] Switcher task stopped\r\n");
  vTaskDelete(NULL);
}

/* Start the ROI switcher task */
void App_StartROISwitcher(int interval_ms)
{
  if (roi_switcher_enabled) {
    printf("[ROI] Switcher already running\r\n");
    return;
  }
  
  roi_switcher_interval_ms = interval_ms;
  roi_switcher_enabled = 1;
  
  /* Use static task creation like other tasks in the project */
  roi_switcher_task_handle = xTaskCreateStatic(
    roi_switcher_task,           /* Task function */
    "roi_switcher",              /* Task name */
    256,                         /* Stack size */
    NULL,                        /* Task parameter */
    2,                           /* Task priority */
    roi_switcher_task_stack,     /* Task stack */
    &roi_switcher_task_buffer    /* Task control block */
  );
  
  printf("[ROI] Switcher started: switches every %d ms\r\n", interval_ms);
}

/* Stop the ROI switcher task */
void App_StopROISwitcher(void)
{
  roi_switcher_enabled = 0;
  if (roi_switcher_task_handle != NULL) {
    vTaskDelete(roi_switcher_task_handle);
    roi_switcher_task_handle = NULL;
  }
  printf("[ROI] Switcher stopped\r\n");
}

/**
  * @brief  Power on the camera sensor
  * @retval 0 on success, -1 on failure
  */
int App_Camera_PowerOn(void)
{
  GPIO_InitTypeDef gpio_init_structure = {0};
  
  if (camera_powered) {
    return 0;
  }
  
  printf("[DEBUG] Powering on camera sensor...\r\n");
  
  /* Enable camera power GPIOs */
  EN_CAM_GPIO_ENABLE_VDDIO();
  EN_CAM_GPIO_CLK_ENABLE();
  NRST_CAM_GPIO_ENABLE_VDDIO();
  NRST_CAM_GPIO_CLK_ENABLE();
  
  /* Configure EN_CAM pin (Enable) */
  gpio_init_structure.Pin       = EN_CAM_PIN;
  gpio_init_structure.Pull      = GPIO_NOPULL;
  gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
  gpio_init_structure.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(EN_CAM_PORT, &gpio_init_structure);
  
  /* Configure NRST_CAM pin (Reset) */
  gpio_init_structure.Pin       = NRST_CAM_PIN;
  gpio_init_structure.Pull      = GPIO_NOPULL;
  gpio_init_structure.Mode      = GPIO_MODE_OUTPUT_PP;
  gpio_init_structure.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(NRST_CAM_PORT, &gpio_init_structure);
  
  /* Enable camera power */
  HAL_GPIO_WritePin(EN_CAM_PORT, EN_CAM_PIN, GPIO_PIN_SET);
  
  /* Reset sequence: hold reset low, then release */
  HAL_GPIO_WritePin(NRST_CAM_PORT, NRST_CAM_PIN, GPIO_PIN_RESET);
  HAL_Delay(5);  /* Minimum 2ms required */
  HAL_GPIO_WritePin(NRST_CAM_PORT, NRST_CAM_PIN, GPIO_PIN_SET);
  HAL_Delay(10); /* Wait for sensor to stabilize */
  
  camera_powered = 1;
  printf("[DEBUG] Camera powered on successfully\r\n");
  
  return 0;
}

/**
  * @brief  Initialize I2C2 for sensor access
  * @retval 0 on success, -1 on failure
  */
int App_I2C_Init(void)
{
  int32_t ret;
  
  if (i2c_initialized) {
    return 0;
  }
  
  /* Power on camera first */
  App_Camera_PowerOn();
  
  ret = BSP_I2C2_Init();
  if (ret == BSP_ERROR_NONE) {
    i2c_initialized = 1;
    printf("I2C2 initialized successfully\r\n");
  } else {
    printf("I2C2 initialization failed: %d\r\n", ret);
  }
  
  return (ret == BSP_ERROR_NONE) ? 0 : -1;
}

/**
  * @brief  Check if IMX335 sensor is present on I2C bus
  * @retval 1 if sensor detected, 0 if not
  */
static int App_IMX335_Detect(void)
{
  uint8_t data;
  
  if (App_I2C_Init() != 0) {
    printf("[DEBUG] I2C init failed\r\n");
    return 0;
  }
  
  /* Try to read the sensor ID register */
  if (BSP_I2C2_ReadReg16(0x34, 0x3912, &data, 1) == BSP_ERROR_NONE) {
    printf("[DEBUG] Sensor detected at 0x34, ID register read successful\r\n");
    return 1;
  }
  
  printf("[DEBUG] Sensor NOT detected at 0x34\r\n");
  return 0;
}

/**
  * @brief  Read IMX335 sensor register via BSP I2C functions
  * @param  reg   Register address to read (16-bit)
  * @param  data  Pointer to store read data
  * @param  len   Number of bytes to read
  * @retval 0 on success, -1 on failure
  */
int App_IMX335_ReadReg(uint16_t reg, uint8_t *data, uint16_t len)
{
  int32_t ret;
  
  /* Ensure I2C is initialized */
  if (App_I2C_Init() != 0) {
    return -1;
  }
  
  /* Use 16-bit register address size for IMX335 sensor */
  /* Sensor I2C address is 0x34 (7-bit) */
  ret = BSP_I2C2_ReadReg16(0x34, reg, data, len);
  
  if (ret != BSP_ERROR_NONE) {
    printf("[DEBUG] ReadReg16(0x34, 0x%04X) failed: %d\r\n", reg, ret);
  }
  
  return (ret == BSP_ERROR_NONE) ? 0 : -1;
}

/**
  * @brief  Write IMX335 sensor register via BSP I2C functions
  * @param  reg   Register address to write (16-bit)
  * @param  data  Pointer to data to write
  * @param  len   Number of bytes to write
  * @retval 0 on success, -1 on failure
  */
int App_IMX335_WriteReg(uint16_t reg, uint8_t *data, uint16_t len)
{
  int32_t ret;
  
  /* Ensure I2C is initialized */
  if (App_I2C_Init() != 0) {
    return -1;
  }
  
  /* Use 16-bit register address size for IMX335 sensor */
  /* Sensor I2C address is 0x34 (7-bit) */
  ret = BSP_I2C2_WriteReg16(0x34, reg, data, len);
  
  if (ret != BSP_ERROR_NONE) {
    printf("[DEBUG] WriteReg16(0x34, 0x%04X) failed: %d\r\n", reg, ret);
  }
  
  return (ret == BSP_ERROR_NONE) ? 0 : -1;
}

/**
  * @brief  Read and print a single register value
  */
static void App_PrintReg(uint16_t reg, uint8_t *data, uint16_t len, int success)
{
  uint32_t value = 0;
  
  if (success) {
    for (int i = 0; i < len; i++) {
      value |= ((uint32_t)data[i] << (i * 8));
    }
    
    if (len == 1) {
      printf("  0x%04X = 0x%02X (%u)\r\n", reg, data[0], data[0]);
    } else if (len == 2) {
      printf("  0x%04X = 0x%04X (%u)\r\n", reg, value, value);
    } else {
      printf("  0x%04X = 0x%08X (%u)\r\n", reg, value, value);
    }
  } else {
    printf("  0x%04X = READ FAILED\r\n", reg);
  }
}

static void app_main_pipe_vsync_event()
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  int ret;

  ret = xSemaphoreGiveFromISR(isp_sem, &xHigherPriorityTaskWoken);
  if (ret == pdTRUE)
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
  * @brief  Test function to read and print IMX335 sensor registers
  *         Call this after camera initialization to verify ROI register access
  */
void App_TestIMX335Registers(void)
{
  uint8_t data[4];
  int ret;
  
  printf("\r\n========================================\r\n");
  printf("IMX335 Sensor Register Test\r\n");
  printf("========================================\r\n");
  
  /* First, try to detect the sensor */
  printf("\r\nDetecting IMX335 sensor...\r\n");
  if (!App_IMX335_Detect()) {
    printf("WARNING: Sensor not detected! Check I2C connections and power.\r\n");
    printf("Note: The camera middleware may have exclusive I2C access.\r\n");
  }
  
  /* Read sensor ID (0x3912) */
  printf("\r\nSensor ID Register (0x3912): ");
  ret = App_IMX335_ReadReg(0x3912, data, 1);
  App_PrintReg(0x3912, data, 1, ret == 0);
  
  /* Read key configuration registers */
  printf("\r\nKey Configuration Registers:\r\n");
  
  printf("  0x3000 (MODE_SELECT):      ");
  ret = App_IMX335_ReadReg(0x3000, data, 1);
  App_PrintReg(0x3000, data, 1, ret == 0);
  
  printf("  0x3030-0x3033 (VMAX):      ");
  ret = App_IMX335_ReadReg(0x3030, data, 4);
  App_PrintReg(0x3030, data, 4, ret == 0);
  
  printf("  0x3058-0x305A (SHUTTER):   ");
  ret = App_IMX335_ReadReg(0x3058, data, 3);
  App_PrintReg(0x3058, data, 3, ret == 0);
  
  printf("  0x30E8-0x30E9 (GAIN):      ");
  ret = App_IMX335_ReadReg(0x30E8, data, 2);
  App_PrintReg(0x30E8, data, 2, ret == 0);
  
  printf("  0x304E (HREVERSE):         ");
  ret = App_IMX335_ReadReg(0x304E, data, 1);
  App_PrintReg(0x304E, data, 1, ret == 0);
  
  printf("  0x304F (VREVERSE):         ");
  ret = App_IMX335_ReadReg(0x304F, data, 1);
  App_PrintReg(0x304F, data, 1, ret == 0);
  
  printf("  0x329E (TPG):              ");
  ret = App_IMX335_ReadReg(0x329E, data, 1);
  App_PrintReg(0x329E, data, 1, ret == 0);
  
  /* Read ROI-specific registers */
  printf("\r\nROI Window Registers:\r\n");
  
  printf("  0x318C-0x318D (X_START):   ");
  ret = App_IMX335_ReadReg(0x318C, data, 2);
  App_PrintReg(0x318C, data, 2, ret == 0);
  
  printf("  0x318E-0x318F (Y_START):   ");
  ret = App_IMX335_ReadReg(0x318E, data, 2);
  App_PrintReg(0x318E, data, 2, ret == 0);
  
  printf("  0x3190-0x3191 (X_END):     ");
  ret = App_IMX335_ReadReg(0x3190, data, 2);
  App_PrintReg(0x3190, data, 2, ret == 0);
  
  printf("  0x3192-0x3193 (Y_END):     ");
  ret = App_IMX335_ReadReg(0x3192, data, 2);
  App_PrintReg(0x3192, data, 2, ret == 0);
  
  printf("  0x3074-0x3075 (AREA3):     ");
  ret = App_IMX335_ReadReg(0x3074, data, 2);
  App_PrintReg(0x3074, data, 2, ret == 0);
  
  printf("\r\n========================================\r\n");
}

/**
  * @brief  Set a custom ROI window on the IMX335 sensor
  * @param  x_start  Horizontal start pixel (0-2591)
  * @param  y_start  Vertical start line (0-1943)
  * @param  width    Window width (1-2592)
  * @param  height   Window height (1-1944)
  * @note   This function uses direct I2C access to write AREA3 registers
  *         Note: Registers 0x318C-0x3193 are READ-ONLY output registers
  *         The actual ROI is configured via AREA3 registers (0x3074-0x307B)
  */
void App_SetIMX335ROI(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height)
{
  uint8_t data[2];
  uint16_t x_end = x_start + width - 1;
  uint16_t y_end = y_start + height - 1;
  uint8_t hold;
  uint8_t mode;
  
  printf("\r\n========================================\r\n");
  printf("Setting IMX335 ROI (Direct I2C Access)\r\n");
  printf("========================================\r\n");
  printf("Requested: x=%u, y=%u, width=%u, height=%u\r\n", x_start, y_start, width, height);
  printf("Calculated: x_end=%u, y_end=%u\r\n", x_end, y_end);
  
  /* First, put sensor in STANDBY mode for register writes */
  printf("\r\nPutting sensor in STANDBY mode...\r\n");
  mode = 0x01;  /* STANDBY */
  App_IMX335_WriteReg(0x3000, &mode, 1);
  HAL_Delay(10);
  
  /* Set HOLD bit to prevent intermediate states */
  hold = 1;
  App_IMX335_WriteReg(0x3001, &hold, 1);  /* HOLD register */
  
  /* Write AREA3 window registers (native IMX335 ROI control) */
  /* AREA3_ST_ADR_1 (0x3074-0x3075) - X start address */
  data[0] = x_start & 0xFF;
  data[1] = (x_start >> 8) & 0xFF;
  printf("Writing AREA3_ST_ADR_1 (0x3074) = %u (0x%04X)\r\n", x_start, x_start);
  App_IMX335_WriteReg(0x3074, data, 2);
  
  /* AREA3_ST_ADR_2 (0x3076-0x3077) - Y start address */
  data[0] = y_start & 0xFF;
  data[1] = (y_start >> 8) & 0xFF;
  printf("Writing AREA3_ST_ADR_2 (0x3076) = %u (0x%04X)\r\n", y_start, y_start);
  App_IMX335_WriteReg(0x3076, data, 2);
  
  /* AREA3_END_ADR_1 (0x3078-0x3079) - X end address */
  data[0] = x_end & 0xFF;
  data[1] = (x_end >> 8) & 0xFF;
  printf("Writing AREA3_END_ADR_1 (0x3078) = %u (0x%04X)\r\n", x_end, x_end);
  App_IMX335_WriteReg(0x3078, data, 2);
  
  /* AREA3_END_ADR_2 (0x307A-0x307B) - Y end address */
  data[0] = y_end & 0xFF;
  data[1] = (y_end >> 8) & 0xFF;
  printf("Writing AREA3_END_ADR_2 (0x307A) = %u (0x%04X)\r\n", y_end, y_end);
  App_IMX335_WriteReg(0x307A, data, 2);
  
  /* Clear HOLD bit to apply changes */
  hold = 0;
  App_IMX335_WriteReg(0x3001, &hold, 1);
  
  printf("\r\nROI set via AREA3 registers!\r\n");
  printf("Expected buffer size: %.2f KB\r\n", (width * height * 1.25) / 1024.0);
  printf("\r\nNOTE: Registers 0x318C-0x3193 are READ-ONLY (calculated by sensor)\r\n");
  printf("        The actual ROI is configured via AREA3 registers (0x3074-0x307B)\r\n");
  
  /* Verify by reading back */
  printf("\r\nVerifying AREA3 registers after write:\r\n");
  
  printf("  0x3074-0x3075 (AREA3_X_START):   ");
  App_IMX335_ReadReg(0x3074, data, 2);
  uint32_t val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x3076-0x3077 (AREA3_Y_START):   ");
  App_IMX335_ReadReg(0x3076, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x3078-0x3079 (AREA3_X_END):     ");
  App_IMX335_ReadReg(0x3078, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x307A-0x307B (AREA3_Y_END):     ");
  App_IMX335_ReadReg(0x307A, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("========================================\r\n");
}

/**
  * @brief  Set ROI using AREA3 registers (IMX335 native ROI control)
  * @param  x_start  Horizontal start pixel (0-2591)
  * @param  y_start  Vertical start line (0-1943)
  * @param  width    Window width (1-2592)
  * @param  height   Window height (1-1944)
  * @note   This uses the native AREA3 registers for ROI control
  */
void App_SetIMX335ROIArea3(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height)
{
  uint8_t data[2];
  uint16_t x_end = x_start + width - 1;
  uint16_t y_end = y_start + height - 1;
  uint8_t mode;
  
  printf("\r\n========================================\r\n");
  printf("Setting IMX335 ROI via AREA3 registers\r\n");
  printf("========================================\r\n");
  printf("Requested: x=%u, y=%u, width=%u, height=%u\r\n", x_start, y_start, width, height);
  printf("Calculated: x_end=%u, y_end=%u\r\n", x_end, y_end);
  
  /* Put sensor in STANDBY mode for register writes */
  printf("\r\nPutting sensor in STANDBY mode...\r\n");
  mode = 0x01;  /* STANDBY */
  App_IMX335_WriteReg(0x3000, &mode, 1);
  HAL_Delay(10);
  
  /* Write AREA3 window registers */
  /* AREA3_ST_ADR_1 (0x3074-0x3075) - X start address */
  data[0] = x_start & 0xFF;
  data[1] = (x_start >> 8) & 0xFF;
  printf("Writing AREA3_ST_ADR_1 (0x3074) = %u (0x%04X)\r\n", x_start, x_start);
  App_IMX335_WriteReg(0x3074, data, 2);
  
  /* AREA3_ST_ADR_2 (0x3076-0x3077) - Y start address */
  data[0] = y_start & 0xFF;
  data[1] = (y_start >> 8) & 0xFF;
  printf("Writing AREA3_ST_ADR_2 (0x3076) = %u (0x%04X)\r\n", y_start, y_start);
  App_IMX335_WriteReg(0x3076, data, 2);
  
  /* AREA3_END_ADR_1 (0x3078-0x3079) - X end address */
  data[0] = x_end & 0xFF;
  data[1] = (x_end >> 8) & 0xFF;
  printf("Writing AREA3_END_ADR_1 (0x3078) = %u (0x%04X)\r\n", x_end, x_end);
  App_IMX335_WriteReg(0x3078, data, 2);
  
  /* AREA3_END_ADR_2 (0x307A-0x307B) - Y end address */
  data[0] = y_end & 0xFF;
  data[1] = (y_end >> 8) & 0xFF;
  printf("Writing AREA3_END_ADR_2 (0x307A) = %u (0x%04X)\r\n", y_end, y_end);
  App_IMX335_WriteReg(0x307A, data, 2);
  
  printf("\r\nROI set via AREA3! Expected buffer size: %.2f KB\r\n", (width * height * 1.25) / 1024.0);
  
  /* Verify by reading back */
  printf("\r\nVerifying AREA3 registers after write:\r\n");
  
  printf("  0x3074-0x3075 (AREA3_X_START):   ");
  App_IMX335_ReadReg(0x3074, data, 2);
  uint32_t val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x3076-0x3077 (AREA3_Y_START):   ");
  App_IMX335_ReadReg(0x3076, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x3078-0x3079 (AREA3_X_END):     ");
  App_IMX335_ReadReg(0x3078, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("  0x307A-0x307B (AREA3_Y_END):     ");
  App_IMX335_ReadReg(0x307A, data, 2);
  val = data[0] | (data[1] << 8);
  printf("  = 0x%04X (%u)\r\n", val, val);
  
  printf("========================================\r\n");
}

static void isp_thread_fct(void *arg)
{
  int ret;

  while (1) {
    ret = xSemaphoreTake(isp_sem, portMAX_DELAY);
    assert(ret == pdTRUE);

    CAM_IspUpdate();
  }
}

static void app_main_pipe_frame_event()
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  int ret;

  ret = xSemaphoreGiveFromISR(capture_sem, &xHigherPriorityTaskWoken);
  if (ret == pdTRUE)
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void capture_thread_fct(void *arg)
{
  struct buffer *current_buffer;
  struct buffer *next_buffer;
  int ret;

  while (1) {
    ret = xSemaphoreTake(capture_sem, portMAX_DELAY);
    assert(ret == pdTRUE);

    next_buffer = lp_pop(&capt_free_buffers, 0);
    if (next_buffer) {
      ret = HAL_DCMIPP_PIPE_SetMemoryAddress(CMW_CAMERA_GetDCMIPPHandle(), DCMIPP_PIPE1,
                                             DCMIPP_MEMORY_ADDRESS_0, (uint32_t) next_buffer->buffer);
      assert(ret == HAL_OK);
      current_buffer = lp_pop(&capt_capturing_buffers, 0);
      assert(current_buffer);
      lp_push(&capt_capturing_buffers, next_buffer);
      lp_push(&capt_ready_buffers, current_buffer);
    }
  }
}

static void app_uvc_streaming_active(struct uvcl_callbacks *cbs, UVCL_StreamConf_t stream)
{
  sr_set_streaming_active(&streaming_req, &stream);

  BSP_LED_On(LED_RED);
}

static void app_uvc_streaming_inactive(struct uvcl_callbacks *cbs)
{
  sr_set_streaming_inactive(&streaming_req);

  BSP_LED_Off(LED_RED);
}

static void app_uvc_frame_release(struct uvcl_callbacks *cbs, void *frame)
{
  struct buffer *buffer;

  buffer = lp_remove_buffer(&uvc_in_use_buffers, frame);
  assert(buffer);

  if (buffer->is_jpeg)
    lp_push(&jpeg_free_buffers, buffer);
  else
    lp_push(&capt_free_buffers, buffer);
}

static void UVC_Init()
{
  const UVCL_StreamConf_t streams[] = IMG_STREAMS;
  UVCL_Conf_t uvcl_conf = { 0 };
  int ret;
  int i;

  for (i = 0; i < ARRAY_NB(streams); i++)
    uvcl_conf.streams[i] = streams[i];
  uvcl_conf.streams_nb = ARRAY_NB(streams);
  uvcl_conf.is_immediate_mode = 1;
  uvcl_cbs.streaming_active = app_uvc_streaming_active;
  uvcl_cbs.streaming_inactive = app_uvc_streaming_inactive;
  uvcl_cbs.frame_release = app_uvc_frame_release;
  ret = UVCL_Init(USB1_OTG_HS, &uvcl_conf, &uvcl_cbs);
  assert(ret == 0);
}

static void send_raw_frame(struct buffer *buffer)
{
  int ret;

  ret = lp_push(&uvc_in_use_buffers, buffer);
  assert(ret == 0);
  ret = UVCL_ShowFrame(buffer->buffer, buffer->len);
  if (ret) {
    buffer = lp_remove_buffer(&uvc_in_use_buffers, buffer->buffer);
    assert(buffer);
    lp_push(&capt_free_buffers, buffer);
  }
}

static void send_jpg_frame(struct buffer *buffer)
{
#ifndef DISABLE_JPEG
  struct buffer *jpeg_buffer;
  int ret;

  jpeg_buffer = lp_pop(&jpeg_free_buffers, 0);
  if (!jpeg_buffer) {
    ret = lp_push(&capt_free_buffers, buffer);
    assert(ret == 0);
    return ;
  }

  ret = JPG_Encode(jpeg_buffer->buffer, buffer->buffer, jpeg_buffer->size, buffer->len);
  assert(ret > 0);
  jpeg_buffer->len = ret;

  ret = lp_push(&capt_free_buffers, buffer);
  assert(ret == 0);
  ret = lp_push(&uvc_in_use_buffers, jpeg_buffer);
  assert(ret == 0);

  ret = UVCL_ShowFrame(jpeg_buffer->buffer, jpeg_buffer->len);
  if (ret) {
    jpeg_buffer = lp_remove_buffer(&uvc_in_use_buffers, jpeg_buffer->buffer);
    assert(jpeg_buffer);
    lp_push(&jpeg_free_buffers, jpeg_buffer);
  }
#else
  assert(0);
#endif
}

static void JPEG_Init(UVCL_StreamConf_t *stream)
{
#ifndef DISABLE_JPEG
  JPG_conf_t jpg_conf = { 0 };
  int ret;

  jpg_conf.width = stream->width;
  jpg_conf.height = stream->height;
  jpg_conf.fmt_src = JPG_SRC_YUV422;

  ret = JPG_Init(&jpg_conf);
  assert(ret == 0);
#else
  assert(0);
#endif
}

static void capture_init(UVCL_StreamConf_t *current, int is_jpeg)
{
  CAM_conf_t cam_conf = { 0 };
  struct buffer *buffer;
  int bpp;
  int ret;
  int i;

  if (is_jpeg)
    JPEG_Init(current);

  printf("[ROI] Camera pipeline initializing for: %dx%d@%dfps\r\n",
         current->width, current->height, current->fps);

  /* 
   * Initialize camera - the CMW middleware handles sensor configuration.
   * The ROI is configured through the crop mechanism in CAM_Init().
   * DO NOT apply ROI registers directly - let the middleware handle it.
   */
  cam_conf.capture_width = current->width;
  cam_conf.capture_height = current->height;
  cam_conf.fps = current->fps;
  cam_conf.dcmipp_output_format = uvcl_payload_to_dcmipp_type(current->payload_type);
  cam_conf.is_rgb_swap = 0;
  CAM_Init(&cam_conf);

  bpp = uvcl_payload_to_bpp(current->payload_type);
  for (i = 0; i < CAPT_BUFFER_NB; i++)
    capture[i].len = current->width * current->height * bpp;
  buffer = lp_pop(&capt_free_buffers, 1);
  assert(buffer);
  ret = lp_push(&capt_capturing_buffers, buffer);
  assert(ret == 0);

  /* Start streaming */
  CAM_CapturePipe_Start(buffer->buffer, CMW_MODE_CONTINUOUS);
}

static void capture_deinit(int is_jpeg)
{
  struct buffer *buffer;

  CAM_Deinit();
  if (is_jpeg)
    JPG_Deinit();

  do {
    buffer = lp_pop(&capt_capturing_buffers, 0);
    if (buffer) {
      lp_push(&capt_free_buffers, buffer);
    }
  } while (buffer);

  do {
    buffer = lp_pop(&capt_ready_buffers, 0);
    if (buffer) {
      lp_push(&capt_free_buffers, buffer);
    }
  } while (buffer);
}

static void uvc_thread_fct(void *arg)
{
  struct streaming_ctx current = { 0 };
  struct buffer *buffer;
  int is_jpeg;
  int roi_restart_pending = 0;

  while (1) {
    /* wait for stream request */
    while (!sr_is_streaming(&streaming_req, &current)) {
      /* Check if ROI changed while waiting - if so, restart with new ROI */
      if (roi_changed_flag) {
        roi_changed_flag = 0;
        printf("[UVC] ROI changed detected, will restart with new ROI\r\n");
      }
      HAL_Delay(1);
    }

    /* copy request */
    is_jpeg = current.stream.payload_type == UVCL_PAYLOAD_JPEG || current.stream.payload_type == UVCL_PAYLOAD_FB_JPEG;
    printf("Start streaming %s | %dx%d@%dfps\r\n", uvcl_payload_to_name(current.stream.payload_type), current.stream.width,
           current.stream.height, current.stream.fps);

    capture_init(&current.stream, is_jpeg);
    /* looping until no more streaming */
    while (1) {
      /* Check if ROI changed during streaming */
      if (roi_changed_flag) {
        roi_changed_flag = 0;
        printf("[UVC] ROI changed during streaming, stopping to apply new ROI...\r\n");
        roi_restart_pending = 1;
        break;
      }
    }

    printf("End streaming\r\n");
    capture_deinit(is_jpeg);
  }
}

static void LIST_Init()
{
  int ret;
  int i;

  ret = lp_init(&capt_free_buffers);
  assert(ret == 0);
  ret = lp_init(&capt_capturing_buffers);
  assert(ret == 0);
  ret = lp_init(&capt_ready_buffers);
  assert(ret == 0);
  ret = lp_init(&uvc_in_use_buffers);
  assert(ret == 0);
  ret = lp_init(&jpeg_free_buffers);
  assert(ret == 0);

  for (i = 0; i < CAPT_BUFFER_NB; i++) {
    capture[i].buffer = capture_buffer[i];
    capture[i].size = MAX_IMG_FRAME_SIZE;
    capture[i].is_jpeg = 0;
    ret = lp_push(&capt_free_buffers, &capture[i]);
    assert(ret == 0);
  }

#ifndef DISABLE_JPEG
  for (i = 0; i < JPEG_BUFFER_NB; i++) {
    jpeg[i].buffer = jpeg_buffer[i];
    jpeg[i].size = MAX_IMG_FRAME_SIZE;
    jpeg[i].is_jpeg = 1;
    ret = lp_push(&jpeg_free_buffers, &jpeg[i]);
    assert(ret == 0);
  }
#endif
}

static void app_display_info_header()
{
  printf("========================================\r\n");
  printf("x-cube-n6-camera-capture v2.0.0 (%s)\r\n", APP_VERSION_STRING);
  printf("Build date & time: %s %s\r\n", __DATE__, __TIME__);
#if defined(__GNUC__)
  printf("Compiler: GCC %d.%d.%d\r\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__ICCARM__)
  printf("Compiler: IAR EWARM %d.%d.%d\r\n", __VER__ / 1000000, (__VER__ / 1000) % 1000 ,__VER__ % 1000);
#else
  printf("Compiler: Unknown\r\n");
#endif
  printf("HAL: %lu.%lu.%lu\r\n", __STM32N6xx_HAL_VERSION_MAIN, __STM32N6xx_HAL_VERSION_SUB1, __STM32N6xx_HAL_VERSION_SUB2);
  printf("========================================\r\n");
}

void app_run()
{
  UBaseType_t capture_priority = FREERTOS_PRIORITY(3);
  UBaseType_t isp_priority = FREERTOS_PRIORITY(2);
  UBaseType_t uvc_priority = FREERTOS_PRIORITY(0);
  TaskHandle_t hdl;

  app_display_info_header();
  /* Enable DWT so DWT_CYCCNT works when debugger not attached */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  LIST_Init();
  sr_init(&streaming_req);

  /* UVC ss */
  UVC_Init();

  /* sems + mutex init */
  isp_sem = xSemaphoreCreateCountingStatic(1, 0, &isp_sem_buffer);
  assert(isp_sem);
  capture_sem = xSemaphoreCreateCountingStatic(1, 0, &capture_sem_buffer);
  assert(capture_sem);

  /* threads init */
  hdl = xTaskCreateStatic(uvc_thread_fct, "uvc", configMINIMAL_STACK_SIZE * 2, NULL, uvc_priority, uvc_thread_stack,
                          &uvc_thread);
  assert(hdl != NULL);
  hdl = xTaskCreateStatic(isp_thread_fct, "isp", configMINIMAL_STACK_SIZE * 2, NULL, isp_priority, isp_thread_stack,
                          &isp_thread);
  assert(hdl != NULL);
  hdl = xTaskCreateStatic(capture_thread_fct, "capture", configMINIMAL_STACK_SIZE, NULL, capture_priority, capture_thread_stack,
                          &capture_thread);
  assert(hdl != NULL);

  BSP_LED_On(LED_GREEN);
}

int CMW_CAMERA_PIPE_FrameEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_frame_event();

  return HAL_OK;
}

int CMW_CAMERA_PIPE_VsyncEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_vsync_event();

  return HAL_OK;
}
