/**
  ******************************************************************************
  * @file    roi_test.h
  * @brief   ROI Register Test Functions Header for IMX335 Sensor
  ******************************************************************************
  */

#ifndef ROI_TEST_H
#define ROI_TEST_H

#include "imx335.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Print sensor ID and key configuration registers
  * @param  pObj  IMX335 object pointer
  */
void IMX335_PrintSensorInfo(IMX335_Object_t *pObj);

/**
  * @brief  Print all ROI-related registers
  * @param  pObj  IMX335 object pointer
  */
void IMX335_PrintROIRegisters(IMX335_Object_t *pObj);

/**
  * @brief  Set ROI window on IMX335 sensor
  * @param  pObj    IMX335 object pointer
  * @param  x_start Horizontal start pixel (0-2591)
  * @param  y_start Vertical start line (0-1943)
  * @param  width   Window width (1-2592)
  * @param  height  Window height (1-1944)
  * @retval IMX335_OK or IMX335_ERROR
  */
int32_t IMX335_SetROI(IMX335_Object_t *pObj, uint16_t x_start, uint16_t y_start, 
                      uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif

#endif /* ROI_TEST_H */