 /**
 ******************************************************************************
 * @file    main.c
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

#include "app.h"
#include "app_config.h"
#include "app_fuseprogramming.h"
#include "main.h"
#include "npu_cache.h"

/* Include ROI test header */
#include "app.h"

#include "stm32n6xx_nucleo.h"
#include "stm32n6xx_nucleo_bus.h"
#include "stm32n6xx_nucleo_xspi.h"

#include <stdio.h>
#include <string.h>
#include "stm32n6xx_hal_rif.h"
#include "FreeRTOS.h"
#include "task.h"

extern int __uncached_bss_start__;
extern int __uncached_bss_end__;

UART_HandleTypeDef huart1;

static StaticTask_t main_thread;
static StackType_t main_thread_stack[configMINIMAL_STACK_SIZE];

static void SystemClock_Config(void);
static void Security_Config();
static void IAC_Config();
static void CONSOLE_Config(void);
static void Setup_Mpu(void);
static int main_freertos(void);
static void main_thread_fct(void *arg);

/* This is defined in port.c */
void vPortSetupTimerInterrupt(void);

/**
  * @brief  Main program
  * @param  None
  * @retval None
  */
int main(void)
{
  /* Power on ICACHE */
  MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_ICACTIVE_Msk;

  /* Set back system and CPU clock source to HSI */
  __HAL_RCC_CPUCLK_CONFIG(RCC_CPUCLKSOURCE_HSI);
  __HAL_RCC_SYSCLK_CONFIG(RCC_SYSCLKSOURCE_HSI);

  HAL_Init();

  Setup_Mpu();

  SCB_EnableICache();

#if defined(USE_DCACHE)
  /* Power on DCACHE */
  MEMSYSCTL->MSCR |= MEMSYSCTL_MSCR_DCACTIVE_Msk;
  SCB_EnableDCache();
#endif

  return main_freertos();
}

static void Setup_Mpu()
{
  MPU_Attributes_InitTypeDef attr;
  MPU_Region_InitTypeDef region;

  attr.Number = MPU_ATTRIBUTES_NUMBER0;
  attr.Attributes = MPU_NOT_CACHEABLE;
  HAL_MPU_ConfigMemoryAttributes(&attr);

  region.Enable = MPU_REGION_ENABLE;
  region.Number = MPU_REGION_NUMBER0;
  region.BaseAddress = (uint32_t)&__uncached_bss_start__;
  region.LimitAddress = (uint32_t)&__uncached_bss_end__ - 1;
  region.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
  region.AccessPermission = MPU_REGION_ALL_RW;
  region.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  region.DisablePrivExec = MPU_PRIV_INSTRUCTION_ACCESS_ENABLE;
  region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  HAL_MPU_ConfigRegion(&region);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  memset(&__uncached_bss_start__, 0, &__uncached_bss_end__ - &__uncached_bss_start__);
}

static void Security_Config()
{
  __HAL_RCC_RIFSC_CLK_ENABLE();
  RIMC_MasterConfig_t RIMC_master = {0};
  RIMC_master.MasterCID = RIF_CID_1;
  RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_NPU, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DMA2D, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC1 , &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_LTDC2 , &RIMC_master);
  HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_OTG1 , &RIMC_master);

  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_NPU , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DMA2D , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_CSI    , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDC   , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL1 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_LTDCL2 , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_JPEG , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
  HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_OTG1HS , RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
}

static void IAC_Config(void)
{
/* Configure IAC to trap illegal access events */
  __HAL_RCC_IAC_CLK_ENABLE();
  __HAL_RCC_IAC_FORCE_RESET();
  __HAL_RCC_IAC_RELEASE_RESET();
}

void IAC_IRQHandler(void)
{
  while (1)
  {
  }
}

