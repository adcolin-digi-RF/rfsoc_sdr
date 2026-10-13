/*
 * eth_ddr.c
 *
 * Bare-metal lwIP RAW-API server for RFSoC_COM ADC capture data.
 *
 * Protocol commands (ASCII, newline terminated):
 *   PING
 *   GET_CAPTURE
 *   GET <offset_bytes> <length_bytes>
 *   CAPTURE <num_kB>
 *   CAPTURE_GET <num_kB>
 *   TONE <MHz>
 *   YES <MHz>                  (alias for TONE)
 *   CHIRP <f0_MHz> <f1_MHz>
 *   PULSE <MHz> <PW_us> <PRI_us>
 *   CLEAR
 *   DACPLAY <num_kB>
 *
 * CAPTURE_GET is intended for the PC GUI: it performs the same ADC DMA capture
 * used by the beamformer CLI and then returns that new capture immediately.
 * The normal CLI "adcCapture <numKB>" command remains available.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "xparameters.h"
#include "xil_mmu.h"
#include "xil_cache.h"
#include "xil_types.h"
#include "xil_printf.h"
#include "xstatus.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"
#include "cap_dma.h"
#include "cli.h"
#include "eth_ddr.h"

#define ETH_DDR_CMD_BYTES        64U
#define ETH_DDR_HEADER_BYTES     16U
#define ETH_TX_CHUNK_MAX         16384U
#define ETH_TX_OUTPUT_CHUNKS     4U
#define MMU_BLOCK_BYTES          0x200000ULL

/* DAC waveform buffer used by the existing ZCU111 waveform-generator path.
 * This is intentionally separate from the ADC capture buffer managed by
 * cap_dma.c. */
#define DAC_DDR_BASE_ADDR        0x580000000ULL
#define DAC_TOTAL_SAMPLES        131072U
#define DAC_TOTAL_BYTES          (DAC_TOTAL_SAMPLES * sizeof(s16))
#define FS_DAC_HZ                3276800000.0
#define DEFAULT_AMPL_DBFS        -5.0
#define PI_CONST                 3.14159265358979323846

/* Set to 1 only while debugging. Per-frame UART prints become measurable once
 * Ethernet is fast because the CLI UART is only 115200 baud. */
#define ETH_DDR_VERBOSE          0

/* Response header, little-endian:
 *   bytes 0..3   = "RFS1"
 *   bytes 4..7   = status (0 = success)
 *   bytes 8..11  = payload byte count
 *   bytes 12..15 = reserved (0)
 * On success, exactly payload byte count raw bytes follow the header.
 */
#define ETH_STATUS_OK             0U
#define ETH_STATUS_NO_CAPTURE     1U
#define ETH_STATUS_BAD_COMMAND    2U
#define ETH_STATUS_BAD_RANGE      3U
#define ETH_STATUS_CAPTURE_FAILED 4U
#define ETH_STATUS_BAD_WAVEFORM  5U
#define ETH_STATUS_DACPLAY_FAILED 6U

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
void tcp_fasttmr(void);
void tcp_slowtmr(void);

/* cli_play_dma_init() registers the existing beamformer dacPlay command in
 * cliCmdTable before eth_ddr_init() is called from main.c.  Using the command
 * table here lets Ethernet request the exact same DAC DMA path as the terminal
 * command "dacPlay 256" without duplicating the play-DMA implementation. */
extern TBLSTRUCT cliCmdTable[TBL_PTR_MAX];
extern int numTablePtrs;

static struct netif server_netif;
/* platform_zynqmp.c from the existing lwIP server expects this symbol. */
struct netif *echo_netif = NULL;

static struct tcp_pcb *listen_pcb = NULL;
static int eth_ready = 0;
static volatile int transfer_active = 0;

typedef struct {
    struct tcp_pcb *pcb;
    char cmd[ETH_DDR_CMD_BYTES];
    u16 cmd_len;
} eth_client_state_t;

