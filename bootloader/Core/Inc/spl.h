#ifndef __SLP_H__
#define __SLP_H__

#include <stdint.h>
#include <stdbool.h>

bool bSplSend(void *pvSrc, uint16_t usSize);
bool bSplRecv(void *pvDst, uint16_t *pusDstSize, uint16_t usExpectSize);

#endif // !__SLP_H__
