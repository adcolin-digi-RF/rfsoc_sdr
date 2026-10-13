

/*
 * cli.c
 *
 */


/***************************** Include Files *********************************/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "xparameters.h"
#include "xcoresightpsdcc.h"
#include "xuartps_hw.h"
#include <xil_types.h>
#include "cli.h"
#include "xstatus.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "eth_ddr.h"


/******************** Constant Definitions **********************************/
/* DCC Status Bits */
#define XCORESIGHTPS_DCC_STATUS_RX (1 << 30)

/* Capture buffer defined by capture.tcl */
#define CAPTURE_BASE_ADDR   0x500000000ULL
#define CAPTURE_NUM_SAMPLES 131072U
#define CAPTURE_BYTES       (CAPTURE_NUM_SAMPLES * sizeof(u16))
#define CAPTURE_WORDS_LINE  16U


/**************************** Type Definitions *******************************/


/***************** Macros (Inline Functions) Definitions *********************/
void cmdComment(void) { return; }		//null command
void doBanner(void);
void doClear(void);
void doCaptureRead(void);
void doCaptureBin(void);
static void captureSendByte(u8 data);
static void captureSendBuffer(const u8 *data, u32 length);


/************************** Function Prototypes ******************************/
u32 my_XCoresightPs_DccGetStatus(void);
static int cli_cmd_equal(const char *a, const char *b);


/************************** Variable Definitions *****************************/

char lineOld[LINEMAX];
char line[LINEMAX];		// buffer for command line
char seps[] = " ,\t\n\r";
const char cliPrompt[] = "beamformer> ";
char *cmdPtr;

u32 cmdVals[MAX_CMD_VALS];
int cmdExitVal  = 0;

unsigned char numCmdVals = 0;
int numTablePtrs = 0;        // index for the cliCmdTable

CMDSTRUCT cliCmds_builtIn[] = {
	//CMD_STRING_LEN=56              CMD_HELP_LEN=56
	{"# Built In",                  "",                                      0, *cmdComment},
	{"?",                           "show command menu",                    0, *doHelp},
	{"help",                        "show command menu",                    0, *doHelp},
	{"banner",                      "print CLI banner",                     0, *doBanner},
	{"clear",                       "clear terminal screen",                0, *doClear},
	{"capread",                     "dump 131072 x 16-bit capture samples",  0, *doCaptureRead},
	{"capbin",                      "send capture as raw 16-bit binary",     0, *doCaptureBin},
	{"#",                           "comment line / no operation",          0, *cmdComment},
	{"exit",                        "exit application",                     0, *doExit},
	{"quit",                        "exit application",                     0, *doExit},
	{"",                            "",                                      0, *cmdComment},
};

TBLSTRUCT cliCmdTable[TBL_PTR_MAX];

/*****************************************************************************/
/**
*
* cli_init Initializes memory for command line structure
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/
void cli_init(void)
{
	cli_addCmds(cliCmds_builtIn, sizeof(cliCmds_builtIn)/sizeof(cliCmds_builtIn[0]));
	doBanner();
}

/*****************************************************************************/
/**
*
* cli_addCmds increase and add commands to command line structure
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/
void cli_addCmds(CMDSTRUCT *cmds2add, int numCmds)
{
	if (numTablePtrs < TBL_PTR_MAX) {
		cliCmdTable[numTablePtrs].cmdPointer = cmds2add;
		cliCmdTable[numTablePtrs].cmdCount = numCmds;
		numTablePtrs++;
	}
	else xil_printf("\r\nError: max number of command indexes reached\r\n");
}


static int cli_cmd_equal(const char *a, const char *b)
{
	if(a == NULL || b == NULL) {
		return 0;
	}

	while(*a && *b) {
		if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return 0;
		}
		a++;
		b++;
	}

	return (*a == '\0' && *b == '\0');
}

/*****************************************************************************/
/**
*
* my_sscanf
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/

int my_sscanf(char *tokenPtr, u32 *value) {
    u32 oldval;
    u8 val8;
    int len;

    len = 0;
    oldval = 0;
    while( (val8 = *tokenPtr) ) {
    	len++;
        tokenPtr++;
        val8  -= 0x30;
        if (val8 > 9) val8  = (val8 & 0xdf) - 7;
        if(val8 > 0xf) {
        	len = 0;
        	oldval = 0;
                break;
        }
        oldval = oldval*16 + val8;
    }
    *value = oldval;
    return len;
}

/*****************************************************************************/
/**
 * Wait for one CLI character while continuing to service lwIP.
 *
 * The original RFSoC_COM CLI used blocking inbyte(). With lwIP NO_SYS=1 the
 * application itself must keep calling xemacif_input(), so a blocking inbyte()
 * would stop TCP whenever the user was not typing.
 ******************************************************************************/
