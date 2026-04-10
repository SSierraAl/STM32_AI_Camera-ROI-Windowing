/**
  ******************************************************************************
  * @file    roi_test.c
  * @brief   ROI Register Test Functions for IMX335 Sensor
  ******************************************************************************
  */

#include <stdio.h>
#include <stdint.h>
#include "imx335_reg.h"
#include "imx335.h"

/* External IMX335 object - will be accessed through camera middleware */
/* We need to create our own IO context for direct register access */

/**
  * @brief  Read IMX335 sensor register and print value
  * @param  pObj  IMX335 object pointer
  * @param  reg   Register address to read
  * @param  len   Number of bytes to read (1, 2, or 4)
  * @retval IMX335_OK or IMX335_ERROR
  */
int32_t IMX335_ReadAndPrintReg(IMX335_Object_t *pObj, uint16_t reg, uint16_t len)
{
  uint8_t data[4] = {0};
  int32_t ret;
  uint32_t value = 0;
  
  ret = imx335_read_reg(&pObj->Ctx, reg, data, len);
  
  if (ret == IMX335_OK) {
    /* Combine bytes into value (little-endian) */
    for (int i = 0; i < len; i++) {
      value |= ((uint32_t)data[i] << (i * 8));
    }
    
    if (len == 1) {
      printf("  Reg 0x%04X = 0x%02X (%u)\r\n", reg, data[0], data[0]);
    } else if (len == 2) {
      printf("  Reg 0x%04X = 0x%04X (%u)\r\n", reg, value, value);
    } else {
      printf("  Reg 0x%04X = 0x%08X (%u)\r\n", reg, value, value);
    }
  } else {
    printf("  Reg 0x%04X = READ FAILED (error: %d)\r\n", reg, ret);
  }
  
  return ret;
}

/**
  * @brief  Write IMX335 sensor register
  * @param  pObj  IMX335 object pointer
  * @param  reg   Register address to write
  * @param  value Value to write
  * @param  len   Number of bytes to write (1 or 2)
  * @retval IMX335_OK or IMX335_ERROR
  */
int32_t IMX335_WriteReg(IMX335_Object_t *pObj, uint16_t reg, uint32_t value, uint16_t len)
{
  uint8_t data[4];
  int32_t ret;
  
  /* Split value into bytes (little-endian) */
  for (int i = 0; i < len; i++) {
    data[i] = (value >> (i * 8)) & 0xFF;
  }
  
  ret = imx335_write_reg(&pObj->Ctx, reg, data, len);
  
  if (ret == IMX335_OK) {
    printf("  Wrote Reg 0x%04X = 0x", reg);
    for (int i = len - 1; i >= 0; i--) {
      printf("%02X", data[i]);
    }
    printf(" (%u)\n", value);
  } else {
    printf("  Write Reg 0x%04X FAILED (error: %d)\n", reg, ret);
  }
  
  return ret;
}

/**
  * @brief  Print all ROI-related registers
  * @param  pObj  IMX335 object pointer
  */
void IMX335_PrintROIRegisters(IMX335_Object_t *pObj)
{
  printf("\n=== IMX335 ROI Registers ===\n");
  printf("Address 0x318C (X_ADDR_START LSB): ");
  IMX335_ReadAndPrintReg(pObj, 0x318C, 2);
  printf("Address 0x318E (Y_ADDR_START LSB): ");
  IMX335_ReadAndPrintReg(pObj, 0x318E, 2);
  printf("Address 0x3190 (X_ADDR_END LSB):   ");
  IMX335_ReadAndPrintReg(pObj, 0x3190, 2);
  printf("Address 0x3192 (Y_ADDR_END LSB):   ");
  IMX335_ReadAndPrintReg(pObj, 0x3192, 2);
  printf("Address 0x3074 (AREA3_ST_ADR_1):   ");
  IMX335_ReadAndPrintReg(pObj, 0x3074, 2);
  printf("\n");
}

/**
  * @brief  Print sensor ID and key configuration registers
  * @param  pObj  IMX335 object pointer
  */
void IMX335_PrintSensorInfo(IMX335_Object_t *pObj)
{
  uint32_t id;
  
  printf("\n=== IMX335 Sensor Info ===\n");
  
  /* Read sensor ID */
  if (IMX335_ReadID(pObj, &id) == IMX335_OK) {
    printf("Sensor ID: 0x%02X (expected: 0x00)\n", id);
  } else {
    printf("Sensor ID: READ FAILED\n");
  }
  
  /* Read key registers */
  printf("\nKey Configuration Registers:\n");
  printf("0x3000 (MODE_SELECT):        ");
  IMX335_ReadAndPrintReg(pObj, 0x3000, 1);
  printf("0x3030 (VMAX LSB):           ");
  IMX335_ReadAndPrintReg(pObj, 0x3030, 4);
  printf("0x3058 (SHUTTER LSB):        ");
  IMX335_ReadAndPrintReg(pObj, 0x3058, 3);
  printf("0x30E8 (GAIN):               ");
  IMX335_ReadAndPrintReg(pObj, 0x30E8, 2);
  printf("0x304E (HREVERSE):           ");
  IMX335_ReadAndPrintReg(pObj, 0x304E, 1);
  printf("0x304F (VREVERSE):           ");
  IMX335_ReadAndPrintReg(pObj, 0x304F, 1);
  printf("0x329E (TPG):                ");
  IMX335_ReadAndPrintReg(pObj, 0x329E, 1);
  printf("\n");
}

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
                      uint16_t width, uint16_t height)
{
  int32_t ret;
  uint16_t x_end = x_start + width - 1;
  uint16_t y_end = y_start + height - 1;
  uint8_t hold = 1;
  
  printf("\n=== Setting ROI ===\n");
  printf("Requested: x=%u, y=%u, width=%u, height=%u\n", x_start, y_start, width, height);
  printf("Calculated: x_end=%u, y_end=%u\n", x_end, y_end);
  
  /* Set HOLD bit to prevent intermediate states */
  imx335_write_reg(&pObj->Ctx, IMX335_REG_HOLD, &hold, 1);
  
  /* Write ROI registers (16-bit values, little-endian) */
  ret = imx335_write_reg(&pObj->Ctx, 0x318C, (uint8_t*)&x_start, 2);
  if (ret != IMX335_OK) return ret;
  
  ret = imx335_write_reg(&pObj->Ctx, 0x318E, (uint8_t*)&y_start, 2);
  if (ret != IMX335_OK) return ret;
  
  ret = imx335_write_reg(&pObj->Ctx, 0x3190, (uint8_t*)&x_end, 2);
  if (ret != IMX335_OK) return ret;
  
  ret = imx335_write_reg(&pObj->Ctx, 0x3192, (uint8_t*)&y_end, 2);
  if (ret != IMX335_OK) return ret;
  
  /* Clear HOLD bit to apply changes */
  hold = 0;
  ret = imx335_write_reg(&pObj->Ctx, IMX335_REG_HOLD, &hold, 1);
  
  printf("ROI set successfully!\n");
  printf("Expected buffer size: %u x %u x 1.25 bytes = %.2f KB\n", 
         width, height, (width * height * 1.25) / 1024.0);
  
  return ret;
}
