/*
 * adcSetFreezeCal.c
 *
 *  Created on: Aug 12, 2019
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
#include "adcSetUnFreezeCal.h"



/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

/************************** Variable Definitions *****************************/

/************************** Function Definitions ******************************/


/*****************************************************************************/
/**
*
* cli_adcSetUnFreezeCal_init Add functions from this file to CLI
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void cli_adcSetUnFreezeCal_init(void)
{
	static CMDSTRUCT cliCmds[] = {
		//000000000011111111112222    000000000011111111112222222222333333333
		//012345678901234567890123    012345678901234567890123456789012345678
		{"adcSetUnFreezeCal"  , "- <tile> <adc> UnFreeze ADC cal"         , 2, *adcSetUnFreezeCal},
		{" "                       , " "                                  , 0, *cmdComment   },
	};

	cli_addCmds(cliCmds, sizeof(cliCmds)/sizeof(cliCmds[0]));
}


/*****************************************************************************/
/**
*
* dacSetNyquistZone TBD
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void adcSetUnFreezeCal (u32 *cmdVals)
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

	if(XRFdc_IsADCBlockEnabled(RFdcInstPtr, Tile_Id, Block_Id) == 1) {
	XRFdc_GetCalFreeze(RFdcInstPtr, Tile_Id, Block_Id, &calFreezeSettings);
		xil_printf("   ADC Tile%d ch%d Cal Freeze Status before command: %d\r\n", Tile_Id, Block_Id,calFreezeSettings.CalFrozen);
		calFreezeSettings.DisableFreezePin = 1;
		calFreezeSettings.FreezeCalibration = 0;
		XRFdc_SetCalFreeze(RFdcInstPtr, Tile_Id, Block_Id, &calFreezeSettings);
		XRFdc_GetCalFreeze(RFdcInstPtr, Tile_Id, Block_Id, &calFreezeSettings);
		sleep(1);
		XRFdc_GetCalFreeze(RFdcInstPtr, Tile_Id, Block_Id, &calFreezeSettings);
		xil_printf("   ADC Tile%d ch%d Cal Freeze Status after command: %d\r\n", Tile_Id, Block_Id,calFreezeSettings.CalFrozen);
	} else {
		xil_printf("ADC Tile%d ch%d is not available.\n\r", Tile_Id, Block_Id);
	}

	xil_printf("###############################################\r\n\n");

	return;
}