static u8 cli_wait_char(void)
{
	while(!myChar_checkRx((u32)STDIN_BASEADDRESS)) {
		eth_ddr_poll();
	}

	return myChar_getRx((u32)STDIN_BASEADDRESS);
}


/*****************************************************************************/
/**
*
* getAndParseInput
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/
char* getAndParseInput( void )
{
    u8 temp, temp1;
    char *cmdValPtr;
    int i = 0;

    while(1) {
    	temp = cli_wait_char();
//    	xil_printf("'inbyte=0x%02x'", temp);
        switch(temp) {
        		case 0:	   goto EXIT_WHILE;   //jtagterminal seems to pass \000 so treat as \n
        		   	   	   break;
        		case 0x0D: goto EXIT_WHILE;   //enter
        				   break;
        		case 0x0A:
        				   break;
                case 0x08:
                case 0x7F: if(i > 0) {
                               xil_printf("\b \b");
                               i--;
                           }
                           break;
                case 0x1b: if((temp1 = cli_wait_char()) == 0x5b) temp1=cli_wait_char();       //Control Key. Weird, if I don't print, I get extra char 0x5b after 0x1b
                           switch(temp1) {
                                case 0x41: strncpy(line, lineOld, LINEMAX);     //up arrow
                                           xil_printf("\r%s%s", cliPrompt, line);
                                           i = strlen(line);
                                           break;
                                case 0x44: if(i > 0) {                         //back arrow
                                               xil_printf("\b \b");
                                               i--;
                                           }
                                           break;
                                default:   break;
                           }
                           break;
                default:
                        xil_printf("%c",temp);		//echo key entered
                        line[i++] = temp;
        }
        if(i >= (LINEMAX-1)) {
        	xil_printf("max line length reached \r\n");
        	goto EXIT_WHILE;
    }
    }
    EXIT_WHILE:
//    xil_printf("\n\r");
    line[i]= '\0';


    strncpy(lineOld, line, LINEMAX);			// make copy of line to history buffer

    // Command scanned into *line so time to scan command
    //
    cmdPtr     = strtok( line, seps );

    if(cmdPtr != NULL) {
        for(char *p = cmdPtr; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
    }

    numCmdVals = 0;
    for(int i=0; i<MAX_CMD_VALS; i++) {
    	cmdValPtr = strtok( NULL, seps );
    	if(!cmdValPtr) break;

    	// Process Hex entry if value starts with "0x"
        if( (cmdValPtr[0]=='0') && (tolower(cmdValPtr[1])=='x') ) {
    		cmdValPtr += 2;
    		if(my_sscanf(cmdValPtr, &cmdVals[i])) numCmdVals++;

    	//Process signed int if starts with '-'
        } else if(cmdValPtr[0]=='-') {
        	cmdValPtr++;
    		cmdVals[i] = atoi(cmdValPtr);
    		cmdVals[i] *= -1;
    		numCmdVals++;

    	// Otherwize process as integer value
    	} else {
    		cmdVals[i] = atoi(cmdValPtr);
    		numCmdVals++;
    	}
    }

    return cmdPtr;
}

/*****************************************************************************/
/**
*
* Parse and run command
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
int cmdParseAndRun( void )
{
	int found;
	void (*func)();

    xil_printf("%s", cliPrompt);
    cmdPtr = getAndParseInput();
    found=0;
	if( cmdPtr != NULL ) {
		for(int i=0; i < numTablePtrs; i++) {
			for (int j = 0; j < cliCmdTable[i].cmdCount; j++) {
				if( cli_cmd_equal(cmdPtr, cliCmdTable[i].cmdPointer[j].cmdString) ) {
					func = cliCmdTable[i].cmdPointer[j].cmdFunc;
					found = 1;
					if(numCmdVals == cliCmdTable[i].cmdPointer[j].valCnt) {
						xil_printf("\r\n");
						(*func)(cmdVals);
						return XST_SUCCESS;
					} else {
						xil_printf("\r\nERROR: '%s' expects %d value(s), received %d\r\n",
								cliCmdTable[i].cmdPointer[j].cmdString,
								cliCmdTable[i].cmdPointer[j].valCnt,
								numCmdVals);
						xil_printf("usage: %s %s\r\n", cliCmdTable[i].cmdPointer[j].cmdString,
								cliCmdTable[i].cmdPointer[j].cmdHelp);
						return XST_FAILURE;
					}
				}
			}
		}
		if(!found) {
			xil_printf("\r\nError: %s command not found\r\n", cmdPtr);
			return XST_FAILURE;
		}
	}
//	xil_printf("cmdPtr is NULL\r\n");
	return XST_NO_DATA;

}

/*****************************************************************************/
/**
*
* Help function for command interface
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void doHelp(void) {
	xil_printf("\r\n");
	xil_printf("============================================================\r\n");
	xil_printf(" Beamformer CLI command menu\r\n");
	xil_printf("============================================================\r\n");
	xil_printf("  %-28s %s\r\n", "Command", "Description / Arguments");
	xil_printf("  %-28s %s\r\n", "-------", "-----------------------");

	for(int i=0; i < numTablePtrs; i++) {
		for (int j = 0; j < cliCmdTable[i].cmdCount; j++) {
			char *cmd = cliCmdTable[i].cmdPointer[j].cmdString;
			char *help = cliCmdTable[i].cmdPointer[j].cmdHelp;

			if(cmd[0] == '\0') {
				xil_printf("\r\n");
			} else if(cmd[0] == '#') {
				xil_printf("\r\n[%s]\r\n", &cmd[2]);
			} else {
				xil_printf("  %-28s %s\r\n", cmd, help);
			}
		}
	}
	xil_printf("============================================================\r\n\r\n");
}

void doBanner(void) {
	xil_printf("\r\n");
	xil_printf("============================================================\r\n");
	xil_printf(" Beamformer CLI\r\n");
	xil_printf(" Type '?' or 'help' to list commands. Values may be decimal or 0xHEX.\r\n");
	xil_printf("============================================================\r\n\r\n");
}

void doClear(void) {
	xil_printf("\033[2J\033[H");
}


/*****************************************************************************/
/**
 * doCaptureRead
 *
 * Dump the capture buffer over the CLI serial output as 16-bit hexadecimal
 * samples. The capture.tcl script reads the same address/range:
 *     base  = 0x500000000
 *     count = 131072 halfwords
 *
 * Output format:
 *     CAPTURE_BEGIN 131072 0x500000000
 *     1234 abcd ...  (16 samples per line)
 *     CAPTURE_END
 ******************************************************************************/
