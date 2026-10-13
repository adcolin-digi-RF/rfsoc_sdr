/*
 * play_dma.c
 *
 *  Created on: Nov 12, 2019
 *      Author: jakobc
 *  10feb20: (JM) Added dacSinCos()
 *
 *  Note: dacSin(), & dacSinCos() require the math library.
 *        To set: C/C++ build settings requires -lm (Library 'm' added) to
 *        gcc Linker.
 */
/***************************** Include Files *********************************/
#include "play_dma.h"
#include "xil_io.h"
#include "xil_types.h"
#include <stdio.h>
#include "cli.h"
#include "xparameters.h"
#include "main.h"
#include "xaxidma.h"
#include "xil_mmu.h"
#include <math.h>
#include "xrfdc.h"



/************************** Constant Definitions *****************************/

// Define used to switch between assignment of dma BD and buffer space using
// defines or using linkerscript sections
#define TX_USE_LNK_ASSIGNMENTS 1

// If 1, disable the use of GPIO to control the FIFO TREADY signal. Without this control
// there will be bubbles in the DAC axis interface as the DMA starts playing.
#define DISABLE_DAC_GPIO 0


// Device hardware build related constants.
#define DMA_DEV_ID				XPAR_AXI_DMA_DAC_DEVICE_ID

#if DISABLE_DAC_GPIO != 1
#define GPIO_DAC_BASE 			XPAR_AXI_GPIO_DAC_BASEADDR

#define FIFO_TREADY_BIT		0x1
#endif

/*
 * Buffer and Buffer Descriptor related constant definition
 */
#define MARK_UNCACHEABLE    (NORM_NONCACHE | INNER_SHAREABLE)


// Define DDR address map. Linkerscript has been modified to carve out
// following memory usage. Make sure there are no overlaps with other
// sections of code such as a RX DMA
#if TX_USE_LNK_ASSIGNMENTS == 0

#define PS_DDR_BASE_ADDR	(XPAR_PSU_DDR_0_S_AXI_BASEADDR)
#define MIG_DDR_BASE_ADDR	(XPAR_DDR4_0_BASEADDR)

#define TX_BUFFER_BASE		(MIG_DDR_BASE_ADDR + 0x00000000)
#define TX_BUFFER_HIGH		(MIG_DDR_BASE_ADDR + 0x07FFFFFF)

#define TX_BD_SPACE_BASE	(PS_DDR_BASE_ADDR + 0x00100000)
#define TX_BD_SPACE_HIGH	(PS_DDR_BASE_ADDR + 0x00100FFF)

#endif

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

/************************** Variable Definitions *****************************/

extern XRFdc RFdcInst;

XAxiDma txAxiDma;

// values that hold dma BD and buffer address info. Define TX_USE_LNK_ASSIGNMENTS
// switches between using absolute addresses from previous defines or using
// values from section assignments in the linker script lscript.ld

#if TX_USE_LNK_ASSIGNMENTS == 0
UINTPTR tx_bd_base  = TX_BD_SPACE_BASE;
UINTPTR tx_bd_high  = TX_BD_SPACE_HIGH;
UINTPTR tx_buf_base = TX_BUFFER_BASE;
UINTPTR tx_buf_high = TX_BUFFER_HIGH;
#else

extern const UINTPTR _tx_bd_space_start;
extern const UINTPTR _tx_bd_space_end;
extern const UINTPTR _tx_buf_space_start;
extern const UINTPTR _tx_buf_space_end;

const UINTPTR tx_bd_base  = (UINTPTR)&_tx_bd_space_start;
const UINTPTR tx_bd_high  = (UINTPTR)&_tx_bd_space_end;
const UINTPTR tx_buf_base = (UINTPTR)&_tx_buf_space_start;
const UINTPTR tx_buf_high = (UINTPTR)&_tx_buf_space_end;

#endif

/************************** Function Definitions ******************************/
void TxDmaInit(void);


