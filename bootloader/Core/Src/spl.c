/*
 *  SPL: Serial Protocal Layer
 *  
 */
#include "main.h"
#include "stm32f412rx.h"
#include "cmsis_armcc.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SPL_PREAMBLE 0xAAAA

extern UART_HandleTypeDef huart2;

/*
 *  SPL format: [preamble][data size][data][check sum]
 *  [preamble]: 2 bytes
 *  [payload size]: 2 bytes
 *  [payload]: n bytes
 *  [check sum]: 2 bytes
 *
 **/ 
static uint16_t prvChkSum(uint8_t *pucData, uint16_t usLen) {
    uint16_t chksum = 0;
    for (int i = 0; i < usLen; i++) {
        chksum += pucData[i];
    }
    return chksum;
} 

bool bSplSend(void *pvPayload, uint16_t usSize) {
    HAL_StatusTypeDef status;

    uint16_t preamble = SPL_PREAMBLE;
    status = HAL_UART_Transmit(&huart2, (uint8_t *)&preamble, sizeof(preamble), 1000);
    if (status != HAL_OK) {
        return false;
    }
    
    status = HAL_UART_Transmit(&huart2, (uint8_t *)&usSize, sizeof(usSize), 1000);
    if (status != HAL_OK) {
        return false;
    }
    
    status =  HAL_UART_Transmit(&huart2, (uint8_t *)pvPayload, usSize, 1000);
    if (status != HAL_OK) {
        return false;
    }
    
    uint16_t chksum = prvChkSum(pvPayload, usSize);
    status = HAL_UART_Transmit(&huart2, (uint8_t *)&chksum, sizeof(chksum), 1000);    
    if (status != HAL_OK) {
        return false;
    }
    
    return true;
}

bool bSplRecv(void *pvPayload, uint16_t *pusActualSize, uint16_t usExpectSize) {
    HAL_StatusTypeDef status;
    
    uint16_t preamble = 0;
    status = HAL_UART_Receive(&huart2, (uint8_t *)&preamble, sizeof(preamble), 1000);
    if (status != HAL_OK) {
        return false;    
    }
    
    if (preamble != SPL_PREAMBLE) {
        return false;
    }
    
    status = HAL_UART_Receive(&huart2, (uint8_t *)pusActualSize, sizeof(*pusActualSize), 1000);
    if (status != HAL_OK) {
        return false;    
    }    
    
    status = HAL_UART_Receive(&huart2, (uint8_t *)pvPayload, *pusActualSize, 1000);
    if (status != HAL_OK) {
        return false;     
    }
    
    uint16_t chksum = 0;
    status = HAL_UART_Receive(&huart2, (uint8_t *)&chksum, sizeof(chksum), 1000); 
    if (status != HAL_OK) {
        return false;       
    }
    
    if (chksum != prvChkSum(pvPayload, *pusActualSize)) {
        return false;    
    }
    
    return true;
}
