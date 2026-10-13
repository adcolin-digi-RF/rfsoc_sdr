#ifndef SRC_ETH_DDR_H_
#define SRC_ETH_DDR_H_

#include "xil_types.h"
#include "xstatus.h"

/* Static IPv4 endpoint used by the RFSoC_COM capture server. */
#define ETH_DDR_TCP_PORT 5001U

int eth_ddr_init(void);
void eth_ddr_poll(void);
int eth_ddr_is_ready(void);
int eth_ddr_transfer_active(void);

#endif /* SRC_ETH_DDR_H_ */
