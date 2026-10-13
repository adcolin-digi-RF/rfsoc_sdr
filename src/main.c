
/***************************** Include Files *********************************/
#include <stdio.h>
#include <stdarg.h>
#include "main.h"
#include "xparameters.h"
#include "xil_io.h"
#include "sleep.h"
#include "xstatus.h"
#include "xil_printf.h"
#include "xrfdc.h"
#include "xrfdc_clk.h"
#include <metal/log.h>
#include <metal/sys.h>
#include "cli.h"


// Includes for user added CLI functions used in this file
#include "cmd_func_mem.h"
#include "rfdc_poweron_status.h"
#include "cmd_func_dac.h"
#include "adcFreezeCal.h"
#include "adcSetFreezeCal.h"
#include "adcSetUnFreezeCal.h"
#include "rfdc_nyquistzone.h"
#include "cmd_dac_adc_int_dec.h"
#include "cap_dma.h"
#include "play_dma.h"
#include "eth_ddr.h"



/******************** Constant Definitions **********************************/
#define ENABLE_METAL_PRINTS


/**************************** Type Definitions *******************************/


/***************** Macros (Inline Functions) Definitions *********************/


/************************** Function Prototypes ******************************/

void my_metal_default_log_handler(enum metal_log_level level,
			       const char *format, ...);

/************************** Variable Definitions *****************************/
/*
 * Device instance definitions
 */
XRFdc RFdcInst;      /* RFdc driver instance */


/*****************************************************************************/
/**
*
* Main function
*
* TBD
*
* @param	None
*
* @return
*		- XST_SUCCESS if tests pass
*		- XST_FAILURE if fails.
*
* @note		None.
*
******************************************************************************/
int main(void)
{
	u32  Val;
	u32  Minor;
	u32  Major;
	int Status;
	XRFdc_Config *ConfigPtr;
	cmdExitVal=0;

	// Initialize CLI commands structure
	cli_init();
	cli_cmd_func_mem_init();
	cli_cmd_func_dac_init();
	cli_DacAdcIntDec_init();
	cli_rfdcNyquistzone_init();
	cli_rfdc_poweron_status_init();
	cli_adcFreezeCalStatus_init();
	cli_adcSetFreezeCal_init();
	cli_adcSetUnFreezeCal_init();
	cli_play_dma_init();
	cli_cap_dma_init();


	xil_printf("\n\r###############################################\n\r");
	xil_printf("Hello RFSoC World!\n\r\n");


	// Display IP version
	Val = Xil_In32(RFDC_BASE + 0x00000);
	Major = (Val >> 24) & 0xFF;
	Minor = (Val >> 16) & 0xFF;

	xil_printf("RFDC IP Version: %d.%d\r\n",Major,Minor);
	xil_printf("RFDC IP Major Version: %d\r\n",Major);
	xil_printf("RFDC IP Minor Version: %d\r\n",Minor);


	// Configure zcu111 clks
	xil_printf("\nConfiguring the data converter clocks...\r\n");

	xil_printf("Configuring LMK04208 with following settings\r\n");
	//LMK4208 Display Settings (Index)
 	LMK04208DisplaySettings(0);
	//Set LMK04208 (XIicBus, Index)
	LMK04208ClockConfig(1, 0);

	xil_printf("LMX2594's being configured...\r\n");
	// Display supported LMX2594 frequencies
#if 0
	LMX2594DisplaySupportedFreq();
#endif

    //Set LMX2594 Clks:   DAC CLK  ADC CLK
	LMX2594ClockConfig(1, 6553600, 4096000);


	xil_printf("\nThe design is ready to use.\r\n");
	xil_printf("Open XSCT or the terminal to run commands.\r\n");
	xil_printf("###############################################\n\r");


	xil_printf("\r\n--------------- Entering main() ---------------\n\r");


	/////////////////////////////////////////////////////////////////////////////////
	// Initialize RFdc
	//	.log_level		= METAL_LOG_DEBUG,
	//  .log_level		= METAL_LOG_INFO,

#ifdef ENABLE_METAL_PRINTS
    xil_printf("=== Metal log enabled ===\n\r");

	struct metal_init_params init_param = {	\
			.log_handler	= my_metal_default_log_handler,	\
			.log_level		= METAL_LOG_DEBUG,			\

	};
#else
	struct metal_init_params init_param = METAL_INIT_DEFAULTS;
#endif

	if (metal_init(&init_param)) {
		xil_printf("ERROR: Failed to run metal initialization\n");
		return XRFDC_FAILURE;
	}
	/* Initialize the RFdc driver. */
	ConfigPtr = XRFdc_LookupConfig(RFDC_DEVICE_ID);
	if (ConfigPtr == NULL) {
		xil_printf("Failed to init RFdc driver\r\n");
		return XST_FAILURE;
	}

	/* Initializes the controller */
	Status = XRFdc_CfgInitialize(&RFdcInst, ConfigPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to init RFdc controller\r\n");
		return XST_FAILURE;
	}

	/* Initialize the lwIP/GEM TCP capture server. Ethernet is optional: if
	 * initialization fails, the existing CLI remains usable. */
	Status = eth_ddr_init();
	if (Status != XST_SUCCESS) {
		xil_printf("WARNING: Ethernet initialization failed; CLI will remain available.\r\n");
	}


	/////////////////////////////////////////////////////////////////////////////////
	// Display various configurations/status of RFDC


	//display ready status for ADCs and DACs
	rfdcReady(NULL);

	//display the Power On Status ADCs and DACs
	rfdcPoweronStatus(NULL);

	// Display current setting for DAC 0 in tile 0
	dacCurrent(NULL);


	/////////////////////////////////////////////////////////////////////////////////
	// Main Loop


    xil_printf("--------------------------------\r\n");
    xil_printf("Available commands:\r\n");
    xil_printf("--------------------------------\r\n");
	doHelp();


	// Run command line interpreter
	xil_printf("Enter ? for help on commands\n\r");
	xil_printf("Enter 'exit' to end the application and shutdown the data converters.\n\r");


	while(!cmdExitVal) {
		xil_printf( "> ");
		Status = cmdParseAndRun();
		if (Status == XST_NO_DATA) {
			continue;
		}
		if (Status != XST_SUCCESS) {
			xil_printf("Invalid command or option. Enter ? for help. \n\r");
		}
	}
	rfdcShutdown(NULL);
	xil_printf("--- Application closed, have a good day! --- \r\n");

	//Exited cmd loop
	while(1);

	return XST_SUCCESS;
}


