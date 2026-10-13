/*
 * cap_dma.c
 *
 *  Created on: Nov 12, 2019
 *      Author: jakobc
 */
/***************************** Include Files *********************************/
#include "cap_dma.h"

#include "xil_io.h"
#include "xil_types.h"
#include <stdio.h>
#include "cli.h"
#include "xparameters.h"
#include "main.h"
#include "xaxidma.h"
#include "xil_mmu.h"
#include "eth_ddr.h"
#include "xtime_l.h"


/************************** Constant Definitions *****************************/

// Define used to switch between assignment of dma BD and buffer space using
// defines or using linkerscript sections
#define RX_USE_LNK_ASSIGNMENTS 1

// Device hardware build related constants.
#define DMA_DEV_ID			XPAR_AXI_DMA_ADC_DEVICE_ID

#define GPIO_ADC_BASE 		XPAR_AXI_GPIO_ADC_BASEADDR

#define FIFO_RST_BIT			0x1

// Define DDR address map. Linkerscript has been modified to carve out
// following memory usage. Make sure there are no overlaps with other
// sections of code such as a TX DMA
#if RX_USE_LNK_ASSIGNMENTS == 0

#define PS_DDR_BASE_ADDR	(XPAR_PSU_DDR_0_S_AXI_BASEADDR)
#define MIG_DDR_BASE_ADDR	(XPAR_DDR4_0_BASEADDR)

#define RX_BUFFER_BASE		(MIG_DDR_BASE_ADDR + 0x08000000)
#define RX_BUFFER_HIGH		(MIG_DDR_BASE_ADDR + 0x0FFFFFFF)

#define RX_BD_SPACE_BASE	(PS_DDR_BASE_ADDR + 0x00101000)
#define RX_BD_SPACE_HIGH	(PS_DDR_BASE_ADDR + 0x00101FFF)

#endif

#define MARK_UNCACHEABLE    (NORM_NONCACHE | INNER_SHAREABLE)

/*
 * Bound all hardware waits.  A 256 kB capture should finish far faster than
 * these limits; the generous values are intended to catch a genuinely stuck
 * DMA without making the Ethernet server unresponsive for many seconds.
 */
#define ADC_DMA_CAPTURE_TIMEOUT_US  500000U   /* 500 ms */
#define ADC_DMA_RESET_TIMEOUT_US    100000U   /* 100 ms */


/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Function Prototypes ******************************/

/************************** Variable Definitions *****************************/

// values that hold dma BD and buffer address info. Define RX_USE_LNK_ASSIGNMENTS
// switches between using absolute addresses from previous defines or using
// values from section assignments in the linker script lscript.ld

#if RX_USE_LNK_ASSIGNMENTS == 0
UINTPTR rx_bd_base  = RX_BD_SPACE_BASE;
UINTPTR rx_bd_high  = RX_BD_SPACE_HIGH;
UINTPTR rx_buf_base = RX_BUFFER_BASE;
UINTPTR rx_buf_high = RX_BUFFER_HIGH;
#else

extern const UINTPTR _rx_bd_space_start;
extern const UINTPTR _rx_bd_space_end;
extern const UINTPTR _rx_buf_space_start;
extern const UINTPTR _rx_buf_space_end;

const UINTPTR rx_bd_base  = (UINTPTR)&_rx_bd_space_start;
const UINTPTR rx_bd_high  = (UINTPTR)&_rx_bd_space_end;
const UINTPTR rx_buf_base = (UINTPTR)&_rx_buf_space_start;
const UINTPTR rx_buf_high = (UINTPTR)&_rx_buf_space_end;

#endif

/* Number of valid bytes in the most recently completed capture. */
static volatile u32 g_adc_capture_bytes = 0U;

UINTPTR adc_capture_base(void)
{
	return rx_buf_base;
}

u32 adc_capture_bytes(void)
{
	return g_adc_capture_bytes;
}

u32 adc_capture_capacity(void)
{
	UINTPTR capacity = rx_buf_high - rx_buf_base + 1U;
	return (capacity > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)capacity;
}


/************************** Function Definitions ******************************/


/* Keep the ADC-side FIFO in reset while DMA is being configured/recovered.
 * In this design the FIFO reset is active when FIFO_RST_BIT is cleared. */
static void adc_fifo_assert_reset(void)
{
	u32 tmpVal = Xil_In32(GPIO_ADC_BASE);
	tmpVal &= ~FIFO_RST_BIT;
	Xil_Out32(GPIO_ADC_BASE, tmpVal);
}


