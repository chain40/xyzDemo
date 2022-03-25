#include "main.h"
#include "stm32f412rx.h"
#include "cmsis_armcc.h"
#include "spl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Flash & Sram layout
 * ---------------------------------------------------------------------------------------
 * | Bootloader    |  64KB | 0x08000000 ~ 0x0800FFFF   | use Flash sector 0, 1, 2, 3    |
 * | DFU		   | 192KB | 0x08010000 ~ 0x0803FFFF   | use Flash sector 4, 5          |
 * | Applicartion  | 256KB | 0x08040000 ~ 0x0807FFFF   | use Flash sector 6, 7			| 
 * | BCB Magic     |   8 B | 0x2001FFF8 ~ 0x2001FFFF   | use on-chip SRAM				|
 * ---------------------------------------------------------------------------------------
 **/

#define MY_SIGNATURE        0xF1517A66
#define APP_SIGNATURE_BASE  (APP_BASE + 0x20)
#define APP_SIGNATURE_SIZE  0x00000004
#define APP_SIGNATURE_VALUE MY_SIGNATURE

// DFU & APP 
#define DFU_BASE		    0x08010000
#define DFU_MAX_SIZE	    0x00030000

#define APP_BASE		    0x08040000
#define APP_MAX_SIZE	    0x00030000

// Boot Ctrl Block (BCB)
#define BCB_MAGIC_BASE	0x2001FFF8
#define BCB_MAGIC_SIZE	0x00000004
#define BCB_LEN_BASE	0x2001FFFC
#define BCB_LEN_SIZE	0x00000002
#define BCB_CRC16_BASE	0x2001FFFE
#define BCB_CRC16_SIZE	0x00000002

#define DFU_MAGIC_VALUE 0x12345678

// Utility
#define COUNTOF(x)  (sizeof(x)/sizeof(x[0]))
#define MIN(X, Y)   (((X) < (Y)) ? (X) : (Y))

// Request ID
#define DFU_START_REQ       0x5555
#define DFU_SIZE_REQ        0x0001 
#define DFU_CHKSUM_REQ      0x0002 
#define DFU_SEG_DATA_REQ    0x0003
#define DFU_SEG_CHKSUM_REQ  0x0004
#define DFU_WAIT_REQ        0x0005
#define DFU_ABORD_REQ       0x00EE
#define DFU_CPLT_REQ        0x00FF


static uint32_t prvDfuSizeReq(void) {    
    uint16_t dfu_size = *(uint16_t *)BCB_LEN_BASE;
    return dfu_size;
}

static uint32_t prvDfuChkSumReq(void) {
    uint16_t dfu_chksum = *(uint16_t *)BCB_CRC16_BASE;
    return dfu_chksum;
}

static uint32_t prvDfuChkSumCal(void *pvDst, uint32_t ulSize) {
    extern unsigned int CRC16(unsigned char * pucFrame, unsigned int usLen);
    uint16_t chk_sum = CRC16(pvDst, ulSize);
    return chk_sum;
}

static bool prvFlashProgram(void *pvDst, void *pvSrc, uint32_t ulSize) {
    uint32_t *pulDest = pvDst;
    uint32_t *pulSrc = pvSrc;
    uint32_t cnt = ulSize / sizeof(*pulSrc);

    /* Program word (32-bit) at a specified address */
    for (int i = 0; i < cnt; i++) {
        HAL_FLASH_Unlock();
        CLEAR_BIT(FLASH->CR, FLASH_CR_PSIZE);
        FLASH->CR |= FLASH_PSIZE_WORD;
        FLASH->CR |= FLASH_CR_PG;
        *pulDest = *pulSrc;
        /* Wait for last operation to be completed */
        HAL_StatusTypeDef status = FLASH_WaitForLastOperation(5000);
        /* If the program operation is completed, disable the PG Bit */
        FLASH->CR &= (~FLASH_CR_PG);
        HAL_FLASH_Lock();
        pulDest++;
        pulSrc++;
        if (status != HAL_OK) {
            return false;
        }
    }

    uint8_t *pucDest = (uint8_t *)pulDest;
    uint8_t *pucSrc = (uint8_t *)pulSrc;
    cnt = ulSize % sizeof(*pulSrc);

    /* Program byte (8-bit) at a specified address */
    for (int i = 0; i < cnt; i++) {
        HAL_FLASH_Unlock();
        CLEAR_BIT(FLASH->CR, FLASH_CR_PSIZE);
        FLASH->CR |= FLASH_PSIZE_BYTE;
        FLASH->CR |= FLASH_CR_PG;
        *pucDest = *pucSrc;
        /* Wait for last operation to be completed */
        HAL_StatusTypeDef status = FLASH_WaitForLastOperation(5000);
        /* If the program operation is completed, disable the PG Bit */
        FLASH->CR &= (~FLASH_CR_PG);
        HAL_FLASH_Lock();
        pucDest++;
        pucSrc++;
        if (status != HAL_OK) {
            return false;
        }
    }
    
    return true;
}

