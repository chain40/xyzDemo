#ifndef __SLP_H__
#define __SLP_H__

#include <stdint.h>
#include <stdbool.h>

bool bSplSend(void *pvPayload, uint16_t usSize) ;
bool bSplRecv(void *pvPayload, uint16_t *pusActualSize, uint16_t usExpectSize);

#endif // !__SLP_H__