static eth_client_state_t client;

static int eth_cli_cmd_equal(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL)) {
        return 0;
    }

    while (*a && *b) {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }

    return ((*a == '\0') && (*b == '\0'));
}

/* Invoke a one-argument command that is already registered with the normal
 * beamformer CLI.  This is intentionally used for dacPlay so Ethernet and the
 * terminal exercise the same DMA implementation. */
static int invoke_registered_cli_u32(const char *command_name, u32 value)
{
    int i;

    for (i = 0; i < numTablePtrs; i++) {
        int j;
        TBLSTRUCT *table = &cliCmdTable[i];

        for (j = 0; j < table->cmdCount; j++) {
            CMDSTRUCT *entry = &table->cmdPointer[j];

            if (eth_cli_cmd_equal(entry->cmdString, command_name)) {
                u32 values[1];

                if ((entry->valCnt != 1U) || (entry->cmdFunc == NULL)) {
                    return XST_FAILURE;
                }

                values[0] = value;
                entry->cmdFunc(values);
                return XST_SUCCESS;
            }
        }
    }

    return XST_FAILURE;
}

static void put_u32_le(u8 *dst, u32 value)
{
    dst[0] = (u8)(value & 0xFFU);
    dst[1] = (u8)((value >> 8) & 0xFFU);
    dst[2] = (u8)((value >> 16) & 0xFFU);
    dst[3] = (u8)((value >> 24) & 0xFFU);
}

static void reset_client_state(void)
{
    memset(&client, 0, sizeof(client));
    transfer_active = 0;
}

static void print_addr(UINTPTR addr)
{
    xil_printf("0x%08x%08x", (u32)(addr >> 32), (u32)addr);
}

/* Xil_SetTlbAttributes() changes one 2 MiB A53 translation block. Map only
 * the DDR blocks the CPU is about to access. Capture mappings are still done
 * only AFTER ADC DMA completes; the separate DAC waveform block is mapped
 * immediately before CPU waveform writes. */
static void map_ddr_range_noncacheable(UINTPTR base, u32 length)
{
    UINTPTR addr;
    UINTPTR end;

    if (length == 0U) {
        return;
    }

    addr = base & ~((UINTPTR)MMU_BLOCK_BYTES - 1U);
    end = base + (UINTPTR)length;

    while (addr < end) {
        Xil_SetTlbAttributes(addr, NORM_NONCACHE);
        addr += (UINTPTR)MMU_BLOCK_BYTES;
    }
}

/* Waveform generation helpers. These mirror the known-good new_echo.c path:
 * 131072 signed 16-bit samples are written at 0x580000000, with a DAC sample
 * rate of 3.2768 GS/s and a default amplitude of -5 dBFS. */
static u32 ceil_positive_double_to_u32(double x)
{
    u32 y = (u32)x;

    if ((double)y < x) {
        y++;
    }

    return y;
}

static s16 clamp_to_s16(double y)
{
    if (y > 32767.0) {
        return 32767;
    }

    if (y < -32768.0) {
        return -32768;
    }

    return (s16)y;
}

static double dbfs_to_scale(double dbfs)
{
    return 32768.0 * pow(10.0, dbfs / 20.0);
}

static void prepare_dac_ddr(void)
{
    map_ddr_range_noncacheable((UINTPTR)DAC_DDR_BASE_ADDR,
                               (u32)DAC_TOTAL_BYTES);
}

static void clear_dac_ddr_buffer(void)
{
    s16 *dst = (s16 *)(UINTPTR)DAC_DDR_BASE_ADDR;
    u32 n;

    prepare_dac_ddr();

    for (n = 0U; n < DAC_TOTAL_SAMPLES; n++) {
        dst[n] = 0;
    }

    Xil_DCacheFlushRange((UINTPTR)dst, (u32)DAC_TOTAL_BYTES);
}