/*****************************************************************************/
/**
*
* My libmetal logger
* Intercepts log prints and adjusts \r\n prints to display the some on a uart
* or through a jtagUart.
*
******************************************************************************/

void my_metal_default_log_handler(enum metal_log_level level,
			       const char *format, ...)
{
	char msg[1024];
	char msgOut[1048];
	char *outPtr;
	int i;

	va_list args;
	static const char *level_strs[] = {
		"metal: emergency: ",
		"metal: alert:     ",
		"metal: critical:  ",
		"metal: error:     ",
		"metal: warning:   ",
		"metal: notice:    ",
		"metal: info:      ",
		"metal: debug:     ",
	};

	va_start(args, format);
	vsnprintf(msg, sizeof(msg), format, args);
	va_end(args);

	//replace single \n with \n\r
	outPtr = msgOut;
	for(i=0; i<1024; i++) {
		// if /n/r or /r/n combo
		if ((msg[i] == '\r' && msg[i+1] == '\n') ||
				(msg[i] == '\n' && msg[i+1] == '\r')) {
			*outPtr++ = msg[i++];
		} else if(msg[i] == '\n') {
			//if first char in string is \n, then remove
			if(i==0) {
				continue;
			} else {
				*outPtr++ = '\r';
			}
		}
		*outPtr++ = msg[i];
		if(msg[i] == 0) {
			break;
		}
	}
	//if line doesn't end with \n\r, then add it
	if( (msg[i-1] != '\n') && (msg[i-1] != '\r') ) {
		*(outPtr-1) = '\r';
		*outPtr++ = '\n';
		*outPtr++ = 0;
	}

	if (level <= METAL_LOG_EMERGENCY || level > METAL_LOG_DEBUG)
		level = METAL_LOG_EMERGENCY;

	xil_printf("%s%s", level_strs[level], msgOut);
}


