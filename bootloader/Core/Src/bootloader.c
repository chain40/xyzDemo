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
 * | BCB Magic     |   4 B | 0x2001FFFC ~ 0x2001FFFF   | use on-chip SRAM				|
 * ---------------------------------------------------------------------------------------
 **/

#define MY_SIGNATURE        0x08040123

// DFU & APP 
#define DFU_BASE		    0x08008000
#define DFU_MAX_SIZE	    0x00038000

#define DFU_SIGNATURE_BASE  (DFU_BASE + 0x20)
#define DFU_SIGNATURE_SIZE  0x00000004
#define DFU_SIGNATURE_VALUE MY_SIGNATURE

#define APP_BASE		    0x08040000
#define APP_MAX_SIZE	    0x00038000

#define APP_BYTES_BASE      (APP_BASE + APP_MAX_SIZE)
#define APP_BYTES_SIZE      0x00000004
#define APP_CHKSUM_BASE     (APP_BYTES_BASE + APP_BYTES_SIZE) 
#define APP_CHKSUM_SIZE	    0x00000004

#define APP_SIGNATURE_BASE  (APP_BASE + 0x20)
#define APP_SIGNATURE_SIZE  0x00000004
#define APP_SIGNATURE_VALUE MY_SIGNATURE

// Boot Ctrl Block (BCB)
#define BCB_MAGIC_BASE	0x2001FFFC
#define BCB_MAGIC_SIZE	0x00000004
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
    uint16_t dfu_chksum_req = DFU_CHKSUM_REQ;
    if (bSplSend(&dfu_chksum_req, sizeof(dfu_chksum_req)) == false) {
        return false;
    }

    uint16_t dfu_chksum = 0;
    uint16_t recv_size = 0;
    if (bSplRecv(&dfu_chksum, &recv_size, sizeof(dfu_chksum)) == false) {
        return false;
    }    
    
    if (recv_size != sizeof(dfu_chksum)) {
        return false;
    }
    
    return dfu_chksum;
}

static void prvDfuAbortReq(void) {
    uint16_t dfu_abort_req = DFU_ABORD_REQ;
    bSplSend(&dfu_abort_req, sizeof(dfu_abort_req));
}

