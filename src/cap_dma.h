/*
 * cap_dma.h
 *
 *  Created on: Nov 12, 2019
 *      Author: jakobc
 */

#ifndef SRC_CAP_DMA_H_
#define SRC_CAP_DMA_H_

/***************************** Include Files *********************************/
#include "xil_types.h"
#include "xaxidma.h"

/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

void cli_cap_dma_init(void);
void adcCapture (u32 *cmdVals);
int adc_capture_kb(u32 num_kb, int verbose);

/* Latest successfully completed ADC capture. */
UINTPTR adc_capture_base(void);
u32 adc_capture_bytes(void);
u32 adc_capture_capacity(void);

/************************** Variable Definitions *****************************/

#endif /* SRC_CAP_DMA_H_ */