static void generate_dac_tone(double freq_mhz)
{
    s16 *dst = (s16 *)(UINTPTR)DAC_DDR_BASE_ADDR;
    double freq_hz = freq_mhz * 1.0e6;
    double total_sample_time = ((double)DAC_TOTAL_SAMPLES) / FS_DAC_HZ;
    double fc_cycles = total_sample_time * freq_hz;
    u32 coherent_cycles = ceil_positive_double_to_u32(fc_cycles + 3.0);
    double fc_new = ((double)coherent_cycles) / total_sample_time;
    double scale = dbfs_to_scale(DEFAULT_AMPL_DBFS);
    u32 n;

    prepare_dac_ddr();

#if ETH_DDR_VERBOSE
    xil_printf("TCP: generating tone requested=%lu kHz coherent=%lu kHz\r\n",
               (u32)(freq_mhz * 1000.0),
               (u32)((fc_new / 1.0e6) * 1000.0));
#endif

    for (n = 0U; n < DAC_TOTAL_SAMPLES; n++) {
        double phase = 2.0 * PI_CONST * fc_new * ((double)(n + 1U)) / FS_DAC_HZ;
        double y = cos(phase) * scale;
        dst[n] = clamp_to_s16(y);
    }

    Xil_DCacheFlushRange((UINTPTR)dst, (u32)DAC_TOTAL_BYTES);
}

static void generate_dac_chirp(double f0_mhz, double f1_mhz)
{
    s16 *dst = (s16 *)(UINTPTR)DAC_DDR_BASE_ADDR;
    double f0 = f0_mhz * 1.0e6;
    double f1 = f1_mhz * 1.0e6;
    double T = ((double)DAC_TOTAL_SAMPLES) / FS_DAC_HZ;
    double k = (f1 - f0) / T;
    double scale = dbfs_to_scale(DEFAULT_AMPL_DBFS);
    u32 n;

    prepare_dac_ddr();

    for (n = 0U; n < DAC_TOTAL_SAMPLES; n++) {
        double t = ((double)n) / FS_DAC_HZ;
        double phase = 2.0 * PI_CONST * ((f0 * t) + (0.5 * k * t * t));
        double y = cos(phase) * scale;
        dst[n] = clamp_to_s16(y);
    }

    Xil_DCacheFlushRange((UINTPTR)dst, (u32)DAC_TOTAL_BYTES);
}

static void generate_dac_pulse(double freq_mhz,
                               double pulse_width_us,
                               double pri_us)
{
    s16 *dst = (s16 *)(UINTPTR)DAC_DDR_BASE_ADDR;
    double freq_hz = freq_mhz * 1.0e6;
    double scale = dbfs_to_scale(DEFAULT_AMPL_DBFS);
    u32 pulse_width_samples =
        (u32)((pulse_width_us * 1.0e-6) * FS_DAC_HZ);
    u32 pri_samples = (u32)((pri_us * 1.0e-6) * FS_DAC_HZ);
    u32 n;

    prepare_dac_ddr();

    if (pulse_width_samples < 1U) {
        pulse_width_samples = 1U;
    }

    if (pri_samples < pulse_width_samples) {
        pri_samples = pulse_width_samples;
    }

    for (n = 0U; n < DAC_TOTAL_SAMPLES; n++) {
        u32 sample_in_pri = n % pri_samples;

        if (sample_in_pri < pulse_width_samples) {
            double phase = 2.0 * PI_CONST * freq_hz * ((double)n) / FS_DAC_HZ;
            double y = cos(phase) * scale;
            dst[n] = clamp_to_s16(y);
        } else {
            dst[n] = 0;
        }
    }

    Xil_DCacheFlushRange((UINTPTR)dst, (u32)DAC_TOTAL_BYTES);
}

/* Service timers and Ethernet RX once. */
static void service_lwip_once(void)
{
    if (!eth_ready || (echo_netif == NULL)) {
        return;
    }

    if (TcpFastTmrFlag) {
        tcp_fasttmr();
        TcpFastTmrFlag = 0;
    }

    if (TcpSlowTmrFlag) {
        tcp_slowtmr();
        TcpSlowTmrFlag = 0;
    }

    xemacif_input(echo_netif);
}

