#include "main.h"
#include "stm32f412rx.h"
#include "cmsis_armcc.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart2;

static void prvApplicationJump(uint32_t ulVectorTabAddr) {
    struct vectors {
        uint32_t __initial_sp;
        void(*Reset_Handler)(void) __attribute__((noreturn));
    };		
    SCB->VTOR = ulVectorTabAddr;
    ((struct vectors *)SCB->VTOR)->Reset_Handler();
}

static void prvSysMemBootJump(void) {
		void (*SysMemBootJump)(void);
    __set_MSP(*(__IO uint32_t *)0x1FFF0000);
    SysMemBootJump = (void (*)(void))(*((uint32_t *)0x1FFF0004));
    SysMemBootJump();
}

static uint32_t prvAppliocationAddr(void) {
	uint32_t addr = 0;
	// TODO... application validation and return application address
	return addr;
}

static bool prvForceEnterDfuMode(void) {
	bool ret = false;
	// TODO... check a trigger status 
	return ret;
}

void vBootloader(void) {
	static char *welcome = "Welcome!!! This is bootloader.\r\n";
	HAL_UART_Transmit(&huart2, (uint8_t *)welcome, strlen(welcome), 0xFFFFFF);
	HAL_GPIO_TogglePin(D2_GPIO_Port,D2_Pin);
	for(int i=0; i<5; i++) {
		HAL_GPIO_TogglePin(D2_GPIO_Port,D2_Pin);
		HAL_GPIO_TogglePin(D3_GPIO_Port,D3_Pin);
		HAL_Delay(500);
	}
	// check application address 
	uint32_t ulAddress;
	if(prvForceEnterDfuMode() == false) {
		ulAddress = prvAppliocationAddr();
	} else {
		ulAddress = 0;
	}
	// reset all peripherals, and irqs
	HAL_DeInit();
	for (int i = WWDG_IRQn; i < (FMPI2C1_ER_IRQn + 1); i++) {
		if(HAL_NVIC_GetActive((IRQn_Type)i)) {
			HAL_NVIC_DisableIRQ((IRQn_Type)i);
			if(HAL_NVIC_GetPendingIRQ((IRQn_Type)i)) {
				HAL_NVIC_ClearPendingIRQ((IRQn_Type)i);
			}
		}
	}
	SysTick->CTRL = 0;
	// if application is exist, enter application.
	if(ulAddress) {
		prvApplicationJump(ulAddress);
	}
	// if application is not exist, enter DFU mode
	prvSysMemBootJump();
}
