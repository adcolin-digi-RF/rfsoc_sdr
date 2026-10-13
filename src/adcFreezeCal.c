/*
 * adcFreezeCal.c
 *
 *  Created on: Aug 10, 2019
 *      Author: jlantz
 */
/***************************** Include Files *********************************/
#include "xil_io.h"
#include "xil_types.h"
#include <stdio.h>
#include "cli.h"
#include "xparameters.h"
#include "main.h"
#include "xrfdc.h"
#include "adcFreezeCal.h"



/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

/************************** Variable Definitions *****************************/

/************************** Function Definitions ******************************/


/*****************************************************************************/
/**
*
* cli_adcFreezeCalStatus_init Add functions from this file to CLI
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void cli_adcFreezeCalStatus_init(void)
{
	static CMDSTRUCT cliCmds[] = {
		//000000000011111111112222    000000000011111111112222222222333333333
		//012345678901234567890123    012345678901234567890123456789012345678
		{"################### ADC Calibration ####################" , " " , 0, *cmdComment   },
		{"adcFreezeCalStatus" , "- Freeze ADC calibration Status"         , 0, *adcFreezeCalStatus},
	};

	cli_addCmds(cliCmds, sizeof(cliCmds)/sizeof(cliCmds[0]));
}

/*****************************************************************************/
/**
*
* adcFreezeCalStatus TBD
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void adcFreezeCalStatus (u32 *cmdVals)
{
	u32 Tile_Id;
	u32 Block_Id;
	XRFdc_IPStatus ipStatus;
	XRFdc* RFdcInstPtr = &RFdcInst;
	XRFdc_Cal_Freeze_Settings calFreezeSettings;

	Tile_Id = cmdVals[0];
	Block_Id = cmdVals[1];

	XRFdc_GetIPStatus(RFdcInstPtr, &ipStatus);
	xil_printf("\n\r###############################################\n\r");
	xil_printf("=== ADC Cal Freeze Report ===\n\r");

	for (Tile_Id=0; Tile_Id<=3; Tile_Id++) {
		for(Block_Id=0; Block_Id<=1; Block_Id++) {
			if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, Tile_Id, Block_Id)) {
				XRFdc_GetCalFreeze(RFdcInstPtr, Tile_Id, Block_Id, &calFreezeSettings);
				xil_printf("   ADC Tile%d ch%d Cal Frozen: %d\r\n", Tile_Id, Block_Id,calFreezeSettings.CalFrozen);
			}
		}
	}

	xil_printf("###############################################\r\n\n");

	return;
}