/* Release the ADC-side FIFO so samples can flow into the DMA. */
static void adc_fifo_release_reset(void)
{
	u32 tmpVal = Xil_In32(GPIO_ADC_BASE);
	tmpVal |= FIFO_RST_BIT;
	Xil_Out32(GPIO_ADC_BASE, tmpVal);
}


/* Convert a microsecond timeout to the platform timestamp-counter units. */
static u64 timeout_ticks_from_us(u32 timeout_us)
{
	return ((u64)timeout_us * (u64)COUNTS_PER_SECOND) / 1000000ULL;
}


/* Wait for the last RX descriptor to complete, but never wait forever.
 * Do NOT call eth_ddr_poll() here: adc_capture_kb() may have been entered from
 * the lwIP receive callback, and recursively re-entering xemacif_input() is
 * unsafe. */
static int wait_for_rx_bd_complete(XAxiDma_Bd *BdLastPtr, u32 timeout_us)
{
	XTime start;
	XTime now;
	u64 timeout_ticks;

	if (BdLastPtr == NULL) {
		return XST_FAILURE;
	}

	timeout_ticks = timeout_ticks_from_us(timeout_us);
	XTime_GetTime(&start);

	while (!XAxiDma_BdHwCompleted(BdLastPtr)) {
		XTime_GetTime(&now);
		if (((u64)now - (u64)start) >= timeout_ticks) {
			return XST_FAILURE;
		}
	}

	return XST_SUCCESS;
}


/* Reset AXI DMA and wait until the hardware reports that reset has completed.
 * The original code issued XAxiDma_Reset() and returned immediately, allowing
 * the next high-rate CAPTURE_GET to reinitialize the DMA while reset could
 * still be in progress. */
static int dma_reset_and_wait(XAxiDma *AxiDmaInstPtr, u32 timeout_us)
{
	XTime start;
	XTime now;
	u64 timeout_ticks;

	if (AxiDmaInstPtr == NULL) {
		return XST_FAILURE;
	}

	XAxiDma_Reset(AxiDmaInstPtr);

	timeout_ticks = timeout_ticks_from_us(timeout_us);
	XTime_GetTime(&start);

	while (!XAxiDma_ResetIsDone(AxiDmaInstPtr)) {
		XTime_GetTime(&now);
		if (((u64)now - (u64)start) >= timeout_ticks) {
			return XST_FAILURE;
		}
	}

	return XST_SUCCESS;
}


