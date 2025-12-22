/*
 * Copyright 2024, TII
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Orin AGX fan control - set fan to max speed via BPMP IPC
 */

#include <autoconf.h>
#include <elfloader.h>
#include <printf.h>
#include <types.h>

#define SRAM_BASE           0x40000000UL
#define BPMP_TX_SHMEM       (SRAM_BASE + 0x70000)
#define BPMP_RX_SHMEM       (SRAM_BASE + 0x71000)

#define HSP_TOP0_BASE       0x03c00000UL
#define HSP_INT_DIMENSIONING 0x380
#define HSP_DB_TRIGGER      0x0

#define PWM3_BASE           0x032a0000UL

#define IVC_ALIGN           64
#define MSG_FRAME_SIZE      128

struct ivc_header {
    volatile uint32_t tx_count;
    volatile uint32_t tx_state;
    uint8_t tx_pad[IVC_ALIGN - 8];
    volatile uint32_t rx_count;
    uint8_t rx_pad[IVC_ALIGN - 4];
};

#define IVC_STATE_ESTABLISHED  0

#define MRQ_RESET           20
#define MRQ_CLK             22

#define CMD_RESET_DEASSERT  2
#define CMD_CLK_ENABLE      7

#define TEGRA234_CLK_PWM3   107
#define TEGRA234_RESET_PWM3 70

#define BPMP_MAIL_DO_ACK    (1 << 0)
#define BPMP_MAIL_RING_DB   (1 << 1)

struct mrq_request {
    uint32_t mrq;
    uint32_t flags;
} __attribute__((packed));

struct mrq_response {
    int32_t err;
    uint32_t flags;
} __attribute__((packed));

static inline void writel(uint32_t val, volatile void *addr)
{
    *(volatile uint32_t *)addr = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t readl(volatile void *addr)
{
    uint32_t val = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return val;
}

static inline void cache_clean(volatile void *addr, size_t size)
{
    uintptr_t p = (uintptr_t)addr & ~63UL;
    uintptr_t end = (uintptr_t)addr + size;
    for (; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static volatile void *get_bpmp_doorbell(void)
{
    uint32_t dim = readl((void *)(HSP_TOP0_BASE + HSP_INT_DIMENSIONING));
    unsigned num_sm = (dim >> 0) & 0xf;
    unsigned num_ss = (dim >> 4) & 0xf;
    unsigned num_as = (dim >> 8) & 0xf;
    unsigned db_offset = (1 + (num_sm / 2) + num_ss + num_as) * 0x10000;
    db_offset += 3 * 0x100;
    return (volatile void *)(HSP_TOP0_BASE + db_offset);
}

static int bpmp_send_request(uint32_t mrq, void *payload_data, int payload_size)
{
    volatile struct ivc_header *tx_hdr = (void *)BPMP_TX_SHMEM;
    volatile struct ivc_header *rx_hdr = (void *)BPMP_RX_SHMEM;
    volatile void *tx_frame = (void *)(BPMP_TX_SHMEM + sizeof(struct ivc_header));
    volatile void *rx_frame = (void *)(BPMP_RX_SHMEM + sizeof(struct ivc_header));
    volatile void *doorbell = get_bpmp_doorbell();

    cache_clean((void *)tx_hdr, sizeof(*tx_hdr));
    cache_clean((void *)rx_hdr, sizeof(*rx_hdr));

    if (tx_hdr->tx_state != IVC_STATE_ESTABLISHED) {
        return -1;
    }

    volatile struct mrq_request *req_hdr = tx_frame;
    volatile uint8_t *payload = (void *)((uintptr_t)tx_frame + sizeof(struct mrq_request));

    req_hdr->mrq = mrq;
    req_hdr->flags = BPMP_MAIL_DO_ACK | BPMP_MAIL_RING_DB;

    uint8_t *src = (uint8_t *)payload_data;
    for (int i = 0; i < payload_size; i++) {
        payload[i] = src[i];
    }

    cache_clean((void *)tx_frame, MSG_FRAME_SIZE);

    uint32_t old_rx_count = rx_hdr->tx_count;
    tx_hdr->tx_count++;
    cache_clean((void *)&tx_hdr->tx_count, 64);

    writel(1, doorbell + HSP_DB_TRIGGER);

    for (int i = 0; i < 1000000; i++) {
        cache_clean((void *)&rx_hdr->tx_count, 64);
        if (rx_hdr->tx_count != old_rx_count) break;
        for (volatile int j = 0; j < 100; j++);
    }

    if (rx_hdr->tx_count == old_rx_count) {
        return -2;
    }

    cache_clean((void *)rx_frame, MSG_FRAME_SIZE);
    volatile struct mrq_response *resp = rx_frame;

    rx_hdr->rx_count++;
    cache_clean((void *)&rx_hdr->rx_count, 64);

    return resp->err;
}

void orinagx_fan_init(void)
{
    int ret;
    uint32_t req[2];

    printf("Orin AGX: enabling fan at max speed...\n");

    /* Deassert PWM3 reset */
    req[0] = CMD_RESET_DEASSERT;
    req[1] = TEGRA234_RESET_PWM3;
    ret = bpmp_send_request(MRQ_RESET, req, 8);
    printf("BPMP: reset_deassert=%d\n", ret);

    /* Enable clock */
    req[0] = (CMD_CLK_ENABLE << 24) | TEGRA234_CLK_PWM3;
    ret = bpmp_send_request(MRQ_CLK, req, 4);
    printf("BPMP: clk_enable=%d\n", ret);

    /* Write PWM register - max duty */
    volatile uint32_t *pwm3 = (volatile uint32_t *)PWM3_BASE;
    printf("PWM3: current=0x%08x\n", *pwm3);
    *pwm3 = 0x80ff0000;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PWM3: wrote 0x80ff0000, readback=0x%08x\n", *pwm3);

    printf("Orin AGX: fan init complete\n");
}