static void prvDfuCpltReq(void) {
    uint16_t dfu_cplt_req = DFU_CPLT_REQ;
    bSplSend(&dfu_cplt_req, sizeof(dfu_cplt_req));
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

static uint32_t prvDfuSegDataReq(void *pvDst, uint32_t ulBase, uint32_t ulSize) {
    struct __attribute__((packed)) {
        uint16_t usReqID;
        uint32_t ulOffset;
        uint32_t ulSize;
    } xSegDatReq = {
        .usReqID = DFU_SEG_DATA_REQ,
        .ulOffset = ulBase,
        .ulSize = ulSize
    };    
    
    if (bSplSend(&xSegDatReq, sizeof(xSegDatReq)) == false) {
        return false;
    }
    
    uint16_t recv_size = 0;
    if (bSplRecv(pvDst, &recv_size, ulSize) == false) {
        return false;
    }        
    
    return recv_size;       
}

static uint32_t prvDfuSegChkSumReq(uint32_t ulBase, uint32_t ulSize) {
    struct __attribute__((packed)) {
        uint16_t usReqID;
        uint32_t ulOffset;
        uint32_t ulSize;
    } xSegDatReq = {
        .usReqID = DFU_SEG_CHKSUM_REQ,
        .ulOffset = ulBase,
        .ulSize = ulSize
    };    
    
    if (bSplSend(&xSegDatReq, sizeof(xSegDatReq)) == false) {
        return false;
    }
    
    uint16_t dfu_seg_chksum = 0;
    uint16_t recv_size = 0;
    if (bSplRecv(&dfu_seg_chksum, &recv_size, sizeof(dfu_seg_chksum)) == false) {
        return false;
    }       
    
    return dfu_seg_chksum;   
}

static uint32_t prvDfuChkSumCal(void *pvDst, uint32_t ulSize) {
    uint16_t chk_sum = 0;
    uint8_t *p = pvDst;
    for (uint32_t i = 0; i < ulSize; i++) {
        chk_sum += p[i];
    }
    
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

static bool prvIsDfuSignatureInvalid(void) {
    return *(uint32_t *)DFU_SIGNATURE_BASE != DFU_SIGNATURE_VALUE;
}

static void prvDfuMode(void) {
    uint32_t dfu_size = prvDfuSizeReq();
    if (dfu_size < 448 || dfu_size > DFU_MAX_SIZE) {
        goto __EXIT;
    }        
    uint32_t dfu_chksum = prvDfuChkSumReq();
    if (dfu_chksum == 0) {
        goto __EXIT;
    }     
    if (prvDfuFlashErase() == false) {
        goto __EXIT;
    }    
    uint32_t offset = 0;
    uint32_t retry = 0;
    do {
        static  uint8_t data[1024];        
        uint32_t req_size = MIN(dfu_size - offset, sizeof(data));        
        uint32_t data_size = prvDfuSegDataReq(data, offset, req_size);
        uint32_t chksum = prvDfuSegChkSumReq(offset, data_size);
        if (prvDfuChkSumCal(data, data_size) != chksum) {
            if (retry++ < 3) {
                continue; 
            } else { 
                goto __EXIT;
            }
        }        
        
        void *pvDst = (void *)(DFU_BASE + offset);
        if (prvFlashProgram(pvDst, data, data_size) == false) {
            if (retry++ < 3) {
                continue; 
            } else {
                goto __EXIT;
            }
        }    
        
        offset += data_size;
        retry = 0;
        
    } while (offset < dfu_size);
    
    // check dfu image signature
    if (prvIsDfuSignatureInvalid()) {
        goto __EXIT;
    } 
    
    // check whole dfu image
    if (prvDfuChkSumCal((void *)DFU_BASE, dfu_size) != dfu_chksum) {
        goto __EXIT;
    }
    
    // erase application
    if (prvAppFlashErase() == false) {
        goto __EXIT;
    }    
    
    // copy dfu image to application
    if (prvFlashProgram((void *)APP_BASE, (void *)DFU_BASE, dfu_size) == false) {
        goto __EXIT;
    }
    
    // compare dfu image vs application
    for (int i = 0; i < dfu_size; i++) {
        if (((uint8_t *)APP_BASE)[i] != ((uint8_t *)DFU_BASE)[i]) {
            goto __EXIT;  
        }
    }   
    
    // program app size
    if (prvFlashProgram((void *)APP_BYTES_BASE, (void *)&dfu_size, APP_BYTES_SIZE) == false) {
        goto __EXIT;
    }
    if(*(uint32_t *)APP_BYTES_BASE != dfu_size){
        goto __EXIT;
    }
    
    // program app check sum
    if (prvFlashProgram((void *)APP_CHKSUM_BASE, (void *)&dfu_chksum, APP_CHKSUM_SIZE) == false) {
        goto __EXIT;
    }
    
    if(*(uint32_t *)APP_CHKSUM_BASE != dfu_chksum){
        goto __EXIT;
    }
    
    prvDfuCpltReq();
    
    return;    
    
__EXIT:
    prvDfuAbortReq();
}

static void prvBootCtrlBlockReset(void) {
    *((uint32_t *)BCB_MAGIC_BASE) = 0;
}

static bool prvIsDfuMagicValid(void) {
    bool valid = *(uint32_t *)BCB_MAGIC_BASE == DFU_MAGIC_VALUE;
    prvBootCtrlBlockReset();
    return valid;
}

static bool prvIsAppSignatureInvalid(void) {
    return *(uint32_t *)APP_SIGNATURE_BASE != APP_SIGNATURE_VALUE;
}

static bool prvIsAppChkSumInvalid(void) {
    return prvDfuChkSumCal((void *)APP_BASE, *(uint32_t *)APP_BYTES_BASE) != *(uint32_t *)APP_CHKSUM_BASE;
}

static bool prvEnterDfuMode(void) {
    if (prvIsDfuMagicValid()) {
        return true;
    }    
    
    if (prvIsAppSignatureInvalid()) {
        return true;
    }    
    
    if (prvIsAppChkSumInvalid()) {
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
        for (int i = 0; i < 10; i++) {
            if (prvDfuStartReq()) {
                prvDfuMode();
                break;
            }
        }
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