void doCaptureRead(void)
{
    volatile u16 *capture = (volatile u16 *)(UINTPTR)CAPTURE_BASE_ADDR;

    /* Important if PL/DMA wrote the buffer while the CPU cache is enabled. */
    Xil_DCacheInvalidateRange((INTPTR)CAPTURE_BASE_ADDR, (u32)CAPTURE_BYTES);

    xil_printf("CAPTURE_BEGIN %d 0x500000000\r\n", (int)CAPTURE_NUM_SAMPLES);

    for(u32 i = 0; i < CAPTURE_NUM_SAMPLES; i++) {
        u16 sample = capture[i];

        xil_printf("%04x", (unsigned int)sample);

        if(((i + 1U) % CAPTURE_WORDS_LINE) == 0U) {
            xil_printf("\r\n");
        } else {
            xil_printf(" ");
        }
    }

    if((CAPTURE_NUM_SAMPLES % CAPTURE_WORDS_LINE) != 0U) {
        xil_printf("\r\n");
    }

    xil_printf("CAPTURE_END\r\n");
}


/*****************************************************************************/
/**
 * captureSendByte
 *
 * Send one RAW byte to the same output interface used by the CLI.  This does
 * not call outbyte(), because the custom Coresight outbyte() implementation
 * may strip 0x0D characters.  Arbitrary binary data must pass unchanged.
 ******************************************************************************/