/*****************************************************************************/
/**
*
* cli_play_dma_init: Add functions from this file to CLI
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void cli_play_dma_init(void)
{
	static CMDSTRUCT cliCmds[] = {
		//000000000011111111112222    000000000011111111112222222222333333333
		//012345678901234567890123    012345678901234567890123456789012345678
		{"###################### DAC DMA #########################", " "			    , 0, *cmdComment},
		{"dacSawtooth" , "<numKB> <increment per sample> - write sawtooth to DDR"	, 2, *dacSawtooth},
		{"dacSin"      , "<numKB> <freq (Hz)> <dBFS>"                                    , 3, *dacSin},
//		{"dacSinCos"   , "<numKB> <freq (Hz)> <dBFS>"                                    , 3, *dacSinCos}, // This is for an IQ design but the current design is real.
		{"dacPlay" 	   , "<numKB> - start DAC player"								, 1, *dacPlay},
		{"dacStop"	   , "- stop DAC player"										, 0, *dacStop},
		{" "		   , " "														, 0, *cmdComment},
	};


	cli_addCmds(cliCmds, sizeof(cliCmds)/sizeof(cliCmds[0]));

	TxDmaInit();
}


/*****************************************************************************/
/**
*
* tx_dma_init: Initializes TX DMA
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void TxDmaInit(void)
{
	XAxiDma_Config *Config;

	Config = XAxiDma_LookupConfig(DMA_DEV_ID);
	XAxiDma_CfgInitialize(&txAxiDma, Config);
}


/*****************************************************************************/
/*
*
* This function sets up the TX channel of a DMA engine to be ready for packet
* transmission.
*
* @param	AxiDmaInstPtr is the pointer to the instance of the DMA engine.
*
* @return	- XST_SUCCESS if the setup is successful.
*		- XST_FAILURE otherwise.
*
* @note		None.
*
******************************************************************************/
static int TxSetup(XAxiDma * AxiDmaInstPtr, u32 BdCount, u32 BytesPerBd)
{
	XAxiDma_Bd BdTemplate;
	XAxiDma_BdRing *TxRingPtr;
	int Status;

	TxRingPtr = XAxiDma_GetTxRing(AxiDmaInstPtr);

	Status = XAxiDma_BdRingCreate(TxRingPtr, tx_bd_base, tx_bd_base,
				     XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed create BD ring\r\n");
		return XST_FAILURE;
	}

	/*
	 * Create a template and set all BDs to be the
	 * same as the template. The sender has to set up the BDs as needed.
	 */
	XAxiDma_BdClear(&BdTemplate);
	Status = XAxiDma_BdRingClone(TxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed clone BDs\r\n");
		return XST_FAILURE;
	}

	/* Enable Cyclic DMA mode */
	XAxiDma_BdRingEnableCyclicDMA(TxRingPtr);
	XAxiDma_SelectCyclicMode(AxiDmaInstPtr, XAXIDMA_DMA_TO_DEVICE, 1);

	/* Assert TREADY on FIFO, allowing data flow to DAC */
#if DISABLE_DAC_GPIO != 1
	{
		u32 tmpVal;

		tmpVal = Xil_In32(GPIO_DAC_BASE);
		tmpVal = tmpVal | FIFO_TREADY_BIT;		//set TREADY
		Xil_Out32(GPIO_DAC_BASE, tmpVal);
	}
#endif

	/* Start the TX channel */
	Status = XAxiDma_BdRingStart(TxRingPtr);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed bd start\r\n");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

/*****************************************************************************/
/*
*
* This function non-blockingly transmits all packets through the DMA engine.
*
* @param	AxiDmaInstPtr points to the DMA engine instance
*
* @return
* 		- XST_SUCCESS if the DMA accepts all the packets successfully,
* 		- XST_FAILURE if error occurs
*
* @note		None.
*
******************************************************************************/
static int SendPacket(XAxiDma * AxiDmaInstPtr, u32 BytesPerBd)
{
	XAxiDma_BdRing *TxRingPtr;
	XAxiDma_Bd *BdPtr, *BdCurPtr;
	int Status;
	int Index;
	UINTPTR BufferAddr;
	u32 FreeBdCount;

	TxRingPtr = XAxiDma_GetTxRing(AxiDmaInstPtr);
	FreeBdCount = XAxiDma_BdRingGetFreeCnt(TxRingPtr);

	Status = XAxiDma_BdRingAlloc(TxRingPtr, FreeBdCount,
								&BdPtr);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed bd alloc\r\n");
		return XST_FAILURE;
	}

	BufferAddr = (UINTPTR)tx_buf_base;
	BdCurPtr = BdPtr;


	/*
	 * Set up the BD using the information of the packet to transmit
	 */
	for(Index = 0; Index < FreeBdCount; Index++) {
		u32 CrBits = 0;

		Status = XAxiDma_BdSetBufAddr(BdCurPtr, BufferAddr);
		if (Status != XST_SUCCESS) {
			xil_printf("Tx set buffer addr 0x%p on BD %x failed %d\r\n",
			(unsigned int)BufferAddr,
			(UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		Status = XAxiDma_BdSetLength(BdCurPtr, BytesPerBd,
					TxRingPtr->MaxTransferLen);
		if (Status != XST_SUCCESS) {
			xil_printf("Tx set length %d on BD %x failed %d\r\n",
					BytesPerBd, (UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		if (Index == 0) {
			/* The first BD has SOF set
			 */
			CrBits |= XAXIDMA_BD_CTRL_TXSOF_MASK;

		}

		if(Index == (FreeBdCount - 1)) {
			/* The last BD should have EOF and IOC set
			 */
			CrBits |= XAXIDMA_BD_CTRL_TXEOF_MASK;
		}

		XAxiDma_BdSetCtrl(BdCurPtr, CrBits);
		XAxiDma_BdSetId(BdCurPtr, BufferAddr);

		BufferAddr += BytesPerBd;
		BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(TxRingPtr, BdCurPtr);
	}

	/* Give the BD to hardware */
	Status = XAxiDma_BdRingToHw(TxRingPtr, FreeBdCount,
						BdPtr);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed to hw, length %d\r\n",
			(int)XAxiDma_BdGetLength(BdPtr,
					TxRingPtr->MaxTransferLen));

		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* play: Calculates the values for the buffer descriptors, starts the DMA
* and then begins cyclic transfer from TX buffer space.
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void dacPlay (u32 *cmdVals)
{
	u32 BdCount;
	u32 BytesPerBd;
	u32 NumBytes;
	XAxiDma_BdRing *TxRingPtr;
	int Status;

	NumBytes = cmdVals[0] * 1024;

	// mark BD space as uncachable with a resolution of 2MB per addr
	Xil_SetTlbAttributes(tx_bd_base, MARK_UNCACHEABLE);

	/* Force deassert TREADY on FIFO to allow it to fill and avoid data bubbles */
#if DISABLE_DAC_GPIO != 1
	{
		u32 tmpVal;

		tmpVal = Xil_In32(GPIO_DAC_BASE);
		tmpVal = tmpVal & ~FIFO_TREADY_BIT;		//clr TREADY
		Xil_Out32(GPIO_DAC_BASE, tmpVal);
	}
#endif

	TxRingPtr = XAxiDma_GetTxRing(&txAxiDma);

	if (NumBytes < 0x1000)
	{
		NumBytes = 0x1000;
		xil_printf("Warning: number of bytes must be at least 4K. Rounding up.\r\n");
	}
	if (NumBytes > (tx_buf_high - tx_buf_base + 1)) {
		NumBytes = tx_buf_high - tx_buf_base + 1;
		xil_printf("Warning: exceeded TX buffer memory space. Reducing to max.\r\n");
	}
	if (NumBytes % 0x1000 != 0) {
		xil_printf("Warning: number of bytes must be 4K aligned. Rounding down.\r\n");
		NumBytes &= ~0xFFF;
	}
	BdCount = (((NumBytes + (TxRingPtr->MaxTransferLen) - 1)) / (TxRingPtr->MaxTransferLen));
	while (NumBytes % BdCount != 0) {
		BdCount++;
	}
	if (BdCount > XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, tx_bd_high - tx_bd_base + 1)) {
		xil_printf("Error: exceeded buffer descriptor memory space. Try transmitting less bytes.\r\n");
		return;
	}
	BytesPerBd = NumBytes / BdCount;
	xil_printf("Starting cyclic transfer of %d bytes from memory location 0x%p\r\n", NumBytes, tx_buf_base);

	/* Set up TX/RX channels to be ready to transmit and receive packets */
	Status = TxSetup(&txAxiDma, BdCount, BytesPerBd);

	if (Status != XST_SUCCESS) {

		xil_printf("Failed TX setup\r\n");
		return;
	}

	/* Send a packet */
	Status = SendPacket(&txAxiDma, BytesPerBd);
	if (Status != XST_SUCCESS) {

		xil_printf("Failed send packet\r\n");
		return;
	}

	return;
}

/*****************************************************************************/
/**
*
* stop: Resets DMA to stop player.
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void dacStop (u32 *cmdVals)
{
	XAxiDma_Reset(&txAxiDma);
	return;
}

/*****************************************************************************/
/**
*
* sawtooth: Writes a sawtooth wave pattern into TX buffer memory space with
* user-specified size and increment per sample.
*
* @param	cmdVals[0] = numKBytes  cmdVals[1] = increment value
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void dacSawtooth (u32 *cmdVals)
{
	u32 NumBytes;
	u32 increment = cmdVals[1];
	u16 *TxPacket;
	u16 Value;
	int Index;

	NumBytes = cmdVals[0] * 1024;
	TxPacket = (u16 *) tx_buf_base;

	if (NumBytes < 0x1000) {
		NumBytes = 0x1000;
		xil_printf("Warning: number of bytes must be at least 4K. Rounding up.\r\n");
	}
	if (NumBytes > (tx_buf_high - tx_buf_base + 1)) {
		NumBytes = tx_buf_high - tx_buf_base + 1;
		xil_printf("Warning: number of bytes exceeded TX buffer memory space. Reducing to max.\r\n");
	}
	if (NumBytes % 0x1000 != 0) {
		xil_printf("Warning: number of bytes must be 4K aligned. Rounding down.\r\n");
		NumBytes &= ~0xFFF;
	}

	// Verify increment value is a power of 2
	if ( !(increment && (!(increment & (increment-1)))) ) {
		increment = 1;
		xil_printf("Warning: increment per sample must be a power of 2 in order to be evenly divisible into 4KB. Defaulting to 1.\r\n");
	}


	xil_printf("Writing %d bytes of sawtooth pattern with increment value of %d to memory location 0x%p\r\n", NumBytes, increment, tx_buf_base);


	// each sample is 16bits
	Value = 0x0;
	for(Index = 0; Index < NumBytes/sizeof(TxPacket[0]); Index++) {
		TxPacket[Index] = Value;

		Value += increment;
	}

	/* Flush the SrcBuffer before the DMA transfer, in case the Data Cache
	 * is enabled
	 */
	Xil_DCacheFlushRange((UINTPTR)TxPacket, NumBytes);
	return;
}



/*****************************************************************************/
/**
*
* sinCos Gen will fill memory with a I/Q sine wave
*
* @param	cmdVals[0] = numKBytes cmdVals[1] = freq  cmdVals[2] = dBFS
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void dacSinCos(u32 *cmdVals) {
	s16 iVal, qVal;
	u32 *TxPacket;
	u32 NumBytes;
	int amplitude;
	double SamplingFreq;
	double dBFS;
	double freq;
	double stepSize;
	double newFreq;
	u32 numCycles;
	int Status;

	NumBytes = cmdVals[0] * 1024;
	freq = (int)cmdVals[1];
	dBFS = (int)cmdVals[2];

	// TxPacket is u32 since it points to I/Q combination
	TxPacket = (u32 *) tx_buf_base;

	if (NumBytes < 0x1000) {
		NumBytes = 0x1000;
		xil_printf("Warning: number of bytes must be at least 4K. Rounding up.\r\n");
	}
	if (NumBytes > (tx_buf_high - tx_buf_base + 1)) {
		NumBytes = tx_buf_high - tx_buf_base + 1;
		xil_printf("Warning: number of bytes exceeded TX buffer memory space. Reducing to max.\r\n");
	}
	if (NumBytes % 0x1000 != 0) {
		xil_printf("Warning: number of bytes must be 4K aligned. Rounding up.\r\n");
		NumBytes += 0x1000;
		NumBytes &= ~0xFFF;
	}

	//Calculate amplitude in hex from input of dBFS
	{
		double ratio;

		ratio = pow(10.0, (dBFS/20));
		amplitude = 0x7ffe * ratio;
		printf("ratio: %f, amplitude: 0x%08x\r\n", ratio, amplitude);

		if(amplitude>0x7FFF) {
			xil_printf("ERROR: Amplitude greater than 16bits\r\n");
			return;
		}
	}

	//Get sampling rate and interpolation of DAC 7 and use for sine freq calculation
	{
		XRFdc* RFdcInstPtr = &RFdcInst;
		XRFdc_BlockStatus blockStatus;
	    u32 InterpolationFactor;

		if(XRFdc_IsDACBlockEnabled(RFdcInstPtr, 1, 3)) {
			XRFdc_GetBlockStatus(RFdcInstPtr, XRFDC_DAC_TILE, 1, 3, &blockStatus);
			SamplingFreq = blockStatus.SamplingFreq * 1e9;
		} else {
			xil_printf("Error reading DAC 7 sampling rate\r\n");
			return;
		}

		Status = XRFdc_GetInterpolationFactor(RFdcInstPtr, 1, 3, &InterpolationFactor);
		if (Status != XST_SUCCESS) {
			xil_printf("XRFdc_GetInterpolationFactor() failed\r\n");
			return;
		}

		// update Sampling Freq with interpolation factor to represent sampling freq
		// of axis interface
		SamplingFreq = SamplingFreq / (float)InterpolationFactor;

	}


	//determine how many sine waves fit in buffer then round up to slightly
	//increase freq to fit buffer
	numCycles = (float)((NumBytes/sizeof(TxPacket[0])) / (SamplingFreq / freq));

	// determine new freq that evenly fits into buffer
	newFreq = 1/ ((NumBytes/sizeof(TxPacket[0])) / numCycles * 1/SamplingFreq);


	stepSize = (float)360 / ((float)(NumBytes/sizeof(TxPacket[0])) / numCycles);

	printf("Adjusted Sampling Frequency: %f Hz\r\n", SamplingFreq);
	printf("freq:                        %f\r\n", freq);
	printf("newFreq:                     %f\r\n", newFreq);
	printf("num cycles in buffer:  %d\r\n", numCycles);
	printf("calculated step size:  %f\r\n", stepSize);


	xil_printf("Writing %d bytes of sin/cos pattern to memory location 0x%p\r\n", NumBytes, tx_buf_base);

    for(int i=0; i < NumBytes/sizeof(TxPacket[0]); i++) {
    	// y(t) = A * sin(2 * PI * f * t + shift)
    	// A = amplitude, peak deviation of function from zero
    	// f = ordinary freq, number of oscillations (cycles)
    	// t = time
    	// shift = phase shift

    	float rads = M_PI/180;

    	// Calculate I/Q values and write to memory
    	iVal = amplitude * sin(stepSize * i * rads);
    	qVal = amplitude * cos(stepSize * i * rads);
    	TxPacket[i] = ((iVal << 16) & 0xFFFF0000) | (qVal & 0xFFFF);

 //   	if( (i > 140) && (i < 160) ) {
 //   		xil_printf("i: %d addr: 0x%p I/Q: 0x%04x 0x%04x\r\n",i, &TxPacket[i],iVal, qVal);
 //   	}
    }

	/* Flush the SrcBuffer before the DMA transfer, in case the Data Cache
	 * is enabled
	 */
	Xil_DCacheFlushRange((UINTPTR)TxPacket, NumBytes);
	return;
}


/*****************************************************************************/
/**
*
* sin Gen will fill memory with a Real sine wave
*
* @param	cmdVals[0] = numKBytes cmdVals[1] = freq  cmdVals[2] = dBFS
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void dacSin(u32 *cmdVals) {
	s16 Val;
	u16 *TxPacket;
	u32 NumBytes;
	int amplitude;
	double SamplingFreq;
	double dBFS;
	double freq;
	double stepSize;
	double newFreq;
	u32 numCycles;
	int Status;

	NumBytes = cmdVals[0] * 1024;
	freq = (int)cmdVals[1];
	dBFS = (int)cmdVals[2];

	// TxPacket is u16 since it points to Real combination
	TxPacket = (u16 *) tx_buf_base;

	if (NumBytes < 0x1000) {
		NumBytes = 0x1000;
		xil_printf("Warning: number of bytes must be at least 4K. Rounding up.\r\n");
	}
	if (NumBytes > (tx_buf_high - tx_buf_base + 1)) {
		NumBytes = tx_buf_high - tx_buf_base + 1;
		xil_printf("Warning: number of bytes exceeded TX buffer memory space. Reducing to max.\r\n");
	}
	if (NumBytes % 0x1000 != 0) {
		xil_printf("Warning: number of bytes must be 4K aligned. Rounding up.\r\n");
		NumBytes += 0x1000;
		NumBytes &= ~0xFFF;
	}

	//Calculate amplitude in hex from input of dBFS
	{
		double ratio;

		ratio = pow(10.0, (dBFS/20));
		amplitude = 0x7ffe * ratio;
		printf("ratio: %f, amplitude: 0x%08x\r\n", ratio, amplitude);

		if(amplitude>0x7FFF) {
			xil_printf("ERROR: Amplitude greater than 16bits\r\n");
			return;
		}
	}

	//Get sampling rate and interpolation of DAC 7 and use for sine freq calculation
	{
		XRFdc* RFdcInstPtr = &RFdcInst;
		XRFdc_BlockStatus blockStatus;
	    u32 InterpolationFactor;

		if(XRFdc_IsDACBlockEnabled(RFdcInstPtr, 1, 3)) {
			XRFdc_GetBlockStatus(RFdcInstPtr, XRFDC_DAC_TILE, 1, 3, &blockStatus);
			SamplingFreq = blockStatus.SamplingFreq * 1e9;
		} else {
			xil_printf("Error reading DAC 7 sampling rate\r\n");
			return;
		}

		Status = XRFdc_GetInterpolationFactor(RFdcInstPtr, 1, 3, &InterpolationFactor);
		if (Status != XST_SUCCESS) {
			xil_printf("XRFdc_GetInterpolationFactor() failed\r\n");
			return;
		}

		// update Sampling Freq with interpolation factor to represent sampling freq
		// of axis interface
		SamplingFreq = SamplingFreq / (float)InterpolationFactor;
	}


	//determine how many sine waves fit in buffer then round up to slightly
	//increase freq to fit buffer
	numCycles = (float)((NumBytes/sizeof(TxPacket[0])) / (SamplingFreq / freq));

	// determine new freq that evenly fits into buffer
	newFreq = 1/(((1/SamplingFreq)*NumBytes/sizeof(TxPacket[0]))/numCycles);


	stepSize = (float)360 / ((float)(NumBytes/sizeof(TxPacket[0])) / numCycles);

	printf("Adjusted Sampling Frequency: %f Hz\r\n", SamplingFreq);
	printf("Requested Frequency:         %f Hz\r\n", freq);
	printf("Adjusted Frequency:          %f Hz\r\n", newFreq);
	printf("num cycles in buffer:  %d\r\n", numCycles);
	printf("calculated step size:  %f\r\n", stepSize);


	xil_printf("Writing %d bytes of sin pattern to memory location 0x%p\r\n", NumBytes, tx_buf_base);

    for(int i=0; i < NumBytes/sizeof(TxPacket[0]); i++) {
    	// y(t) = A * sin(2 * PI * f * t + shift)
    	// A = amplitude, peak deviation of function from zero
    	// f = ordinary freq, number of oscillations (cycles)
    	// t = time
    	// shift = phase shift

    	float rads = M_PI/180;

    	// Calculate Sine values and write to memory
    	Val = amplitude * sin(stepSize * i * rads);
    	TxPacket[i] = Val;

 //   	if( (i > 140) && (i < 160) ) {
 //   		xil_printf("i: %d addr: 0x%p Real: 0x%04x\r\n", i, &TxPacket[i], Val);
  //  	}
    }

	/* Flush the SrcBuffer before the DMA transfer, in case the Data Cache
	 * is enabled
	 */
	Xil_DCacheFlushRange((UINTPTR)TxPacket, NumBytes);
	return;
}
