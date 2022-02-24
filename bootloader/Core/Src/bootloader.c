#include "main.h"
#include "stm32f412rx.h"
#include "cmsis_armcc.h"
#include "spl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Flash & Sram layout
 * ---------------------------------------------------------------------------------------
 * | Bootloader    |  16KB | 0x08000000 ~ 0x08003FFF   | use Flash sector 0				|
 * | Reserved      |  16KB | 0x08004000 ~ 0x08007FFF   | use Flash sector 1				|  
 * | DFU		   | 244KB | 0x08008000 ~ 0x0803FFFF   | use Flash sector 2, 3, 4, 5    |
 * | Applicartion  | 256KB | 0x08040000 ~ 0x0807FFFF   | use Flash sector 6, 7			| 
 * | BCB Magic     | 256 B | 0x2001FFFC ~ 0x2001FFFF   | use on-chip SRAM				|
 * ---------------------------------------------------------------------------------------
 **/

#define DFU_BASE		0x08008000
#define DFU_MAX_SIZE	0x08038000

#define APP_BASE		0x08040000
#define APP_MAX_SIZE	0x00040000

#define APP_MAGIC_BASE  0x08040020
#define APP_MAGIC_SIZE  0x00000004
#define APP_MAGIC_VALUE "ABAB"

// Boot Ctrl Block (BCB)
#define BCB_MAGIC_BASE	0x2001FFFC
#define BCB_MAGIC_SIZE	0x00000004
#define BCB_MAGIC_VALUE "okgo"

#define COUNTOF(x)  (sizeof(x)/sizeof(x[0]))
extern UART_HandleTypeDef huart2;

static void prvApplicationJump(uint32_t ulVectorTabAddr) {
    struct vectors {
        uint32_t __initial_sp;
        void(*Reset_Handler)(void) __attribute__((noreturn));
    };		
    SCB->VTOR = ulVectorTabAddr;
    ((struct vectors *)SCB->VTOR)->Reset_Handler();
}

static void prvBootCtrlBlockReset(void) {
    *((uint32_t *)BCB_MAGIC_BASE) = 0;
}

static bool prvIsBcbMagicInvalid(void) {
    return *(uint32_t *)BCB_MAGIC_BASE != *(uint32_t *)BCB_MAGIC_VALUE;
}

static bool prvIsApplicationInvalid(void) {
    return *(uint32_t *)APP_MAGIC_BASE != *(uint32_t *)APP_MAGIC_VALUE;
}

static bool prvEnterDfuMode(void) {
    return prvIsBcbMagicInvalid() || prvIsApplicationInvalid();
}

#define DFU_START_REQ       0x5555
#define DFU_SIZE_REQ        0x0001 
#define DFU_CHKSUM_REQ      0x0002 
#define DFU_SEG_CHKSUM_REQ  0x0003
#define DFU_SEG_DATA_REQ    0x0004

static bool prvDfuStartReq(void) {
    #define DFU_START_RSP 0xCC33
    uint16_t dfu_start_req = DFU_START_REQ;
    uint16_t dfu_start_rsp = 0;
    if (bSplSend(&dfu_start_req, sizeof(dfu_start_req)) == false) {
        return false;
    }
    uint16_t recv_size = 0;
    if (bSplRecv(&dfu_start_rsp, &recv_size, sizeof(dfu_start_rsp)) == false) {
        return false;
    }   
    if (dfu_start_rsp != DFU_START_RSP) {
        return false; 
    }
    return true;
}

static uint32_t prvDfuSizeReq(void) {    
    uint16_t dfu_size_req = DFU_SIZE_REQ;
    if (bSplSend(&dfu_size_req, sizeof(dfu_size_req)) == false) {
        return false;
    }

    uint32_t dfu_size = 0;
    uint16_t recv_size = 0;
    if (bSplRecv(&dfu_size, &recv_size, sizeof(dfu_size)) == false) {
        return false;
    }    
    if (recv_size != sizeof(dfu_size)) {
        return false;
    }
    
    return dfu_size;
}