static void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_PeriphCLKInitTypeDef RCC_PeriphCLKInitStruct = {0};

  BSP_SMPS_Init(SMPS_VOLTAGE_OVERDRIVE);
  HAL_Delay(1); /* Assuming Voltage Ramp Speed of 1mV/us --> 100mV increase takes 100us */

  // Oscillator config already done in bootrom
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_NONE;

  /* PLL1 = 64 x 25 / 2 = 800MHz */
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL1.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL1.PLLM = 2;
  RCC_OscInitStruct.PLL1.PLLN = 25;
  RCC_OscInitStruct.PLL1.PLLFractional = 0;
  RCC_OscInitStruct.PLL1.PLLP1 = 1;
  RCC_OscInitStruct.PLL1.PLLP2 = 1;

  /* PLL2 = 64 x 125 / 8 = 1000MHz */
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL2.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL2.PLLM = 8;
  RCC_OscInitStruct.PLL2.PLLFractional = 0;
  RCC_OscInitStruct.PLL2.PLLN = 125;
  RCC_OscInitStruct.PLL2.PLLP1 = 1;
  RCC_OscInitStruct.PLL2.PLLP2 = 1;

  /* PLL3 = 64 x 225 / 16 = 900MHz */
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL3.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL3.PLLM = 16;
  RCC_OscInitStruct.PLL3.PLLN = 225;
  RCC_OscInitStruct.PLL3.PLLFractional = 0;
  RCC_OscInitStruct.PLL3.PLLP1 = 1;
  RCC_OscInitStruct.PLL3.PLLP2 = 1;

  /* PLL4 = 64 x 20 / 32 = 50MHz */
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL4.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL4.PLLM = 32;
  RCC_OscInitStruct.PLL4.PLLFractional = 0;
  RCC_OscInitStruct.PLL4.PLLN = 20;
  RCC_OscInitStruct.PLL4.PLLP1 = 1;
  RCC_OscInitStruct.PLL4.PLLP2 = 1;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    while(1);
  }

  RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK |
                                 RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                                 RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_PCLK4 |
                                 RCC_CLOCKTYPE_PCLK5);

  /* CPU CLock (sysa_ck) = ic1_ck = PLL1 output/ic1_divider = 800 MHz */
  RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_IC1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
  RCC_ClkInitStruct.IC1Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC1Selection.ClockDivider = 1;

  /* AXI Clock (sysb_ck) = ic2_ck = PLL1 output/ic2_divider = 400 MHz */
  RCC_ClkInitStruct.IC2Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC2Selection.ClockDivider = 2;

  /* NPU Clock (sysc_ck) = ic6_ck = PLL2 output/ic6_divider = 1000 MHz */
  RCC_ClkInitStruct.IC6Selection.ClockSelection = RCC_ICCLKSOURCE_PLL2;
  RCC_ClkInitStruct.IC6Selection.ClockDivider = 1;

  /* AXISRAM3/4/5/6 Clock (sysd_ck) = ic11_ck = PLL3 output/ic11_divider = 900 MHz */
  RCC_ClkInitStruct.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL3;
  RCC_ClkInitStruct.IC11Selection.ClockDivider = 1;

  /* HCLK = sysb_ck / HCLK divider = 200 MHz */
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;

  /* PCLKx = HCLK / PCLKx divider = 200 MHz */
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;
  RCC_ClkInitStruct.APB5CLKDivider = RCC_APB5_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
  {
    while(1);
  }

  RCC_PeriphCLKInitStruct.PeriphClockSelection = 0;

  /* XSPI1 kernel clock (ck_ker_xspi1) = HCLK = 200MHz */
  RCC_PeriphCLKInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_XSPI1;
  RCC_PeriphCLKInitStruct.Xspi1ClockSelection = RCC_XSPI1CLKSOURCE_HCLK;

  /* XSPI2 kernel clock (ck_ker_xspi1) = HCLK =  200MHz */
  RCC_PeriphCLKInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_XSPI2;
  RCC_PeriphCLKInitStruct.Xspi2ClockSelection = RCC_XSPI2CLKSOURCE_HCLK;

  if (HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct) != HAL_OK)
  {
    while (1);
  }
}

