################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
LD_SRCS += \
../src/lscript.ld 

C_SRCS += \
../src/adcFreezeCal.c \
../src/adcSetFreezeCal.c \
../src/adcSetUnFreezeCal.c \
../src/cap_dma.c \
../src/cli.c \
../src/cmd_dac_adc_int_dec.c \
../src/cmd_func_dac.c \
../src/cmd_func_mem.c \
../src/eth_ddr.c \
../src/main.c \
../src/platform_zynqmp.c \
../src/play_dma.c \
../src/rfdc_nyquistzone.c \
../src/rfdc_poweron_status.c \
../src/xrfdc.c \
../src/xrfdc_clk.c \
../src/xrfdc_clock.c \
../src/xrfdc_g.c \
../src/xrfdc_intr.c \
../src/xrfdc_mb.c \
../src/xrfdc_mixer.c \
../src/xrfdc_mts.c \
../src/xrfdc_sinit.c 

OBJS += \
./src/adcFreezeCal.o \
./src/adcSetFreezeCal.o \
./src/adcSetUnFreezeCal.o \
./src/cap_dma.o \
./src/cli.o \
./src/cmd_dac_adc_int_dec.o \
./src/cmd_func_dac.o \
./src/cmd_func_mem.o \
./src/eth_ddr.o \
./src/main.o \
./src/platform_zynqmp.o \
./src/play_dma.o \
./src/rfdc_nyquistzone.o \
./src/rfdc_poweron_status.o \
./src/xrfdc.o \
./src/xrfdc_clk.o \
./src/xrfdc_clock.o \
./src/xrfdc_g.o \
./src/xrfdc_intr.o \
./src/xrfdc_mb.o \
./src/xrfdc_mixer.o \
./src/xrfdc_mts.o \
./src/xrfdc_sinit.o 

C_DEPS += \
./src/adcFreezeCal.d \
./src/adcSetFreezeCal.d \
./src/adcSetUnFreezeCal.d \
./src/cap_dma.d \
./src/cli.d \
./src/cmd_dac_adc_int_dec.d \
./src/cmd_func_dac.d \
./src/cmd_func_mem.d \
./src/eth_ddr.d \
./src/main.d \
./src/platform_zynqmp.d \
./src/play_dma.d \
./src/rfdc_nyquistzone.d \
./src/rfdc_poweron_status.d \
./src/xrfdc.d \
./src/xrfdc_clk.d \
./src/xrfdc_clock.d \
./src/xrfdc_g.d \
./src/xrfdc_intr.d \
./src/xrfdc_mb.d \
./src/xrfdc_mixer.d \
./src/xrfdc_mts.d \
./src/xrfdc_sinit.d 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: ARM v8 gcc compiler'
	aarch64-none-elf-gcc -Wall -O0 -g3 -c -fmessage-length=0 -MT"$@" -mcmodel=large -D __BAREMETAL__ -IC:/Users/col_m/vws/rfsoc_com/ZCU111_PLDDR_DMA/export/ZCU111_PLDDR_DMA/sw/ZCU111_PLDDR_DMA/standalone_psu_cortexa53_1/bspinclude/include -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