static uint32_t prvDfuChkSumReq(void) {
    uint32_t dfu_chksum_req = DFU_CHKSUM_REQ;
    uint32_t dfu_chksum = 0;
    HAL_UART_Transmit(&huart2, (uint8_t *)&dfu_chksum_req, sizeof(dfu_chksum_req), 1000);
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, (uint8_t *)&dfu_chksum, sizeof(dfu_chksum), 1000);
    if (status != HAL_OK) {
        return 0;
    }
    return dfu_chksum;
}

static bool prvDfuFlashErase(void) {
    #define FLASH_TIMEOUT 5000
    const uint32_t dfu_sectors[] = { 2, 3, 4, 5 };
    bool ret = false;
    HAL_FLASH_Unlock();
    for (int i = 0; i < COUNTOF(dfu_sectors); i++) {
        FLASH_Erase_Sector(dfu_sectors[i], FLASH_VOLTAGE_RANGE_3);
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

static uint32_t prvDfuSegChkSumReq(uint32_t ulBase, uint32_t ulSize) {
    uint32_t dfu_seg_chksum_req = DFU_SEG_CHKSUM_REQ;
    uint32_t dfu_seg_chksum = 0;
    HAL_UART_Transmit(&huart2, (uint8_t *)&dfu_seg_chksum_req, sizeof(dfu_seg_chksum_req), 1000);
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, (uint8_t *)&dfu_seg_chksum, sizeof(dfu_seg_chksum), 1000);
    if (status != HAL_OK) {
        return 0;
    }
    return dfu_seg_chksum;   
}

static uint32_t prvDfuSegDataReq(void *pvDst, uint32_t ulBase, uint32_t ulSize) {
    uint32_t dfu_seg_dat_req = DFU_SEG_DATA_REQ;
    HAL_UART_Transmit(&huart2, (uint8_t *)&dfu_seg_dat_req, sizeof(dfu_seg_dat_req), 1000);
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, (uint8_t *)pvDst, ulSize, 1000);
    if (status != HAL_OK) {
        return ulSize - huart2.RxXferCount;
    }
    return ulSize;       
}

static uint32_t prvDfuChkSumCal(void *pvDst, uint32_t ulSize) {
    uint32_t chk_sum = 0;
    uint8_t *p = pvDst;
    for (uint32_t i = 0; i < ulSize; i++) {
        chk_sum += p[i];
    }
    return chk_sum;
}

static bool prvDfuSegProgram(void *pvDst, void *pvSrc, uint32_t ulSize) {
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

static void prvDfuMode(void) {
    
    if (prvDfuStartReq() == false) {
        return;
    }
    
    uint32_t dfu_size = prvDfuSizeReq();
    if (dfu_size < 448 || dfu_size > DFU_MAX_SIZE) {
        return;
    }    

    uint32_t dfu_chksum = prvDfuChkSumReq();
    if (dfu_chksum == 0) {
        return;
    } 
    
    if (prvDfuFlashErase() == false) {
        return;
    }
    
    uint32_t offset = 0;
    uint32_t retry = 0;
    do {
        static struct {
            uint32_t ulRecvSize;
            uint32_t ulChkSum;
            uint8_t aucData[1024];
        } xSegmentData;         
        
        xSegmentData.ulRecvSize = prvDfuSegDataReq(xSegmentData.aucData, offset, sizeof(xSegmentData.aucData));
        
        xSegmentData.ulChkSum = prvDfuSegChkSumReq(offset, xSegmentData.ulRecvSize);
        
        if (prvDfuChkSumCal(xSegmentData.aucData, xSegmentData.ulRecvSize) != xSegmentData.ulChkSum) {
            if (retry++ < 3)
                continue;
            else 
                return;
        }
        
        void *pvDst = (void *)(DFU_BASE + offset);
        if (prvDfuSegProgram(pvDst, xSegmentData.aucData, xSegmentData.ulRecvSize) == false) {
            if (retry++ < 3)
                continue;
            else 
                return;
        }
        
        offset += xSegmentData.ulRecvSize;
        retry = 0;
        if (xSegmentData.ulRecvSize < sizeof(xSegmentData.aucData)) {
            break;
        }    
    } while (offset < dfu_size);

    __BKPT(255);
    
    prvBootCtrlBlockReset();
}



void vBootloader(void) {
    // Enter Dfu Mode ?
    if (prvEnterDfuMode()) {
        prvDfuMode();
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