/* Copy data into lwIP-owned buffers. This is intentionally not zero-copy: a
 * subsequent ADC capture may safely overwrite PL DDR as soon as this routine
 * returns. Larger tcp_write() calls reduce raw-API overhead; lwIP segments the
 * data according to TCP_MSS internally. */
static err_t tcp_write_all_copy(struct tcp_pcb *tpcb,
                                const u8 *src,
                                u32 length)
{
    u32 sent = 0U;
    u32 chunks_since_output = 0U;

    while (sent < length) {
        u16 snd_space = tcp_sndbuf(tpcb);
        u32 remaining = length - sent;
        u32 chunk;
        u8 flags;
        err_t err;

        if (snd_space == 0U) {
            (void)tcp_output(tpcb);
            service_lwip_once();
            continue;
        }

        chunk = remaining;
        if (chunk > (u32)snd_space) {
            chunk = (u32)snd_space;
        }
        if (chunk > ETH_TX_CHUNK_MAX) {
            chunk = ETH_TX_CHUNK_MAX;
        }

        flags = TCP_WRITE_FLAG_COPY;
        if ((sent + chunk) < length) {
            flags |= TCP_WRITE_FLAG_MORE;
        }

        err = tcp_write(tpcb, src + sent, (u16)chunk, flags);
        if (err == ERR_OK) {
            sent += chunk;
            chunks_since_output++;

            if ((chunks_since_output >= ETH_TX_OUTPUT_CHUNKS) ||
                (tcp_sndbuf(tpcb) < TCP_MSS)) {
                (void)tcp_output(tpcb);
                chunks_since_output = 0U;
            }
        } else if (err == ERR_MEM) {
            /* Send queued data and process ACKs until lwIP frees space. */
            (void)tcp_output(tpcb);
            service_lwip_once();
        } else {
            return err;
        }
    }

    return tcp_output(tpcb);
}

static err_t send_response(struct tcp_pcb *tpcb,
                           u32 status,
                           UINTPTR payload_base,
                           u32 payload_len)
{
    u8 header[ETH_DDR_HEADER_BYTES];
    err_t err;

    memset(header, 0, sizeof(header));
    header[0] = 'R';
    header[1] = 'F';
    header[2] = 'S';
    header[3] = '1';
    put_u32_le(&header[4], status);
    put_u32_le(&header[8], payload_len);
    put_u32_le(&header[12], 0U);

    transfer_active = 1;

    err = tcp_write_all_copy(tpcb, header, ETH_DDR_HEADER_BYTES);
    if (err != ERR_OK) {
        transfer_active = 0;
        xil_printf("TCP: header send failed: %d\r\n", err);
        return err;
    }

    if ((status == ETH_STATUS_OK) && (payload_len != 0U)) {
        err = tcp_write_all_copy(tpcb,
                                 (const u8 *)(UINTPTR)payload_base,
                                 payload_len);
        if (err != ERR_OK) {
            transfer_active = 0;
            xil_printf("TCP: payload send failed: %d\r\n", err);
            return err;
        }
    }

    /* COPY means PL DDR is no longer referenced by lwIP after the bytes have
     * been queued, even if the last TCP packets have not yet been ACKed. */
    transfer_active = 0;

#if ETH_DDR_VERBOSE
    xil_printf("TCP: response queued (%u payload bytes)\r\n", payload_len);
#endif

    return ERR_OK;
}

