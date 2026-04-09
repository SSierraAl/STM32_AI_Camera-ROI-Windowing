################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include Src/subdir.mk
-include STM32Cube_FW_N6/Middlewares/ST/usbx/common/usbx_stm32_device_controllers/subdir.mk
-include STM32Cube_FW_N6/Middlewares/ST/usbx/common/usbx_device_classes/src/subdir.mk
-include STM32Cube_FW_N6/Middlewares/ST/usbx/common/core/src/subdir.mk
-include STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/subdir.mk
-include STM32Cube_FW_N6/Drivers/CMSIS/Device/ST/STM32N6xx/Source/Templates/gcc/subdir.mk
-include STM32Cube_FW_N6/Drivers/CMSIS/Device/ST/STM32N6xx/Source/Templates/subdir.mk
-include STM32Cube_FW_N6/Drivers/BSP/STM32N6xx_Nucleo/subdir.mk
-include STM32Cube_FW_N6/Drivers/BSP/Components/mx25um51245g/subdir.mk
-include Lib/uvcl/Src/usbx/subdir.mk
-include Lib/uvcl/Src/subdir.mk
-include Lib/FreeRTOS/Source/portable/GCC/ARM_CM55_NTZ/non_secure/subdir.mk
-include Lib/FreeRTOS/Source/subdir.mk
-include Lib/Camera_Middleware/sensors/vd6g/subdir.mk
-include Lib/Camera_Middleware/sensors/vd55g1/subdir.mk
-include Lib/Camera_Middleware/sensors/vd1943/subdir.mk
-include Lib/Camera_Middleware/sensors/imx335/subdir.mk
-include Lib/Camera_Middleware/sensors/subdir.mk
-include Lib/Camera_Middleware/ISP_Library/isp/Src/subdir.mk
-include Lib/Camera_Middleware/subdir.mk
-include Gcc/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := x-cube-n6-camera-capture-nucleo
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
x-cube-n6-camera-capture-nucleo.elf \

MAP_FILES += \
x-cube-n6-camera-capture-nucleo.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
x-cube-n6-camera-capture-nucleo.list \

OBJCOPY_BIN += \
x-cube-n6-camera-capture-nucleo.bin \


# All Target
all: main-build

# Main-build Target
main-build: x-cube-n6-camera-capture-nucleo.elf secondary-outputs

# Tool invocations
x-cube-n6-camera-capture-nucleo.elf x-cube-n6-camera-capture-nucleo.map: $(OBJS) $(USER_OBJS) ../../../Gcc/STM32N657xx_nucleo.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-gcc -o "x-cube-n6-camera-capture-nucleo.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m55 -T"../../../Gcc/STM32N657xx_nucleo.ld" --specs=nosys.specs -Wl,-Map="x-cube-n6-camera-capture-nucleo.map" -Wl,--gc-sections -static -Wl,--cmse-implib -Wl,--out-implib=./secure_nsclib.o -u _printf_float -Wl,--no-warn-rwx-segments --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo 'Finished building: $@'
	@echo ' '

x-cube-n6-camera-capture-nucleo.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "x-cube-n6-camera-capture-nucleo.list"
	@echo 'Finished building: $@'
	@echo ' '

x-cube-n6-camera-capture-nucleo.bin: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objcopy  -O binary $(EXECUTABLES) "x-cube-n6-camera-capture-nucleo.bin"
	@echo 'Finished building: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) default.size.stdout x-cube-n6-camera-capture-nucleo.bin x-cube-n6-camera-capture-nucleo.elf x-cube-n6-camera-capture-nucleo.list x-cube-n6-camera-capture-nucleo.map
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST) $(OBJCOPY_BIN)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified

-include ../makefile.targets