static void CONSOLE_Config()
{
  GPIO_InitTypeDef gpio_init;

  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

 /* DISCO & NUCLEO USART1 (PE5/PE6) */
  gpio_init.Mode      = GPIO_MODE_AF_PP;
  gpio_init.Pull      = GPIO_PULLUP;
  gpio_init.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio_init.Pin       = GPIO_PIN_5 | GPIO_PIN_6;
  gpio_init.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(GPIOE, &gpio_init);

  huart1.Instance          = USART1;
  huart1.Init.BaudRate     = 115200;
  huart1.Init.Mode         = UART_MODE_TX_RX;
  huart1.Init.Parity       = UART_PARITY_NONE;
  huart1.Init.WordLength   = UART_WORDLENGTH_8B;
  huart1.Init.StopBits     = UART_STOPBITS_1;
  huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_8;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    while (1);
  }
}

static int main_freertos()
{
  TaskHandle_t hdl;

  hdl = xTaskCreateStatic(main_thread_fct, "main", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1,
                          main_thread_stack, &main_thread);
  assert(hdl != NULL);

  vTaskStartScheduler();
  assert(0);

  return -1;
}

static void main_thread_fct(void *arg)
{
  uint32_t preemptPriority;
  uint32_t subPriority;
  IRQn_Type i;
  int ret;

  /* Copy SysTick_IRQn priority set by RTOS and use it as default priorities for IRQs. We are now sure that all irqs
   * have default priority below or equal to configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
   */
  HAL_NVIC_GetPriority(SysTick_IRQn, HAL_NVIC_GetPriorityGrouping(), &preemptPriority, &subPriority);
  for (i = PVD_PVM_IRQn; i <= LTDC_UP_ERR_IRQn; i++)
    HAL_NVIC_SetPriority(i, preemptPriority, subPriority);

  /* Call SystemClock_Config() after vTaskStartScheduler() since it call HAL_Delay() which call vTaskDelay(). Drawback
   * is that we must call vPortSetupTimerInterrupt() since SystemCoreClock value has been modified by SystemClock_Config()
   */
  SystemClock_Config();
  vPortSetupTimerInterrupt();

  CONSOLE_Config();

  Fuse_Programming();

  /*** External RAM and NOR Flash *********************************************/
#ifdef STM32N6570_DK_REV
  BSP_XSPI_RAM_Init(0);
  BSP_XSPI_RAM_EnableMemoryMappedMode(0);
#endif

  BSP_XSPI_NOR_Init_t NOR_Init;
  NOR_Init.InterfaceMode = BSP_XSPI_NOR_OPI_MODE;
  NOR_Init.TransferRate = BSP_XSPI_NOR_DTR_TRANSFER;
  BSP_XSPI_NOR_Init(0, &NOR_Init);
  BSP_XSPI_NOR_EnableMemoryMappedMode(0);

  ret = BSP_LED_Init(LED_GREEN);
  assert(ret == BSP_ERROR_NONE);

  ret = BSP_LED_Init(LED_RED);
  assert(ret == BSP_ERROR_NONE);

  /* Set all required IPs as secure privileged */
  Security_Config();

  IAC_Config();

  /* Keep all IP's enabled during WFE so they can wake up CPU. Fine tune
   * this if you want to save maximum power
   */
  LL_BUS_EnableClockLowPower(~0);
  LL_MEM_EnableClockLowPower(~0);
  LL_AHB1_GRP1_EnableClockLowPower(~0);
  LL_AHB2_GRP1_EnableClockLowPower(~0);
  LL_AHB3_GRP1_EnableClockLowPower(~0);
  LL_AHB4_GRP1_EnableClockLowPower(~0);
  LL_AHB5_GRP1_EnableClockLowPower(~0);
  LL_APB1_GRP1_EnableClockLowPower(~0);
  LL_APB1_GRP2_EnableClockLowPower(~0);
  LL_APB2_GRP1_EnableClockLowPower(~0);
  LL_APB4_GRP1_EnableClockLowPower(~0);
  LL_APB4_GRP2_EnableClockLowPower(~0);
  LL_APB5_GRP1_EnableClockLowPower(~0);
  LL_MISC_EnableClockLowPower(~0);

   /* ============================================
      DYNAMIC ROI CONFIGURATION - FIXED SIZE, VARIABLE POSITION
      CRITICAL: ROI must be configured BEFORE app_run() so it applies
      before the CMW middleware initializes the sensor.
      ============================================ */
   
   printf("\r\n=== Configuring ROI BEFORE Camera Init (640x480) ===\r\n");
   
   /* Configure ROIs - first one is FULL SENSOR, rest are high-res crops */
   App_AddROIConfig(0, 0, 2592, 1944);     /* ROI #0: FULL SENSOR (full view) */
   App_AddROIConfig(976, 732, 640, 480);   /* ROI #1: Center (high-res crop) */
   App_AddROIConfig(0, 0, 640, 480);       /* ROI #2: Top-Left (high-res crop) */
   App_AddROIConfig(1952, 0, 640, 480);    /* ROI #3: Top-Right (high-res crop) */
   App_AddROIConfig(0, 1464, 640, 480);    /* ROI #4: Bottom-Left (high-res crop) */
   App_AddROIConfig(1952, 1464, 640, 480); /* ROI #5: Bottom-Right (high-res crop) */
   App_AddROIConfig(0, 732, 640, 480);     /* ROI #6: Center-Left (high-res crop) */
   App_AddROIConfig(1952, 732, 640, 480);  /* ROI #7: Center-Right (high-res crop) */
   
   /* Activate the FULL SENSOR ROI at startup */
   App_ActivateROI(0);
   
   printf("[ROI] Active ROI at startup: FULL SENSOR (2592x1944)\r\n");
   printf("[ROI] Camera will output full sensor view, downscaled to 640x480\r\n");
   
   App_ListROIs();
   
   printf("\r\n=== ROI CONFIGURED - Starting Camera ===\r\n");
   printf("Camera will output 640x480 from region x=976, y=732\r\n");
   printf("=================================================\r\n");
   
   /* 
    * CRITICAL: Apply ROI to sensor BEFORE any camera initialization.
    * The CMW middleware will detect these registers and use them.
    * This MUST happen before app_run() which starts the camera threads.
    */
   printf("[ROI] Applying ROI to sensor registers NOW (before camera init)...\r\n");
   
   /* Power on camera and initialize I2C */
   App_Camera_PowerOn();
   App_I2C_Init();
   
   /* Now apply the ROI - this happens BEFORE CMW middleware takes control */
   ROI_Config_t *active_roi = App_GetCurrentROI();
   if (active_roi != NULL) {
     /* Manually write registers (bypass App_ApplyROIToSensor which has retry logic issues) */
     uint8_t data[4];
     uint16_t x_end = active_roi->x_start + active_roi->sensor_width - 1;
     uint16_t y_end = active_roi->y_start + active_roi->sensor_height - 1;
     uint32_t vmax = active_roi->sensor_height + 100;
     uint32_t shutter = vmax - 100;
     uint8_t hold, mode;
     
     printf("[ROI] Writing ROI registers directly to sensor...\r\n");
     
     /* Put sensor in STANDBY */
     mode = 0x01;
     BSP_I2C2_WriteReg16(0x34, 0x3000, &mode, 1);
     HAL_Delay(10);
     
     /* Set HOLD */
     hold = 1;
     BSP_I2C2_WriteReg16(0x34, 0x3001, &hold, 1);
     
     /* Write AREA3 registers */
     data[0] = active_roi->x_start & 0xFF;
     data[1] = (active_roi->x_start >> 8) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x3074, data, 2);
     
     data[0] = active_roi->y_start & 0xFF;
     data[1] = (active_roi->y_start >> 8) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x3076, data, 2);
     
     data[0] = x_end & 0xFF;
     data[1] = (x_end >> 8) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x3078, data, 2);
     
     data[0] = y_end & 0xFF;
     data[1] = (y_end >> 8) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x307A, data, 2);
     
     /* Write VMAX */
     data[0] = vmax & 0xFF;
     data[1] = (vmax >> 8) & 0xFF;
     data[2] = (vmax >> 16) & 0xFF;
     data[3] = (vmax >> 24) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x3030, data, 4);
     
     /* Write SHUTTER */
     data[0] = shutter & 0xFF;
     data[1] = (shutter >> 8) & 0xFF;
     data[2] = (shutter >> 16) & 0xFF;
     BSP_I2C2_WriteReg16(0x34, 0x3058, data, 3);
     
     /* Clear HOLD */
     hold = 0;
     BSP_I2C2_WriteReg16(0x34, 0x3001, &hold, 1);
     HAL_Delay(20);
     
     /* Return to streaming mode */
     mode = 0x00;
     BSP_I2C2_WriteReg16(0x34, 0x3000, &mode, 1);
     
     printf("[ROI] Sensor configured! ROI: x=%u, y=%u, size=%dx%d\r\n",
            active_roi->x_start, active_roi->y_start,
            active_roi->sensor_width, active_roi->sensor_height);
   }
   
   /* NOW start the camera system - sensor already configured */
   app_run();
   
   /* 
    * Start automatic ROI switching every 2 seconds.
    * The STM32 will switch ROIs in the background.
    * When you reconnect the Python client, you'll see the new ROI.
    */
   CAM_StartROISwitcher(5000);
   /* ============================================
      ROI REGISTER TEST (Optional - for debugging)
      ============================================ */
   printf("\r\n\n>>> IMX335 Register Test <<<\n");
   printf("Waiting 3 seconds for camera to stabilize...\r\n");
   HAL_Delay(3000);
   App_TestIMX335Registers();
   
   /* NOTE: Dynamic ROI switching is NOT supported during streaming.
    * The camera outputs a fixed 640x480 region configured at startup.
    * To change ROI, you must stop streaming, reconfigure, and restart.
    */
   
   printf("\r\n=== Camera Running - ROI Fixed at Startup ===\r\n");
   printf("Press any key to stop...\r\n");

   vTaskDelete(NULL);
}