static err_t send_latest_capture(struct tcp_pcb *tpcb)
{
    UINTPTR base = adc_capture_base();
    u32 capture_bytes = adc_capture_bytes();

    if (capture_bytes == 0U) {
        return send_response(tpcb, ETH_STATUS_NO_CAPTURE, 0U, 0U);
    }

    /* The ADC DMA has already completed at this point. Only now make the CPU
     * view of the PL-DDR capture normal non-cacheable. */
    map_ddr_range_noncacheable(base, capture_bytes);

#if ETH_DDR_VERBOSE
    xil_printf("TCP: sending %u capture bytes from ", capture_bytes);
    print_addr(base);
    xil_printf("\r\n");
#endif

    return send_response(tpcb, ETH_STATUS_OK, base, capture_bytes);
}

static int valid_capture_kb(unsigned long kb_ul)
{
    u32 max_kb = adc_capture_capacity() / 1024U;

    if ((kb_ul == 0UL) || (kb_ul > 0xFFFFFFFFUL)) {
        return 0;
    }

    return ((u32)kb_ul <= max_kb);
}

static err_t process_command(struct tcp_pcb *tpcb, const char *cmd)
{
    UINTPTR base = adc_capture_base();
    u32 capture_bytes = adc_capture_bytes();
    unsigned long offset_ul = 0UL;
    unsigned long length_ul = 0UL;
    unsigned long capture_kb_ul = 0UL;

    /* Capture a fresh frame and return it in the same request. */
    if (sscanf(cmd, "CAPTURE_GET %lu", &capture_kb_ul) == 1) {
        if (!valid_capture_kb(capture_kb_ul)) {
            return send_response(tpcb, ETH_STATUS_BAD_RANGE, 0U, 0U);
        }

        if (adc_capture_kb((u32)capture_kb_ul, 0) != XST_SUCCESS) {
            return send_response(tpcb, ETH_STATUS_CAPTURE_FAILED, 0U, 0U);
        }

        return send_latest_capture(tpcb);
    }

    /* Capture only; useful for scripts that want to trigger now and read later. */
    if (sscanf(cmd, "CAPTURE %lu", &capture_kb_ul) == 1) {
        if (!valid_capture_kb(capture_kb_ul)) {
            return send_response(tpcb, ETH_STATUS_BAD_RANGE, 0U, 0U);
        }

        if (adc_capture_kb((u32)capture_kb_ul, 0) != XST_SUCCESS) {
            return send_response(tpcb, ETH_STATUS_CAPTURE_FAILED, 0U, 0U);
        }

        return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
    }

    if ((strcmp(cmd, "GET_CAPTURE") == 0) || (strcmp(cmd, "GET") == 0)) {
        return send_latest_capture(tpcb);
    }

    /* Optional bounded sub-range read within the last completed capture:
     *     GET <offset_bytes> <length_bytes>
     */
    if (sscanf(cmd, "GET %lu %lu", &offset_ul, &length_ul) == 2) {
        u32 offset;
        u32 length;

        capture_bytes = adc_capture_bytes();
        if ((capture_bytes == 0U) ||
            (offset_ul > 0xFFFFFFFFUL) ||
            (length_ul > 0xFFFFFFFFUL)) {
            return send_response(tpcb, ETH_STATUS_BAD_RANGE, 0U, 0U);
        }

        offset = (u32)offset_ul;
        length = (u32)length_ul;

        if ((offset > capture_bytes) ||
            (length > (capture_bytes - offset))) {
            return send_response(tpcb, ETH_STATUS_BAD_RANGE, 0U, 0U);
        }

        base = adc_capture_base() + offset;
        map_ddr_range_noncacheable(base, length);

#if ETH_DDR_VERBOSE
        xil_printf("TCP: sending range offset=%u length=%u from ", offset, length);
        print_addr(base);
        xil_printf("\r\n");
#endif

        return send_response(tpcb, ETH_STATUS_OK, base, length);
    }

    /* Start playback using the same registered CLI implementation as
     * "dacPlay <numKB>".  The GUI issues DACPLAY 256 immediately after it
     * generates/clears the DDR waveform. */
    {
        unsigned long play_kb_ul = 0UL;

        if ((sscanf(cmd, "DACPLAY %lu", &play_kb_ul) == 1) ||
            (sscanf(cmd, "dacplay %lu", &play_kb_ul) == 1) ||
            (sscanf(cmd, "dacPlay %lu", &play_kb_ul) == 1)) {
            if ((play_kb_ul == 0UL) || (play_kb_ul > 0xFFFFFFFFUL)) {
                return send_response(tpcb, ETH_STATUS_DACPLAY_FAILED, 0U, 0U);
            }

            if (invoke_registered_cli_u32("dacPlay", (u32)play_kb_ul) != XST_SUCCESS) {
                return send_response(tpcb, ETH_STATUS_DACPLAY_FAILED, 0U, 0U);
            }

            return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
        }
    }

    /* DAC waveform generation commands. Both upper- and lower-case forms are
     * accepted so the RFSoC_COM GUI and the older new_echo.py-style client can
     * use the same server. Successful generation returns the normal RFS1
     * header with status=0 and a zero-length payload. */
    {
        double freq_mhz = 0.0;

        if ((sscanf(cmd, "TONE %lf", &freq_mhz) == 1) ||
            (sscanf(cmd, "tone %lf", &freq_mhz) == 1) ||
            (sscanf(cmd, "YES %lf", &freq_mhz) == 1) ||
            (sscanf(cmd, "yes %lf", &freq_mhz) == 1)) {
            if (freq_mhz <= 0.0) {
                return send_response(tpcb, ETH_STATUS_BAD_WAVEFORM, 0U, 0U);
            }

            generate_dac_tone(freq_mhz);
            return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
        }
    }

    {
        double f0_mhz = 0.0;
        double f1_mhz = 0.0;

        if ((sscanf(cmd, "CHIRP %lf %lf", &f0_mhz, &f1_mhz) == 2) ||
            (sscanf(cmd, "chirp %lf %lf", &f0_mhz, &f1_mhz) == 2)) {
            if ((f0_mhz <= 0.0) || (f1_mhz <= 0.0)) {
                return send_response(tpcb, ETH_STATUS_BAD_WAVEFORM, 0U, 0U);
            }

            generate_dac_chirp(f0_mhz, f1_mhz);
            return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
        }
    }

    {
        double freq_mhz = 0.0;
        double pulse_width_us = 0.0;
        double pri_us = 0.0;

        if ((sscanf(cmd, "PULSE %lf %lf %lf",
                    &freq_mhz, &pulse_width_us, &pri_us) == 3) ||
            (sscanf(cmd, "pulse %lf %lf %lf",
                    &freq_mhz, &pulse_width_us, &pri_us) == 3)) {
            if ((freq_mhz <= 0.0) ||
                (pulse_width_us <= 0.0) ||
                (pri_us <= 0.0)) {
                return send_response(tpcb, ETH_STATUS_BAD_WAVEFORM, 0U, 0U);
            }

            generate_dac_pulse(freq_mhz, pulse_width_us, pri_us);
            return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
        }
    }

    if ((strcmp(cmd, "CLEAR") == 0) || (strcmp(cmd, "clear") == 0)) {
        clear_dac_ddr_buffer();
        return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
    }

    if (strcmp(cmd, "PING") == 0) {
        return send_response(tpcb, ETH_STATUS_OK, 0U, 0U);
    }

    return send_response(tpcb, ETH_STATUS_BAD_COMMAND, 0U, 0U);
}