static void captureSendByte(u8 data)
{
    if(STDOUT_BASEADDRESS == 0xFE800000U) {
        /* Coresight DCC/JTAG UART */
        XCoresightPs_DccSendByte(STDOUT_BASEADDRESS, data);
    } else {
        /* Zynq UltraScale+ PS UART */
        XUartPs_SendByte((UINTPTR)STDOUT_BASEADDRESS, data);
    }
}


/*****************************************************************************/
/**
 * captureSendBuffer
 *
 * Blocking raw-byte transmit.  Keeping this separate makes it easy to replace
 * later with a faster FIFO/interrupt/DMA implementation without changing the
 * capbin protocol.
 ******************************************************************************/
static void captureSendBuffer(const u8 *data, u32 length)
{
    for(u32 i = 0U; i < length; i++) {
        captureSendByte(data[i]);
    }
}


/*****************************************************************************/
/**
 * doCaptureBin
 *
 * Send the capture buffer as raw binary bytes.  The payload is exactly
 * CAPTURE_BYTES bytes (131072 x 16-bit samples = 262144 bytes).
 *
 * Protocol:
 *     CAPBIN_BEGIN <samples> <bytes> <address>\r\n
 *     <exactly CAPTURE_BYTES raw bytes>
 *     \r\nCAPBIN_END\r\n
 *
 * The receiver MUST read exactly the byte count declared in CAPBIN_BEGIN
 * before interpreting any following bytes as text, because the payload may
 * contain arbitrary values including CR/LF and the ASCII trailer itself.
 ******************************************************************************/
void doCaptureBin(void)
{
    const u8 *capture = (const u8 *)(UINTPTR)CAPTURE_BASE_ADDR;

    /* Refresh CPU view if PL/DMA populated this memory. */
    Xil_DCacheInvalidateRange((INTPTR)CAPTURE_BASE_ADDR, (u32)CAPTURE_BYTES);

    xil_printf("CAPBIN_BEGIN %d %d 0x500000000\r\n",
               (int)CAPTURE_NUM_SAMPLES,
               (int)CAPTURE_BYTES);

    /* Raw memory bytes; Zynq MPSoC is little-endian, matching '<i2' in Python. */
    captureSendBuffer(capture, (u32)CAPTURE_BYTES);

    xil_printf("\r\nCAPBIN_END\r\n");
}

/*****************************************************************************/
/**
*
* Help function for command interface
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void doExit(void) {

    xil_printf("Exiting application...\r\n");
    cmdExitVal = 1;
}


/*****************************************************************************/
/**
*
* outbyte: Override for BSP outbyte that removes '\r' when printing to
*          Coresight UART
*
* @param	c contains character to output to Coresight JTAG UART
*
* @return
*		- void
*
* @note		None.
*
******************************************************************************/
#if defined (STRIP_CHAR_CR) && STDIN_BASEADDRESS==0xFE800000
void outbyte(char c) {

	if(c == '\r') {
		return;
	}
	XCoresightPs_DccSendByte(STDOUT_BASEADDRESS, c);
}
#endif