static bool prvAppFlashErase(void) {
    #define FLASH_TIMEOUT 5000
    const uint32_t app_sectors[] = { 6, 7 };
    bool ret = false;
    HAL_FLASH_Unlock();
    for (int i = 0; i < COUNTOF(app_sectors); i++) {
        FLASH_Erase_Sector(app_sectors[i], FLASH_VOLTAGE_RANGE_3);
        HAL_StatusTypeDef status = FLASH_WaitForLastOperation(FLASH_TIMEOUT);
        if (status != HAL_OK) {
            goto __EXIT;
        }
    }
    ret = true;
__EXIT:
    HAL_FLASH_Lock();  
    
    return ret;
}

static void prvDfuMode(void) {
	// get size of dfu image
    uint32_t dfu_size = prvDfuSizeReq();
    if (dfu_size < 448 || dfu_size > DFU_MAX_SIZE) {
        return;
    }        
	// get checksum of dfu image
    uint32_t dfu_chksum = prvDfuChkSumReq();
    if (dfu_chksum == 0) {
        return;
    }
	// check whole dfu image
    if (prvDfuChkSumCal((void *)DFU_BASE, dfu_size) != dfu_chksum) {
        return;
    }
	// erase application
    if (prvAppFlashErase() == false) {
        return;
    }
	// copy dfu image to application
    if (prvFlashProgram((void *)APP_BASE, (void *)DFU_BASE, dfu_size) == false) {
        return;
    }
	// compare dfu image vs application
    for (int i = 0; i < dfu_size; i++) {
        if (((uint8_t *)APP_BASE)[i] != ((uint8_t *)DFU_BASE)[i]) {
			return;
        }
    }  
}

static void prvBootCtrlBlockReset(void) {
    *((uint32_t *)BCB_MAGIC_BASE) = 0;
}

static bool prvIsDfuMagicValid(void) {
    bool valid = *(uint32_t *)BCB_MAGIC_BASE == DFU_MAGIC_VALUE;
    return valid;
}

static bool prvIsAppSignatureValid(void) {
    return *(uint32_t *)APP_SIGNATURE_BASE == APP_SIGNATURE_VALUE;
}

static bool prvEnterDfuMode(void) {
    if (prvIsDfuMagicValid() && prvIsAppSignatureValid()) {
        return true;
    }    
    return false;
}

static void prvApplicationJump(uint32_t ulVectorTabAddr) {
    struct vectors {
        uint32_t __initial_sp;
        void(*Reset_Handler)(void) __attribute__((noreturn));
    };		
    SCB->VTOR = ulVectorTabAddr;
    ((struct vectors *)SCB->VTOR)->Reset_Handler();
}

void vBootloader(void) {
    // Enter Dfu Mode ?
    if (prvEnterDfuMode()) {        
        prvDfuMode();
        prvBootCtrlBlockReset();
        HAL_NVIC_SystemReset();
    }       
    
    // Reset all peripherals, and irqs
    HAL_DeInit();
    for (int i = WWDG_IRQn; i < (FMPI2C1_ER_IRQn + 1); i++) {
        if (HAL_NVIC_GetActive((IRQn_Type)i)) {
            HAL_NVIC_DisableIRQ((IRQn_Type)i);
            if (HAL_NVIC_GetPendingIRQ((IRQn_Type)i)) {
                HAL_NVIC_ClearPendingIRQ((IRQn_Type)i);
            }
        }
    }    
    SysTick->CTRL = 0;    
    
    // If application is exist, enter application.
    prvApplicationJump(APP_BASE);
}