HAL_StatusTypeDef MX_DCMIPP_ClockConfig(DCMIPP_HandleTypeDef *hdcmipp)
{
  RCC_PeriphCLKInitTypeDef RCC_PeriphCLKInitStruct = {0};
  HAL_StatusTypeDef ret;

  RCC_PeriphCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_DCMIPP;
  RCC_PeriphCLKInitStruct.DcmippClockSelection = RCC_DCMIPPCLKSOURCE_IC17;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC17].ClockSelection = RCC_ICCLKSOURCE_PLL2;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC17].ClockDivider = 3;
  ret = HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct);
  if (ret)
    return ret;

  RCC_PeriphCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CSI;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC18].ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_PeriphCLKInitStruct.ICSelection[RCC_IC18].ClockDivider = 40;
  ret = HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphCLKInitStruct);
  if (ret)
    return ret;

  return HAL_OK;
}

void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
  assert(hpcd->Instance == USB1_OTG_HS);

  __HAL_RCC_PWR_CLK_ENABLE();

  /* Enable the VDD33USB independent USB 33 voltage monitor */
  HAL_PWREx_EnableVddUSBVMEN();

  /* Wait until VDD33USB is ready */
  while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);

  /* Enable VDDUSB supply */
  HAL_PWREx_EnableVddUSB();

  /* Enable USB1 OTG clock */
  __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();

  /* Set FSEL to 24 Mhz */
  USB1_HS_PHYC->USBPHYC_CR &= ~(0x7U << 0x4U);
  USB1_HS_PHYC->USBPHYC_CR |= (0x2U << 0x4U);

  /* Enable USB1 OTG PHY clock */
  __HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();

  HAL_NVIC_SetPriority(USB1_OTG_HS_IRQn, 6U, 0U);

  /* Enable USB OTG interrupt */
  HAL_NVIC_EnableIRQ(USB1_OTG_HS_IRQn);
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{
  UNUSED(file);
  UNUSED(line);
  __BKPT(0);
  while (1)
  {
  }
}
#endif

/* Allow to debug with cache enable */
__attribute__ ((section (".keep_me"))) void app_clean_invalidate_dbg()
{
  SCB_CleanInvalidateDCache();
}