/*****************************************************************************/
/**
*
* myChar_checkRx: Checks UartPs or coresight jtag uart for available rx data
*
* @param	BaseAddress of uart
*
* @return
*		- 1 if a character is available from the uart
*   - 0 if no char available
*
* @note		None.
*
******************************************************************************/
int myChar_checkRx(u32 BaseAddress)
{
	int val=0;

	//If mpsoc coresight uart
	if(BaseAddress == 0xFE800000) {
		if((my_XCoresightPs_DccGetStatus() & XCORESIGHTPS_DCC_STATUS_RX)) {
			val = 1;
		}
		dsb();

	//Do mpsoc PS uart
	} else {
		if(XUartPs_IsReceiveData(BaseAddress)) {
			val = 1;
		}
	}

	return val;
}


/*****************************************************************************/
/**
*
* myChar_getRx: Read char from UartPs or coresight jtag uart
*
* @param	BaseAddress of uart
*
* @return
*		- Data from uart
*
* @note		None.
*
******************************************************************************/
u8 myChar_getRx(u32 BaseAddress)
{
	u8 Data;

	//If mpsoc coresight uart
	if(BaseAddress == 0xFE800000) {

		#ifdef __aarch64__
			asm volatile ("mrs %0, dbgdtrrx_el0" : "=r" (Data));
		#elif defined (__GNUC__) || defined (__ICCARM__)
			asm volatile("mrc p14, 0, %0, c0, c5, 0"
					: "=r" (Data));
		#else
			{
				volatile register u32 Reg __asm("cp14:0:c0:c5:0");
				Data = Reg;
			}
		#endif
		isb();

	//Do mpsoc PS uart
	} else {
		Data = (u8)(XUartPs_ReadReg(BaseAddress, XUARTPS_FIFO_OFFSET));
	}

	return Data;
}


/*****************************************************************************/
/**
*
* my_XCoresightPs_DccGetStatus: Check Coresight JTAG UART for valid data
*
* @param	moid
*
* @return
*		- Status = 0 if no char available
*
* @note		None.
*
******************************************************************************/
u32 my_XCoresightPs_DccGetStatus(void)
{
	u32 Status = 0U;

#ifdef __aarch64__
	asm volatile ("mrs %0, mdccsr_el0" : "=r" (Status));
#elif defined (__GNUC__) || defined (__ICCARM__)
	asm volatile("mrc p14, 0, %0, c0, c1, 0"
			: "=r" (Status) : : "cc");
#else
	{
		volatile register u32 Reg __asm("cp14:0:c0:c1:0");
		Status = Reg;
	}
#endif
	return Status;
}


/*****************************************************************************/
/**
*
* my_strcpy
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/
char *my_strcpy(char *DestPtr, const char *SrcPtr)
{
	unsigned int Count;

	for (Count=0U; SrcPtr[Count] != '\0'; ++Count)
	{
		DestPtr[Count] = SrcPtr[Count];
	}
	DestPtr[Count] = '\0';

	return DestPtr;
}


/*****************************************************************************/
/**
*
* getAndParseInput
*
* @param	TBD
*
* @return	TBD
*
* @note		TBD
*
******************************************************************************/
char * my_strcat(char* Str1Ptr, const char* Str2Ptr)
{
	while( *Str1Ptr )
	{
		Str1Ptr++;
	}

	while( *Str2Ptr )
	{
		*Str1Ptr = *Str2Ptr;
		Str1Ptr++; Str2Ptr++;
	}

	*Str1Ptr = '\0';
	return --Str1Ptr;
}
