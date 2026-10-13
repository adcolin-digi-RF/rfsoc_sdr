/*
 * play_dma.h
 *
 *  Created on: Nov 12, 2019
 *      Author: jakobc
 */

#ifndef SRC_PLAY_DMA_H_
#define SRC_PLAY_DMA_H_

/***************************** Include Files *********************************/
#include "xil_types.h"

/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

void cli_play_dma_init(void);
void dacPlay (u32 *cmdVals);
void dacStop (u32 *cmdVals);
void dacSawtooth (u32 *cmdVals);
void dacSinCos(u32 *cmdVals);
void dacSin(u32 *cmdVals);

/************************** Variable Definitions *****************************/

#endif /* SRC_PLAY_DMA_H_ */