/*****************************************************************************/
/**
*
* cli_cap_dma_init Add functions from this file to CLI
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
void cli_cap_dma_init(void)
{
	static CMDSTRUCT cliCmds[] = {
		//000000000011111111112222    000000000011111111112222222222333333333
		//012345678901234567890123    012345678901234567890123456789012345678
    	{"###################### ADC DMA #########################", " "	 , 0, *cmdComment},
		{"adcCapture"              , "<numKB> - initiate ADC capture"        , 1, *adcCapture},
		{" "                       , " "                                     , 0, *cmdComment},
	};

	cli_addCmds(cliCmds, sizeof(cliCmds)/sizeof(cliCmds[0]));
}


/*****************************************************************************/
/**
*
* RxSetup used by capture function to initialize the DMA buffer descriptors
* and start the RX DMA
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
static int RxSetup(XAxiDma * AxiDmaInstPtr, u32 BdCount, u32 BytesPerBd, UINTPTR *BdLastPtr)
{
	XAxiDma_Bd BdTemplate;
	XAxiDma_Bd *BdPtr;
	u32 FreeBdCount;
	UINTPTR RxBufferPtr;
	int Index;
	int Status;
	XAxiDma_Bd *BdCurPtr;
	XAxiDma_BdRing *RxRingPtr;

	RxRingPtr = XAxiDma_GetRxRing(AxiDmaInstPtr);

	Status = XAxiDma_BdRingCreate(RxRingPtr, rx_bd_base, rx_bd_base,
				XAXIDMA_BD_MINIMUM_ALIGNMENT, BdCount);

	if (Status != XST_SUCCESS) {
		xil_printf("RX create BD ring failed %d\r\n", Status);

		return XST_FAILURE;
	}

	/*
	 * Setup an all-zero BD as the template for the Rx channel.
	 */
	XAxiDma_BdClear(&BdTemplate);

	Status = XAxiDma_BdRingClone(RxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {
		xil_printf("RX clone BD failed %d\r\n", Status);

		return XST_FAILURE;
	}

	/* Attach buffers to RxBD ring so we are ready to receive packets */

	FreeBdCount = XAxiDma_BdRingGetFreeCnt(RxRingPtr);

	Status = XAxiDma_BdRingAlloc(RxRingPtr, FreeBdCount, &BdPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("RX alloc BD failed %d\r\n", Status);

		return XST_FAILURE;
	}

	BdCurPtr = BdPtr;
	RxBufferPtr = rx_buf_base;
	for (Index = 0; Index < FreeBdCount; Index++) {
		Status = XAxiDma_BdSetBufAddr(BdCurPtr, RxBufferPtr);

		if (Status != XST_SUCCESS) {
			xil_printf("Set buffer addr 0x%p on BD %x failed %d\r\n",
			    (unsigned int)RxBufferPtr,
			    (UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		Status = XAxiDma_BdSetLength(BdCurPtr, BytesPerBd,
				RxRingPtr->MaxTransferLen);
		if (Status != XST_SUCCESS) {
			xil_printf("Rx set length %d on BD %x failed %d\r\n",
					BytesPerBd, (UINTPTR)BdCurPtr, Status);

			return XST_FAILURE;
		}

		XAxiDma_BdSetCtrl(BdCurPtr, 0);
		XAxiDma_BdSetId(BdCurPtr, RxBufferPtr);
		/* clear the buffer memory pointed to by this bd */
		//memset((void *)RxBufferPtr, 0, BytesPerBd);

		RxBufferPtr += BytesPerBd;
		if (Index != FreeBdCount - 1) { //don't run on last iteration (preserves last Bd address in BdCurPtr)
			BdCurPtr = (XAxiDma_Bd *)XAxiDma_BdRingNext(RxRingPtr, BdCurPtr);
		}
	}

	Status = XAxiDma_BdRingToHw(RxRingPtr, FreeBdCount,
						BdPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("RX submit hw failed %d\r\n", Status);

		return XST_FAILURE;
	}

	/* Start RX DMA channel */
	Status = XAxiDma_BdRingStart(RxRingPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("RX start hw failed %d\r\n", Status);

		return XST_FAILURE;
	}

	// Pass last BD ptr to function caller
	*BdLastPtr = (UINTPTR)BdCurPtr;
	return XST_SUCCESS;
}


/*****************************************************************************/
/**
*
* capture: Calculates the values for the buffer descriptors, starts the DMA
* and then monitors the last buffer descriptor until it is complete. This
* polling removes the requirement for TLAST which normally triggers completion
* of the dma transaction.
*
* @param	None
*
* @return	None
*
* @note		TBD
*
******************************************************************************/
int adc_capture_kb(u32 num_kb, int verbose)
{
	int Status;
	XAxiDma_Config *Config;
	XAxiDma rxAxiDma;
	u32 BdCount;
	u32 BytesPerBd;
	UINTPTR BdLastPtr = 0U;
	XAxiDma_BdRing *RxRingPtr;
	u32 NumBytes;
	int dma_initialized = 0;

	NumBytes = num_kb * 1024U;

	/* A CLI-triggered capture cannot overwrite PL DDR while an Ethernet
	 * response is still being copied into lwIP-owned buffers. */
	while (eth_ddr_transfer_active()) {
		eth_ddr_poll();
	}

	/* A capture is not considered valid until the complete transaction has
	 * finished successfully. */
	g_adc_capture_bytes = 0U;

	/* DMA buffer descriptors live in PS DDR and must remain uncached. */
	Xil_SetTlbAttributes(rx_bd_base, MARK_UNCACHEABLE);

	Config = XAxiDma_LookupConfig(DMA_DEV_ID);
	if (Config == NULL) {
		xil_printf("ADC DMA: no config found for device %d\r\n", DMA_DEV_ID);
		return XST_FAILURE;
	}

	/* Hold the FIFO in reset before touching the DMA so stale ADC data cannot
	 * enter the newly-created descriptor ring. */
	adc_fifo_assert_reset();

	Status = XAxiDma_CfgInitialize(&rxAxiDma, Config);
	if (Status != XST_SUCCESS) {
		xil_printf("ADC DMA: XAxiDma_CfgInitialize failed %d\r\n", Status);
		return XST_FAILURE;
	}
	dma_initialized = 1;

	RxRingPtr = XAxiDma_GetRxRing(&rxAxiDma);

	if (NumBytes < 0x1000U) {
		NumBytes = 0x1000U;
		if (verbose) {
			xil_printf("Warning: number of bytes must be at least 4K. Rounding up.\r\n");
		}
	}

	if ((UINTPTR)NumBytes > (rx_buf_high - rx_buf_base + 1U)) {
		NumBytes = (u32)(rx_buf_high - rx_buf_base + 1U);
		if (verbose) {
			xil_printf("Warning: exceeded RX buffer memory space. Reducing to max.\r\n");
		}
	}

	if ((NumBytes & 0xFFFU) != 0U) {
		if (verbose) {
			xil_printf("Warning: number of bytes must be 4K aligned. Rounding down.\r\n");
		}
		NumBytes &= ~0xFFFU;
	}

	BdCount = (NumBytes + RxRingPtr->MaxTransferLen - 1U) /
	          RxRingPtr->MaxTransferLen;
	while ((NumBytes % BdCount) != 0U) {
		BdCount++;
	}

	if (BdCount > XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
	                                    rx_bd_high - rx_bd_base + 1U)) {
		xil_printf("ADC DMA: exceeded buffer descriptor memory space. Try capturing fewer bytes.\r\n");
		Status = XST_FAILURE;
		goto cleanup;
	}

	BytesPerBd = NumBytes / BdCount;
	if (verbose) {
		xil_printf("Transferring %u bytes to memory location 0x%p\r\n",
		           NumBytes, (void *)rx_buf_base);
	}

	Status = RxSetup(&rxAxiDma, BdCount, BytesPerBd, &BdLastPtr);
	if (Status != XST_SUCCESS) {
		xil_printf("ADC DMA: RxSetup() failed\r\n");
		goto cleanup;
	}

	/* Release FIFO reset and acquire the capture. */
	adc_fifo_release_reset();

	/*
	 * Do not call eth_ddr_poll() here. This function can be invoked from the
	 * lwIP receive callback by CAPTURE_GET; re-entering xemacif_input() from
	 * inside that callback is unsafe.
	 *
	 * The original implementation waited forever here. If the final BD never
	 * completes, the whole TCP server wedges and the PC repeatedly times out and
	 * reconnects. Bound the wait so the DMA can be reset and the board can
	 * continue servicing commands.
	 */
	Status = wait_for_rx_bd_complete((XAxiDma_Bd *)BdLastPtr,
	                                ADC_DMA_CAPTURE_TIMEOUT_US);
	if (Status != XST_SUCCESS) {
		xil_printf("ADC DMA: capture timeout after %u us (bytes=%u, bd_count=%u, bytes_per_bd=%u)\r\n",
		           ADC_DMA_CAPTURE_TIMEOUT_US,
		           NumBytes,
		           BdCount,
		           BytesPerBd);
		Status = XST_FAILURE;
		goto cleanup;
	}

	/* Capture completed. Stop new FIFO data before resetting the DMA. */
	adc_fifo_assert_reset();

	/* Crucial for repeated CAPTURE_GET: do not return until the hardware reset
	 * has actually completed. */
	Status = dma_reset_and_wait(&rxAxiDma, ADC_DMA_RESET_TIMEOUT_US);
	if (Status != XST_SUCCESS) {
		xil_printf("ADC DMA: reset timeout after successful capture\r\n");
		g_adc_capture_bytes = 0U;
		return XST_FAILURE;
	}

	g_adc_capture_bytes = NumBytes;

	if (verbose) {
		xil_printf("ADC DMA: capture complete (%u bytes)\r\n", NumBytes);
	}

	return XST_SUCCESS;

cleanup:
	/* Always leave the FIFO quiescent on error. */
	adc_fifo_assert_reset();

	if (dma_initialized) {
		if (dma_reset_and_wait(&rxAxiDma, ADC_DMA_RESET_TIMEOUT_US) != XST_SUCCESS) {
			xil_printf("ADC DMA: reset timeout during error recovery\r\n");
		}
	}

	g_adc_capture_bytes = 0U;
	return XST_FAILURE;
}

void adcCapture(u32 *cmdVals)
{
	if (cmdVals == NULL) {
		return;
	}

	(void)adc_capture_kb(cmdVals[0], 1);
}