static err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                           struct pbuf *p, err_t err)
{
    struct pbuf *q;

    (void)arg;

    if (p == NULL) {
        tcp_recv(tpcb, NULL);
        tcp_arg(tpcb, NULL);
        client.pcb = NULL;
        client.cmd_len = 0U;
        transfer_active = 0;
        return tcp_close(tpcb);
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    tcp_recved(tpcb, p->tot_len);

    for (q = p; q != NULL; q = q->next) {
        const u8 *src = (const u8 *)q->payload;
        u16 i;

        for (i = 0U; i < q->len; i++) {
            char c = (char)src[i];

            if ((c == '\n') || (c == '\r')) {
                if (client.cmd_len != 0U) {
                    client.cmd[client.cmd_len] = '\0';
                    (void)process_command(tpcb, client.cmd);
                    client.cmd_len = 0U;
                }
            } else if (client.cmd_len < (ETH_DDR_CMD_BYTES - 1U)) {
                client.cmd[client.cmd_len++] = c;
            } else {
                client.cmd_len = 0U;
                (void)process_command(tpcb, "BAD_COMMAND");
                break;
            }
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static void error_callback(void *arg, err_t err)
{
    (void)arg;
    xil_printf("TCP: connection error: %d\r\n", err);
    reset_client_state();
}

static err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if ((err != ERR_OK) || (newpcb == NULL)) {
        return ERR_VAL;
    }

    if (client.pcb != NULL) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    client.pcb = newpcb;
    client.cmd_len = 0U;

    tcp_arg(newpcb, &client);
    tcp_recv(newpcb, recv_callback);
    tcp_err(newpcb, error_callback);

#if ETH_DDR_VERBOSE
    xil_printf("TCP: client connected\r\n");
#endif

    return ERR_OK;
}

static int start_capture_server(void)
{
    struct tcp_pcb *pcb;
    err_t err;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) {
        xil_printf("TCP: tcp_new failed\r\n");
        return XST_FAILURE;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, ETH_DDR_TCP_PORT);
    if (err != ERR_OK) {
        xil_printf("TCP: bind port %u failed: %d\r\n", ETH_DDR_TCP_PORT, err);
        tcp_close(pcb);
        return XST_FAILURE;
    }

    listen_pcb = tcp_listen(pcb);
    if (listen_pcb == NULL) {
        xil_printf("TCP: tcp_listen failed\r\n");
        tcp_close(pcb);
        return XST_FAILURE;
    }

    tcp_accept(listen_pcb, accept_callback);
    return XST_SUCCESS;
}

int eth_ddr_init(void)
{
    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gw;
    unsigned char mac_ethernet_address[] =
        {0x00, 0x0a, 0x35, 0x00, 0x01, 0x02};

    if (eth_ready) {
        return XST_SUCCESS;
    }

    reset_client_state();
    echo_netif = &server_netif;

    init_platform();

    IP4_ADDR(&ipaddr,  192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    lwip_init();

    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gw,
                   mac_ethernet_address, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Ethernet: xemac_add failed\r\n");
        return XST_FAILURE;
    }

    netif_set_default(echo_netif);
    netif_set_up(echo_netif);
    platform_enable_interrupts();

    if (start_capture_server() != XST_SUCCESS) {
        return XST_FAILURE;
    }

    eth_ready = 1;

    xil_printf("Ethernet capture server ready\r\n");
    xil_printf("  IP   : 192.168.1.10\r\n");
    xil_printf("  Port : %u\r\n", ETH_DDR_TCP_PORT);
    xil_printf("  GEM  : 0x%08x\r\n", (u32)PLATFORM_EMAC_BASEADDR);
    xil_printf("  TCP  : MSS=%u SND_BUF=%u WND=%u\r\n",
               (u32)TCP_MSS, (u32)TCP_SND_BUF, (u32)TCP_WND);
    xil_printf("  Cmds : GET_CAPTURE, CAPTURE <kB>, CAPTURE_GET <kB>\r\n");
    xil_printf("         TONE <MHz>, CHIRP <f0> <f1>, PULSE <MHz> <PW_us> <PRI_us>, CLEAR\r\n");
    xil_printf("         DACPLAY <kB>  (same path as CLI dacPlay <kB>)\r\n");

    return XST_SUCCESS;
}

void eth_ddr_poll(void)
{
    service_lwip_once();
}

int eth_ddr_is_ready(void)
{
    return eth_ready;
}

int eth_ddr_transfer_active(void)
{
    return transfer_active;
}
