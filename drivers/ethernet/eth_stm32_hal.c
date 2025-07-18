/*
 * Copyright (c) 2017 Erwin Rol <erwin@erwinrol.com>
 * Copyright (c) 2020 Alexander Kozhinov <ak.alexander.kozhinov@gmail.com>
 * Copyright (c) 2021 Carbon Robotics
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_ethernet

#define LOG_MODULE_NAME eth_stm32_hal
#define LOG_LEVEL CONFIG_ETHERNET_LOG_LEVEL

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/crc.h>
#include <errno.h>
#include <stdbool.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/phy.h>
#include <ethernet/eth_stats.h>
#include <soc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/net/lldp.h>
#include <zephyr/drivers/hwinfo.h>

#if defined(CONFIG_NET_DSA_DEPRECATED)
#include <zephyr/net/dsa.h>
#endif

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
#include <zephyr/drivers/ptp_clock.h>
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

#include "eth.h"
#include "eth_stm32_hal_priv.h"

#if DT_INST_PROP(0, zephyr_random_mac_address)
#define ETH_STM32_RANDOM_MAC
#endif

#if defined(CONFIG_ETH_STM32_HAL_USE_DTCM_FOR_DMA_BUFFER) && \
	    !DT_NODE_HAS_STATUS_OKAY(DT_CHOSEN(zephyr_dtcm))
#error DTCM for DMA buffer is activated but zephyr,dtcm is not present in dts
#endif

static const struct device *eth_stm32_phy_dev = DEVICE_DT_GET(DT_INST_PHANDLE(0, phy_handle));

#define ETH_STM32_AUTO_NEGOTIATION_ENABLE                                                          \
	UTIL_NOT(DT_NODE_HAS_PROP(DT_INST_PHANDLE(0, phy_handle), fixed_link))

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
#define IS_ETH_DMATXDESC_OWN(dma_tx_desc)	(dma_tx_desc->DESC3 & \
							ETH_DMATXNDESCRF_OWN)

#define ETH_RXBUFNB	ETH_RX_DESC_CNT
#define ETH_TXBUFNB	ETH_TX_DESC_CNT

/* Only one tx_buffer is sufficient to pass only 1 dma_buffer */
#define ETH_TXBUF_DEF_NB	1U
#else

#define IS_ETH_DMATXDESC_OWN(dma_tx_desc)	(dma_tx_desc->Status & \
							ETH_DMATXDESC_OWN)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
#define ETH_MII_MODE	HAL_ETH_MII_MODE
#define ETH_RMII_MODE	HAL_ETH_RMII_MODE
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
#define ETH_GMII_MODE	HAL_ETH_GMII_MODE
#define ETH_RGMII_MODE	HAL_ETH_RGMII_MODE
#endif

#else
#define ETH_MII_MODE	ETH_MEDIA_INTERFACE_MII
#define ETH_RMII_MODE	ETH_MEDIA_INTERFACE_RMII
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
#define STM32_ETH_PHY_MODE(inst) \
	((DT_INST_ENUM_HAS_VALUE(inst, phy_connection_type, rgmii) ? ETH_RGMII_MODE : \
	 (DT_INST_ENUM_HAS_VALUE(inst, phy_connection_type, gmii) ? ETH_GMII_MODE : \
	 (DT_INST_ENUM_HAS_VALUE(inst, phy_connection_type, mii) ? ETH_MII_MODE : \
		 ETH_RMII_MODE))))
#else
#define STM32_ETH_PHY_MODE(inst) \
	(DT_INST_ENUM_HAS_VALUE(inst, phy_connection_type, mii) ? \
		ETH_MII_MODE : ETH_RMII_MODE)
#endif

#define ETH_DMA_TX_TIMEOUT_MS	20U  /* transmit timeout in milliseconds */

#if defined(CONFIG_ETH_STM32_HAL_USE_DTCM_FOR_DMA_BUFFER) && \
	    DT_NODE_HAS_STATUS_OKAY(DT_CHOSEN(zephyr_dtcm))
#define __eth_stm32_desc __dtcm_noinit_section
#define __eth_stm32_buf  __dtcm_noinit_section
#elif defined(CONFIG_SOC_SERIES_STM32H7X)
#define __eth_stm32_desc __attribute__((section(".eth_stm32_desc")))
#define __eth_stm32_buf  __attribute__((section(".eth_stm32_buf")))
#elif defined(CONFIG_NOCACHE_MEMORY)
#define __eth_stm32_desc __nocache __aligned(4)
#define __eth_stm32_buf  __nocache __aligned(4)
#else
#define __eth_stm32_desc __aligned(4)
#define __eth_stm32_buf  __aligned(4)
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
static ETH_DMADescTypeDef
	dma_rx_desc_tab[ETH_DMA_RX_CH_CNT][ETH_RXBUFNB] ALIGN_32BYTES(__eth_stm32_desc);
static ETH_DMADescTypeDef
	dma_tx_desc_tab[ETH_DMA_TX_CH_CNT][ETH_TXBUFNB] ALIGN_32BYTES(__eth_stm32_desc);
#else
static ETH_DMADescTypeDef dma_rx_desc_tab[ETH_RXBUFNB] __eth_stm32_desc;
static ETH_DMADescTypeDef dma_tx_desc_tab[ETH_TXBUFNB] __eth_stm32_desc;
#endif

static uint8_t dma_rx_buffer[ETH_RXBUFNB][ETH_STM32_RX_BUF_SIZE] __eth_stm32_buf;
static uint8_t dma_tx_buffer[ETH_TXBUFNB][ETH_STM32_TX_BUF_SIZE] __eth_stm32_buf;

#if defined(CONFIG_ETH_STM32_HAL_API_V2)

BUILD_ASSERT(ETH_STM32_RX_BUF_SIZE % 4 == 0, "Rx buffer size must be a multiple of 4");

struct eth_stm32_rx_buffer_header {
	struct eth_stm32_rx_buffer_header *next;
	uint16_t size;
	bool used;
};

struct eth_stm32_tx_buffer_header {
	ETH_BufferTypeDef tx_buff;
	bool used;
};

struct eth_stm32_tx_context {
	struct net_pkt *pkt;
	uint16_t first_tx_buffer_index;
	bool used;
};

static struct eth_stm32_rx_buffer_header dma_rx_buffer_header[ETH_RXBUFNB];
static struct eth_stm32_tx_buffer_header dma_tx_buffer_header[ETH_TXBUFNB];
static struct eth_stm32_tx_context dma_tx_context[ETH_TX_DESC_CNT];

void HAL_ETH_RxAllocateCallback(uint8_t **buf)
{
	for (size_t i = 0; i < ETH_RXBUFNB; ++i) {
		if (!dma_rx_buffer_header[i].used) {
			dma_rx_buffer_header[i].next = NULL;
			dma_rx_buffer_header[i].size = 0;
			dma_rx_buffer_header[i].used = true;
			*buf = dma_rx_buffer[i];
			return;
		}
	}
	*buf = NULL;
}

/* Pointer to an array of ETH_STM32_RX_BUF_SIZE uint8_t's */
typedef uint8_t (*RxBufferPtr)[ETH_STM32_RX_BUF_SIZE];

/* called by HAL_ETH_ReadData() */
void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
	/* buff points to the begin on one of the rx buffers,
	 * so we can compute the index of the given buffer
	 */
	size_t index = (RxBufferPtr)buff - &dma_rx_buffer[0];
	struct eth_stm32_rx_buffer_header *header = &dma_rx_buffer_header[index];

	__ASSERT_NO_MSG(index < ETH_RXBUFNB);

	header->size = Length;

	if (!*pStart) {
		/* first packet, set head pointer of linked list */
		*pStart = header;
		*pEnd = header;
	} else {
		__ASSERT_NO_MSG(*pEnd != NULL);
		/* not the first packet, add to list and adjust tail pointer */
		((struct eth_stm32_rx_buffer_header *)*pEnd)->next = header;
		*pEnd = header;
	}
}

/* Called by HAL_ETH_ReleaseTxPacket */
void HAL_ETH_TxFreeCallback(uint32_t *buff)
{
	__ASSERT_NO_MSG(buff != NULL);

	/* buff is the user context in tx_config.pData */
	struct eth_stm32_tx_context *ctx = (struct eth_stm32_tx_context *)buff;
	struct eth_stm32_tx_buffer_header *buffer_header =
		&dma_tx_buffer_header[ctx->first_tx_buffer_index];

	while (buffer_header != NULL) {
		buffer_header->used = false;
		if (buffer_header->tx_buff.next != NULL) {
			buffer_header = CONTAINER_OF(buffer_header->tx_buff.next,
				struct eth_stm32_tx_buffer_header, tx_buff);
		} else {
			buffer_header = NULL;
		}
	}
	ctx->used = false;
}

/* allocate a tx buffer and mark it as used */
static inline uint16_t allocate_tx_buffer(void)
{
	for (;;) {
		for (uint16_t index = 0; index < ETH_TXBUFNB; index++) {
			if (!dma_tx_buffer_header[index].used) {
				dma_tx_buffer_header[index].used = true;
				return index;
			}
		}
		k_yield();
	}
}

/* allocate a tx context and mark it as used, the first tx buffer is also allocated */
static inline struct eth_stm32_tx_context *allocate_tx_context(struct net_pkt *pkt)
{
	for (;;) {
		for (uint16_t index = 0; index < ETH_TX_DESC_CNT; index++) {
			if (!dma_tx_context[index].used) {
				dma_tx_context[index].used = true;
				dma_tx_context[index].pkt = pkt;
				dma_tx_context[index].first_tx_buffer_index = allocate_tx_buffer();
				return &dma_tx_context[index];
			}
		}
		k_yield();
	}
}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
static ETH_TxPacketConfig tx_config;
#endif

static inline void setup_mac_filter(ETH_HandleTypeDef *heth)
{
	__ASSERT_NO_MSG(heth != NULL);

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	ETH_MACFilterConfigTypeDef MACFilterConf;

	HAL_ETH_GetMACFilterConfig(heth, &MACFilterConf);

	MACFilterConf.HashMulticast =
		IS_ENABLED(CONFIG_ETH_STM32_MULTICAST_FILTER) ? ENABLE : DISABLE;
	MACFilterConf.PassAllMulticast =
		IS_ENABLED(CONFIG_ETH_STM32_MULTICAST_FILTER) ? DISABLE : ENABLE;
	MACFilterConf.HachOrPerfectFilter = DISABLE;

	HAL_ETH_SetMACFilterConfig(heth, &MACFilterConf);

	k_sleep(K_MSEC(1));
#else /* CONFIG_ETH_STM32_HAL_API_V2 */
	uint32_t tmp = heth->Instance->MACFFR;

	/* clear all multicast filter bits, resulting in perfect filtering */
	tmp &= ~(ETH_MULTICASTFRAMESFILTER_PERFECTHASHTABLE |
		 ETH_MULTICASTFRAMESFILTER_HASHTABLE |
		 ETH_MULTICASTFRAMESFILTER_PERFECT |
		 ETH_MULTICASTFRAMESFILTER_NONE);

	if (IS_ENABLED(CONFIG_ETH_STM32_MULTICAST_FILTER)) {
		/* enable multicast hash receive filter */
		tmp |= ETH_MULTICASTFRAMESFILTER_HASHTABLE;
	} else {
		/* enable receiving all multicast frames */
		tmp |= ETH_MULTICASTFRAMESFILTER_NONE;
	}

	heth->Instance->MACFFR = tmp;

	/* Wait until the write operation will be taken into account:
	 * at least four TX_CLK/RX_CLK clock cycles
	 */
	tmp = heth->Instance->MACFFR;
	k_sleep(K_MSEC(1));
	heth->Instance->MACFFR = tmp;
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */
}

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
static bool eth_is_ptp_pkt(struct net_if *iface, struct net_pkt *pkt)
{
	if (ntohs(NET_ETH_HDR(pkt)->type) != NET_ETH_PTYPE_PTP) {
		return false;
	}

	net_pkt_set_priority(pkt, NET_PRIORITY_CA);

	return true;
}

void HAL_ETH_TxPtpCallback(uint32_t *buff, ETH_TimeStampTypeDef *timestamp)
{
	struct eth_stm32_tx_context *ctx = (struct eth_stm32_tx_context *)buff;

	ctx->pkt->timestamp.second = timestamp->TimeStampHigh;
	ctx->pkt->timestamp.nanosecond = timestamp->TimeStampLow;

	net_if_add_tx_timestamp(ctx->pkt);
}
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

static int eth_tx(const struct device *dev, struct net_pkt *pkt)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	int res;
	size_t total_len;
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	size_t remaining_read;
	struct eth_stm32_tx_context *ctx = NULL;
	struct eth_stm32_tx_buffer_header *buf_header = NULL;
#else
	uint8_t *dma_buffer;
	__IO ETH_DMADescTypeDef *dma_tx_desc;
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */
	HAL_StatusTypeDef hal_ret = HAL_OK;
#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	bool timestamped_frame;
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

	__ASSERT_NO_MSG(pkt != NULL);
	__ASSERT_NO_MSG(pkt->frags != NULL);

	total_len = net_pkt_get_len(pkt);
	if (total_len > (ETH_STM32_TX_BUF_SIZE * ETH_TXBUFNB)) {
		LOG_ERR("PKT too big");
		return -EIO;
	}

	k_mutex_lock(&dev_data->tx_mutex, K_FOREVER);

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	ctx = allocate_tx_context(pkt);
	buf_header = &dma_tx_buffer_header[ctx->first_tx_buffer_index];
#else
	dma_tx_desc = heth->TxDesc;
	while (IS_ETH_DMATXDESC_OWN(dma_tx_desc) != (uint32_t)RESET) {
		k_yield();
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	timestamped_frame = eth_is_ptp_pkt(net_pkt_iface(pkt), pkt) ||
			    net_pkt_is_tx_timestamping(pkt);
	if (timestamped_frame) {
		/* Enable transmit timestamp */
		HAL_ETH_PTP_InsertTxTimestamp(heth);
	}
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	remaining_read = total_len;
	/* fill and allocate buffer until remaining data fits in one buffer */
	while (remaining_read > ETH_STM32_TX_BUF_SIZE) {
		if (net_pkt_read(pkt, buf_header->tx_buff.buffer, ETH_STM32_TX_BUF_SIZE)) {
			res = -ENOBUFS;
			goto error;
		}
		const uint16_t next_buffer_id = allocate_tx_buffer();

		buf_header->tx_buff.len = ETH_STM32_TX_BUF_SIZE;
		/* append new buffer to the linked list */
		buf_header->tx_buff.next = &dma_tx_buffer_header[next_buffer_id].tx_buff;
		/* and adjust tail pointer */
		buf_header = &dma_tx_buffer_header[next_buffer_id];
		remaining_read -= ETH_STM32_TX_BUF_SIZE;
	}
	if (net_pkt_read(pkt, buf_header->tx_buff.buffer, remaining_read)) {
		res = -ENOBUFS;
		goto error;
	}
	buf_header->tx_buff.len = remaining_read;
	buf_header->tx_buff.next = NULL;

#else
	dma_buffer = (uint8_t *)(dma_tx_desc->Buffer1Addr);

	if (net_pkt_read(pkt, dma_buffer, total_len)) {
		res = -ENOBUFS;
		goto error;
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	tx_config.Length = total_len;
	tx_config.pData = ctx;
	tx_config.TxBuffer = &dma_tx_buffer_header[ctx->first_tx_buffer_index].tx_buff;

	/* Reset TX complete interrupt semaphore before TX request*/
	k_sem_reset(&dev_data->tx_int_sem);

	/* tx_buffer is allocated on function stack, we need */
	/* to wait for the transfer to complete */
	/* So it is not freed before the interrupt happens */
	hal_ret = HAL_ETH_Transmit_IT(heth, &tx_config);

	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Transmit: failed!");
		res = -EIO;
		goto error;
	}

	/* the tx context is now owned by the HAL */
	ctx = NULL;

	/* Wait for end of TX buffer transmission */
	/* If the semaphore timeout breaks, it means */
	/* an error occurred or IT was not fired */
	if (k_sem_take(&dev_data->tx_int_sem,
			K_MSEC(ETH_DMA_TX_TIMEOUT_MS)) != 0) {

		LOG_ERR("HAL_ETH_TransmitIT tx_int_sem take timeout");
		res = -EIO;

		/* Check for errors */
		/* Ethernet device was put in error state */
		/* Error state is unrecoverable ? */
		if (HAL_ETH_GetState(heth) == HAL_ETH_STATE_ERROR) {
			LOG_ERR("%s: ETH in error state: errorcode:%x",
				__func__,
				HAL_ETH_GetError(heth));
			/* TODO recover from error state by restarting eth */
		}

		/* Check for DMA errors */
		if (HAL_ETH_GetDMAError(heth)) {
			LOG_ERR("%s: ETH DMA error: dmaerror:%x",
				__func__,
				HAL_ETH_GetDMAError(heth));
			/* DMA fatal bus errors are putting in error state*/
			/* TODO recover from this */
		}

		/* Check for MAC errors */
		if (HAL_ETH_GetMACError(heth)) {
			LOG_ERR("%s: ETH MAC error: macerror:%x",
				__func__,
				HAL_ETH_GetMACError(heth));
			/* MAC errors are putting in error state*/
			/* TODO recover from this */
		}

		goto error;
	}

#else
	hal_ret = HAL_ETH_TransmitFrame(heth, total_len);

	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Transmit: failed!");
		res = -EIO;
		goto error;
	}

	/* When Transmit Underflow flag is set, clear it and issue a
	 * Transmit Poll Demand to resume transmission.
	 */
	if ((heth->Instance->DMASR & ETH_DMASR_TUS) != (uint32_t)RESET) {
		/* Clear TUS ETHERNET DMA flag */
		heth->Instance->DMASR = ETH_DMASR_TUS;
		/* Resume DMA transmission*/
		heth->Instance->DMATPDR = 0;
		res = -EIO;
		goto error;
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

	res = 0;
error:

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	if (!ctx) {
		/* The HAL owns the tx context */
		HAL_ETH_ReleaseTxPacket(heth);
	} else {
		/* We need to release the tx context and its buffers */
		HAL_ETH_TxFreeCallback((uint32_t *)ctx);
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

	k_mutex_unlock(&dev_data->tx_mutex);

	return res;
}

static struct net_if *get_iface(struct eth_stm32_hal_dev_data *ctx)
{
	return ctx->iface;
}

static struct net_pkt *eth_rx(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	struct net_pkt *pkt;
	size_t total_len = 0;
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	void *appbuf = NULL;
	struct eth_stm32_rx_buffer_header *rx_header;
#else
	__IO ETH_DMADescTypeDef *dma_rx_desc;
	uint8_t *dma_buffer;
	HAL_StatusTypeDef hal_ret = HAL_OK;
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */
#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	struct net_ptp_time timestamp;
	ETH_TimeStampTypeDef ts_registers;
	/* Default to invalid value. */
	timestamp.second = UINT64_MAX;
	timestamp.nanosecond = UINT32_MAX;
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	if (HAL_ETH_ReadData(heth, &appbuf) != HAL_OK) {
		/* no frame available */
		return NULL;
	}

	/* computing total length */
	for (rx_header = (struct eth_stm32_rx_buffer_header *)appbuf;
			rx_header; rx_header = rx_header->next) {
		total_len += rx_header->size;
	}
#else
	hal_ret = HAL_ETH_GetReceivedFrame_IT(heth);
	if (hal_ret != HAL_OK) {
		/* no frame available */
		return NULL;
	}

	total_len = heth->RxFrameInfos.length;
	dma_buffer = (uint8_t *)heth->RxFrameInfos.buffer;
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	if (HAL_ETH_PTP_GetRxTimestamp(heth, &ts_registers) == HAL_OK) {
		timestamp.second = ts_registers.TimeStampHigh;
		timestamp.nanosecond = ts_registers.TimeStampLow;
	}
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

	pkt = net_pkt_rx_alloc_with_buffer(get_iface(dev_data),
					   total_len, AF_UNSPEC, 0, K_MSEC(100));
	if (!pkt) {
		LOG_ERR("Failed to obtain RX buffer");
		goto release_desc;
	}

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	for (rx_header = (struct eth_stm32_rx_buffer_header *)appbuf;
			rx_header; rx_header = rx_header->next) {
		const size_t index = rx_header - &dma_rx_buffer_header[0];

		__ASSERT_NO_MSG(index < ETH_RXBUFNB);
		if (net_pkt_write(pkt, dma_rx_buffer[index], rx_header->size)) {
			LOG_ERR("Failed to append RX buffer to context buffer");
			net_pkt_unref(pkt);
			pkt = NULL;
			goto release_desc;
		}
	}
#else
	if (net_pkt_write(pkt, dma_buffer, total_len)) {
		LOG_ERR("Failed to append RX buffer to context buffer");
		net_pkt_unref(pkt);
		pkt = NULL;
		goto release_desc;
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

release_desc:
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	for (rx_header = (struct eth_stm32_rx_buffer_header *)appbuf;
			rx_header; rx_header = rx_header->next) {
		rx_header->used = false;
	}
#else
	/* Release descriptors to DMA */
	/* Point to first descriptor */
	dma_rx_desc = heth->RxFrameInfos.FSRxDesc;
	/* Set Own bit in Rx descriptors: gives the buffers back to DMA */
	for (int i = 0; i < heth->RxFrameInfos.SegCount; i++) {
		dma_rx_desc->Status |= ETH_DMARXDESC_OWN;
		dma_rx_desc = (ETH_DMADescTypeDef *)
			(dma_rx_desc->Buffer2NextDescAddr);
	}

	/* Clear Segment_Count */
	heth->RxFrameInfos.SegCount = 0;

	/* When Rx Buffer unavailable flag is set: clear it
	 * and resume reception.
	 */
	if ((heth->Instance->DMASR & ETH_DMASR_RBUS) != (uint32_t)RESET) {
		/* Clear RBUS ETHERNET DMA flag */
		heth->Instance->DMASR = ETH_DMASR_RBUS;
		/* Resume DMA reception */
		heth->Instance->DMARPDR = 0;
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

	if (!pkt) {
		goto out;
	}

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	pkt->timestamp.second = timestamp.second;
	pkt->timestamp.nanosecond = timestamp.nanosecond;
	if (timestamp.second != UINT64_MAX) {
		net_pkt_set_rx_timestamping(pkt, true);
	}
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

out:
	if (!pkt) {
		eth_stats_update_errors_rx(get_iface(dev_data));
	}

	return pkt;
}

static void rx_thread(void *arg1, void *unused1, void *unused2)
{
	const struct device *dev = (const struct device *)arg1;
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	struct net_if *iface;
	struct net_pkt *pkt;
	int res;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	while (1) {
		res = k_sem_take(&dev_data->rx_int_sem, K_FOREVER);
		if (res == 0) {
			/* semaphore taken and receive packets */
			while ((pkt = eth_rx(dev)) != NULL) {
				iface = net_pkt_iface(pkt);
#if defined(CONFIG_NET_DSA_DEPRECATED)
				iface = dsa_net_recv(iface, &pkt);
#endif
				res = net_recv_data(iface, pkt);
				if (res < 0) {
					eth_stats_update_errors_rx(
							net_pkt_iface(pkt));
					LOG_ERR("Failed to enqueue frame "
						"into RX queue: %d", res);
					net_pkt_unref(pkt);
				}
			}
		}
	}
}

static void eth_isr(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;

	HAL_ETH_IRQHandler(heth);
}
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	struct eth_stm32_hal_dev_data *dev_data =
		CONTAINER_OF(heth_handle, struct eth_stm32_hal_dev_data, heth);

	__ASSERT_NO_MSG(dev_data != NULL);

	k_sem_give(&dev_data->tx_int_sem);

}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth)
{
	/* Do not log errors. If errors are reported due to high traffic,
	 * logging errors will only increase traffic issues
	 */
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
	__ASSERT_NO_MSG(heth != NULL);

	uint32_t dma_error;
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	uint32_t mac_error;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
	const uint32_t error_code = HAL_ETH_GetError(heth);

	struct eth_stm32_hal_dev_data *dev_data =
		CONTAINER_OF(heth, struct eth_stm32_hal_dev_data, heth);

	switch (error_code) {
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
	case HAL_ETH_ERROR_DMA_CH0:
	case HAL_ETH_ERROR_DMA_CH1:
#else
	case HAL_ETH_ERROR_DMA:
#endif
		dma_error = HAL_ETH_GetDMAError(heth);

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
		if ((dma_error & ETH_DMA_RX_WATCHDOG_TIMEOUT_FLAG)   ||
			(dma_error & ETH_DMA_RX_PROCESS_STOPPED_FLAG)    ||
			(dma_error & ETH_DMA_RX_BUFFER_UNAVAILABLE_FLAG)) {
			eth_stats_update_errors_rx(dev_data->iface);
		}
		if ((dma_error & ETH_DMA_EARLY_TX_IT_FLAG) ||
			(dma_error & ETH_DMA_TX_PROCESS_STOPPED_FLAG)) {
			eth_stats_update_errors_tx(dev_data->iface);
		}
#else
		if ((dma_error & ETH_DMASR_RWTS) ||
			(dma_error & ETH_DMASR_RPSS) ||
			(dma_error & ETH_DMASR_RBUS)) {
			eth_stats_update_errors_rx(dev_data->iface);
		}
		if ((dma_error & ETH_DMASR_ETS)  ||
			(dma_error & ETH_DMASR_TPSS) ||
			(dma_error & ETH_DMASR_TJTS)) {
			eth_stats_update_errors_tx(dev_data->iface);
		}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
		break;

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	case HAL_ETH_ERROR_MAC:
		mac_error = HAL_ETH_GetMACError(heth);

		if (mac_error & ETH_RECEIVE_WATCHDOG_TIMEOUT) {
			eth_stats_update_errors_rx(dev_data->iface);
		}

		if ((mac_error & ETH_EXECESSIVE_COLLISIONS)  ||
			(mac_error & ETH_LATE_COLLISIONS)        ||
			(mac_error & ETH_EXECESSIVE_DEFERRAL)    ||
			(mac_error & ETH_TRANSMIT_JABBR_TIMEOUT) ||
			(mac_error & ETH_LOSS_OF_CARRIER)        ||
			(mac_error & ETH_NO_CARRIER)) {
			eth_stats_update_errors_tx(dev_data->iface);
		}
		break;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
	}

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	dev_data->stats.error_details.rx_crc_errors = heth->Instance->MMCRCRCEPR;
	dev_data->stats.error_details.rx_align_errors = heth->Instance->MMCRAEPR;
#else
	dev_data->stats.error_details.rx_crc_errors = heth->Instance->MMCRFCECR;
	dev_data->stats.error_details.rx_align_errors = heth->Instance->MMCRFAECR;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

#endif /* CONFIG_NET_STATISTICS_ETHERNET */
}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth_handle)
{
	__ASSERT_NO_MSG(heth_handle != NULL);

	struct eth_stm32_hal_dev_data *dev_data =
		CONTAINER_OF(heth_handle, struct eth_stm32_hal_dev_data, heth);

	__ASSERT_NO_MSG(dev_data != NULL);

	k_sem_give(&dev_data->rx_int_sem);
}

static void generate_mac(uint8_t *mac_addr)
{
#if defined(ETH_STM32_RANDOM_MAC)
	/* "zephyr,random-mac-address" is set, generate a random mac address */
	gen_random_mac(mac_addr, ST_OUI_B0, ST_OUI_B1, ST_OUI_B2);
#else /* Use user defined mac address */
	mac_addr[0] = ST_OUI_B0;
	mac_addr[1] = ST_OUI_B1;
	mac_addr[2] = ST_OUI_B2;
#if NODE_HAS_VALID_MAC_ADDR(DT_DRV_INST(0))
	mac_addr[3] = NODE_MAC_ADDR_OCTET(DT_DRV_INST(0), 3);
	mac_addr[4] = NODE_MAC_ADDR_OCTET(DT_DRV_INST(0), 4);
	mac_addr[5] = NODE_MAC_ADDR_OCTET(DT_DRV_INST(0), 5);
#else
	uint8_t unique_device_ID_12_bytes[12];
	uint32_t result_mac_32_bits;

	/* Nothing defined by the user, use device id */
	hwinfo_get_device_id(unique_device_ID_12_bytes, 12);
	result_mac_32_bits = crc32_ieee((uint8_t *)unique_device_ID_12_bytes, 12);
	memcpy(&mac_addr[3], &result_mac_32_bits, 3);

#endif /* NODE_HAS_VALID_MAC_ADDR(DT_DRV_INST(0))) */
#endif
}

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
/**
 * Configures the RISAF (RIF Security Attribute Framework) for Ethernet on STM32N6.
 * This function sets up the master and slave security attributes for the Ethernet peripheral.
 */

static void RISAF_Config(void)
{
	/* Define and initialize the master configuration structure */
	RIMC_MasterConfig_t RIMC_master = {0};

	/* Enable the clock for the RIFSC (RIF Security Controller) */
	__HAL_RCC_RIFSC_CLK_ENABLE();

	RIMC_master.MasterCID = RIF_CID_1;
	RIMC_master.SecPriv = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;

	/* Configure the master attributes for the Ethernet peripheral (ETH1) */
	HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_ETH1, &RIMC_master);

	/* Set the secure and privileged attributes for the Ethernet peripheral (ETH1) as a slave */
	HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_ETH1,
					      RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
}
#endif

static int eth_initialize(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	const struct eth_stm32_hal_dev_cfg *cfg = dev->config;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	int ret = 0;

	if (!device_is_ready(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE))) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
	/* RISAF Configuration */
	RISAF_Config();
#endif

	/* enable clock */
	ret = clock_control_on(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
		(clock_control_subsys_t)&cfg->pclken);
	ret |= clock_control_on(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
		(clock_control_subsys_t)&cfg->pclken_tx);
	ret |= clock_control_on(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
		(clock_control_subsys_t)&cfg->pclken_rx);
#if DT_INST_CLOCKS_HAS_NAME(0, mac_clk_ptp)
	ret |= clock_control_on(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
		(clock_control_subsys_t)&cfg->pclken_ptp);
#endif

	if (ret) {
		LOG_ERR("Failed to enable ethernet clock");
		return -EIO;
	}

	/* configure pinmux */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("Could not configure ethernet pins");
		return ret;
	}

	generate_mac(dev_data->mac_addr);

	heth->Init.MACAddr = dev_data->mac_addr;

#if defined(CONFIG_ETH_STM32_HAL_API_V1)
	HAL_StatusTypeDef hal_ret = HAL_OK;

	if (!ETH_STM32_AUTO_NEGOTIATION_ENABLE) {
		struct phy_link_state state;

		phy_get_link_state(eth_stm32_phy_dev, &state);

		heth->Init.DuplexMode = PHY_LINK_IS_FULL_DUPLEX(state.speed) ? ETH_MODE_FULLDUPLEX
									     : ETH_MODE_HALFDUPLEX;
		heth->Init.Speed =
			PHY_LINK_IS_SPEED_100M(state.speed) ? ETH_SPEED_100M : ETH_SPEED_10M;
	}

	hal_ret = HAL_ETH_Init(heth);
	if (hal_ret == HAL_TIMEOUT) {
		/* HAL Init time out. This could be linked to */
		/* a recoverable error. Log the issue and continue */
		/* driver initialisation */
		LOG_WRN("HAL_ETH_Init timed out (cable not connected?)");
	} else if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Init failed: %d", hal_ret);
		return -EINVAL;
	}

	/* Initialize semaphores */
	k_mutex_init(&dev_data->tx_mutex);
	k_sem_init(&dev_data->rx_int_sem, 0, K_SEM_MAX_LIMIT);

	HAL_ETH_DMATxDescListInit(heth, dma_tx_desc_tab,
		&dma_tx_buffer[0][0], ETH_TXBUFNB);
	HAL_ETH_DMARxDescListInit(heth, dma_rx_desc_tab,
		&dma_rx_buffer[0][0], ETH_RXBUFNB);

#endif /* !CONFIG_ETH_STM32_HAL_API_V1 */

	LOG_DBG("MAC %02x:%02x:%02x:%02x:%02x:%02x",
		dev_data->mac_addr[0], dev_data->mac_addr[1],
		dev_data->mac_addr[2], dev_data->mac_addr[3],
		dev_data->mac_addr[4], dev_data->mac_addr[5]);

	return 0;
}

#if defined(CONFIG_ETH_STM32_MULTICAST_FILTER)
static void eth_stm32_mcast_filter(const struct device *dev, const struct ethernet_filter *filter)
{
	struct eth_stm32_hal_dev_data *dev_data = (struct eth_stm32_hal_dev_data *)dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	uint32_t crc;
	uint32_t hash_table[2];
	uint32_t hash_index;

	crc = __RBIT(crc32_ieee(filter->mac_address.addr, sizeof(struct net_eth_addr)));
	hash_index = (crc >> 26) & 0x3f;

	__ASSERT_NO_MSG(hash_index < ARRAY_SIZE(dev_data->hash_index_cnt));

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	hash_table[0] = heth->Instance->MACHT0R;
	hash_table[1] = heth->Instance->MACHT1R;
#else
	hash_table[0] = heth->Instance->MACHTLR;
	hash_table[1] = heth->Instance->MACHTHR;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	if (filter->set) {
		dev_data->hash_index_cnt[hash_index]++;
		hash_table[hash_index / 32] |= (1 << (hash_index % 32));
	} else {
		if (dev_data->hash_index_cnt[hash_index] == 0) {
			__ASSERT_NO_MSG(false);
			return;
		}

		dev_data->hash_index_cnt[hash_index]--;
		if (dev_data->hash_index_cnt[hash_index] == 0) {
			hash_table[hash_index / 32] &= ~(1 << (hash_index % 32));
		}
	}

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACHT0R = hash_table[0];
	heth->Instance->MACHT1R = hash_table[1];
#else
	heth->Instance->MACHTLR = hash_table[0];
	heth->Instance->MACHTHR = hash_table[1];
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
}

#endif /* CONFIG_ETH_STM32_MULTICAST_FILTER */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
static int eth_init_api_v2(const struct device *dev)
{
	HAL_StatusTypeDef hal_ret = HAL_OK;
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet)
	for (int ch = 0; ch < ETH_DMA_CH_CNT; ch++) {
		heth->Init.TxDesc[ch] = dma_tx_desc_tab[ch];
		heth->Init.RxDesc[ch] = dma_rx_desc_tab[ch];
	}
#else
	heth->Init.TxDesc = dma_tx_desc_tab;
	heth->Init.RxDesc = dma_rx_desc_tab;
#endif
	heth->Init.RxBuffLen = ETH_STM32_RX_BUF_SIZE;

	hal_ret = HAL_ETH_Init(heth);
	if (hal_ret == HAL_TIMEOUT) {
		/* HAL Init time out. This could be linked to */
		/* a recoverable error. Log the issue and continue */
		/* driver initialisation */
		LOG_ERR("HAL_ETH_Init Timed out");
	} else if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Init failed: %d", hal_ret);
		return -EINVAL;
	}

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	/* Enable timestamping of RX packets. We enable all packets to be
	 * timestamped to cover both IEEE 1588 and gPTP.
	 */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSENALL;
#else
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSSARFE;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

	/* Initialize semaphores */
	k_mutex_init(&dev_data->tx_mutex);
	k_sem_init(&dev_data->rx_int_sem, 0, K_SEM_MAX_LIMIT);
	k_sem_init(&dev_data->tx_int_sem, 0, K_SEM_MAX_LIMIT);

	/* Tx config init: */
	memset(&tx_config, 0, sizeof(ETH_TxPacketConfig));
	tx_config.Attributes = ETH_TX_PACKETS_FEATURES_CSUM |
				ETH_TX_PACKETS_FEATURES_CRCPAD;
	tx_config.ChecksumCtrl = IS_ENABLED(CONFIG_ETH_STM32_HW_CHECKSUM) ?
			ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC : ETH_CHECKSUM_DISABLE;
	tx_config.CRCPadCtrl = ETH_CRC_PAD_INSERT;

	/* prepare tx buffer header */
	for (uint16_t i = 0; i < ETH_TXBUFNB; ++i) {
		dma_tx_buffer_header[i].tx_buff.buffer = dma_tx_buffer[i];
	}

	return 0;
}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */

static void set_mac_config(const struct device *dev, struct phy_link_state *state)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	HAL_StatusTypeDef hal_ret = HAL_OK;
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	ETH_MACConfigTypeDef mac_config = {0};

	HAL_ETH_GetMACConfig(heth, &mac_config);

	mac_config.DuplexMode =
		PHY_LINK_IS_FULL_DUPLEX(state->speed) ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;

	mac_config.Speed =
		IF_ENABLED(DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet),
			PHY_LINK_IS_SPEED_1000M(state->speed) ? ETH_SPEED_1000M :)
		PHY_LINK_IS_SPEED_100M(state->speed) ? ETH_SPEED_100M : ETH_SPEED_10M;

	hal_ret = HAL_ETH_SetMACConfig(heth, &mac_config);
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_SetMACConfig: failed: %d", hal_ret);
	}
#else  /* CONFIG_ETH_STM32_HAL_API_V2 */
	heth->Init.DuplexMode =
		PHY_LINK_IS_FULL_DUPLEX(state->speed) ? ETH_MODE_FULLDUPLEX : ETH_MODE_HALFDUPLEX;

	heth->Init.Speed = PHY_LINK_IS_SPEED_100M(state->speed) ? ETH_SPEED_100M : ETH_SPEED_10M;

	hal_ret = HAL_ETH_ConfigMAC(heth, NULL);
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_ConfigMAC: failed: %d", hal_ret);
	}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */
}

static int eth_stm32_hal_start(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	HAL_StatusTypeDef hal_ret = HAL_OK;

	LOG_DBG("Starting ETH HAL driver");

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	hal_ret = HAL_ETH_Start_IT(heth);
#else
	hal_ret = HAL_ETH_Start(heth);
#endif
	if (hal_ret != HAL_OK) {
		LOG_ERR("HAL_ETH_Start{_IT} failed");
	}

	return 0;
}

static int eth_stm32_hal_stop(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	HAL_StatusTypeDef hal_ret = HAL_OK;

	LOG_DBG("Stopping ETH HAL driver");

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	hal_ret = HAL_ETH_Stop_IT(heth);
#else
	hal_ret = HAL_ETH_Stop(heth);
#endif
	if (hal_ret != HAL_OK) {
		/* HAL_ETH_Stop{_IT} returns HAL_ERROR only if ETH is already stopped */
		LOG_DBG("HAL_ETH_Stop{_IT} returned error (Ethernet is already stopped)");
	}

	return 0;
}

static void phy_link_state_changed(const struct device *phy_dev, struct phy_link_state *state,
				   void *user_data)
{
	const struct device *dev = (const struct device *)user_data;
	struct eth_stm32_hal_dev_data *dev_data = dev->data;

	ARG_UNUSED(phy_dev);

	/* The hal also needs to be stopped before changing the MAC config.
	 * The speed can change without receiving a link down callback before.
	 */
	eth_stm32_hal_stop(dev);
	if (state->is_up) {
		set_mac_config(dev, state);
		eth_stm32_hal_start(dev);
		net_eth_carrier_on(dev_data->iface);
	} else {
		net_eth_carrier_off(dev_data->iface);
	}
}

static void eth_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;
	bool is_first_init = false;

	if (dev_data->iface == NULL) {
		dev_data->iface = iface;
		is_first_init = true;
	}

	/* Register Ethernet MAC Address with the upper layer */
	net_if_set_link_addr(iface, dev_data->mac_addr,
			     sizeof(dev_data->mac_addr),
			     NET_LINK_ETHERNET);

#if defined(CONFIG_NET_DSA_DEPRECATED)
	dsa_register_master_tx(iface, &eth_tx);
#endif

	ethernet_init(iface);

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	/* This function requires the Ethernet interface to be
	 * properly initialized. In auto-negotiation mode, it reads the speed
	 * and duplex settings to configure the driver accordingly.
	 */
	eth_init_api_v2(dev);
#endif

	setup_mac_filter(heth);

	net_if_carrier_off(iface);

	net_lldp_set_lldpdu(iface);

	if (device_is_ready(eth_stm32_phy_dev)) {
		phy_link_callback_set(eth_stm32_phy_dev, phy_link_state_changed, (void *)dev);
	} else {
		LOG_ERR("PHY device not ready");
	}

	if (is_first_init) {
		const struct eth_stm32_hal_dev_cfg *cfg = dev->config;
		/* Now that the iface is setup, we are safe to enable IRQs. */
		__ASSERT_NO_MSG(cfg->config_func != NULL);
		cfg->config_func();

		/* Start interruption-poll thread */
		k_thread_create(&dev_data->rx_thread, dev_data->rx_thread_stack,
				K_KERNEL_STACK_SIZEOF(dev_data->rx_thread_stack),
				rx_thread, (void *) dev, NULL, NULL,
				IS_ENABLED(CONFIG_ETH_STM32_HAL_RX_THREAD_PREEMPTIVE)
					? K_PRIO_PREEMPT(CONFIG_ETH_STM32_HAL_RX_THREAD_PRIO)
					: K_PRIO_COOP(CONFIG_ETH_STM32_HAL_RX_THREAD_PRIO),
				0, K_NO_WAIT);

		k_thread_name_set(&dev_data->rx_thread, "stm_eth");
	}
}

static enum ethernet_hw_caps eth_stm32_hal_get_capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);

	return ETHERNET_LINK_10BASE | ETHERNET_LINK_100BASE
#if defined(CONFIG_NET_VLAN)
		| ETHERNET_HW_VLAN
#endif
#if defined(CONFIG_NET_PROMISCUOUS_MODE)
		| ETHERNET_PROMISC_MODE
#endif
#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
		| ETHERNET_PTP
#endif
#if defined(CONFIG_NET_LLDP)
		| ETHERNET_LLDP
#endif
#if defined(CONFIG_ETH_STM32_HW_CHECKSUM)
		| ETHERNET_HW_RX_CHKSUM_OFFLOAD
		| ETHERNET_HW_TX_CHKSUM_OFFLOAD
#endif
#if defined(CONFIG_NET_DSA_DEPRECATED)
		| ETHERNET_DSA_CONDUIT_PORT
#endif
#if defined(CONFIG_ETH_STM32_MULTICAST_FILTER)
		| ETHERNET_HW_FILTERING
#endif
		;
}

static int eth_stm32_hal_set_config(const struct device *dev,
				    enum ethernet_config_type type,
				    const struct ethernet_config *config)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;
	ETH_HandleTypeDef *heth = &dev_data->heth;

	switch (type) {
	case ETHERNET_CONFIG_TYPE_MAC_ADDRESS:
		memcpy(dev_data->mac_addr, config->mac_address.addr, 6);
		heth->Instance->MACA0HR = (dev_data->mac_addr[5] << 8) |
			dev_data->mac_addr[4];
		heth->Instance->MACA0LR = (dev_data->mac_addr[3] << 24) |
			(dev_data->mac_addr[2] << 16) |
			(dev_data->mac_addr[1] << 8) |
			dev_data->mac_addr[0];
		net_if_set_link_addr(dev_data->iface, dev_data->mac_addr,
				     sizeof(dev_data->mac_addr),
				     NET_LINK_ETHERNET);
		return 0;
#if defined(CONFIG_NET_PROMISCUOUS_MODE)
	case ETHERNET_CONFIG_TYPE_PROMISC_MODE:
#if defined(CONFIG_ETH_STM32_HAL_API_V2)
		ETH_MACFilterConfigTypeDef MACFilterConf;

		HAL_ETH_GetMACFilterConfig(heth, &MACFilterConf);

		MACFilterConf.PromiscuousMode = config->promisc_mode ? ENABLE : DISABLE;

		HAL_ETH_SetMACFilterConfig(heth, &MACFilterConf);
#else
		if (config->promisc_mode) {
			heth->Instance->MACFFR |= ETH_MACFFR_PM;
		} else {
			heth->Instance->MACFFR &= ~ETH_MACFFR_PM;
		}
#endif /* CONFIG_ETH_STM32_HAL_API_V2 */
		return 0;
#endif /* CONFIG_NET_PROMISCUOUS_MODE */
#if defined(CONFIG_ETH_STM32_MULTICAST_FILTER)
	case ETHERNET_CONFIG_TYPE_FILTER:
		eth_stm32_mcast_filter(dev, &config->filter);
		return 0;
#endif /* CONFIG_ETH_STM32_MULTICAST_FILTER */
	default:
		break;
	}

	return -ENOTSUP;
}

static const struct device *eth_stm32_hal_get_phy(const struct device *dev)
{
	ARG_UNUSED(dev);
	return eth_stm32_phy_dev;
}

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
static const struct device *eth_stm32_get_ptp_clock(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;

	return dev_data->ptp_clock;
}
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */

#if defined(CONFIG_NET_STATISTICS_ETHERNET)
static struct net_stats_eth *eth_stm32_hal_get_stats(const struct device *dev)
{
	struct eth_stm32_hal_dev_data *dev_data = dev->data;

	return &dev_data->stats;
}
#endif /* CONFIG_NET_STATISTICS_ETHERNET */

static const struct ethernet_api eth_api = {
	.iface_api.init = eth_iface_init,
#if defined(CONFIG_PTP_CLOCK_STM32_HAL)
	.get_ptp_clock = eth_stm32_get_ptp_clock,
#endif /* CONFIG_PTP_CLOCK_STM32_HAL */
	.get_capabilities = eth_stm32_hal_get_capabilities,
	.set_config = eth_stm32_hal_set_config,
	.get_phy = eth_stm32_hal_get_phy,
#if defined(CONFIG_NET_DSA_DEPRECATED)
	.send = dsa_tx,
#else
	.send = eth_tx,
#endif
#if defined(CONFIG_NET_STATISTICS_ETHERNET)
	.get_stats = eth_stm32_hal_get_stats,
#endif /* CONFIG_NET_STATISTICS_ETHERNET */
};

static void eth0_irq_config(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), eth_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

PINCTRL_DT_INST_DEFINE(0);

static const struct eth_stm32_hal_dev_cfg eth0_config = {
	.config_func = eth0_irq_config,
	.pclken = {.bus = DT_CLOCKS_CELL_BY_NAME(DT_INST_PARENT(0), stm_eth, bus),
		   .enr = DT_CLOCKS_CELL_BY_NAME(DT_INST_PARENT(0), stm_eth, bits)},
	.pclken_tx = {.bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_tx, bus),
		      .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_tx, bits)},
	.pclken_rx = {.bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_rx, bus),
		      .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_rx, bits)},
#if DT_INST_CLOCKS_HAS_NAME(0, mac_clk_ptp)
	.pclken_ptp = {.bus = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_ptp, bus),
		       .enr = DT_INST_CLOCKS_CELL_BY_NAME(0, mac_clk_ptp, bits)},
#endif
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
};

BUILD_ASSERT(DT_INST_ENUM_HAS_VALUE(0, phy_connection_type, mii)
	|| DT_INST_ENUM_HAS_VALUE(0, phy_connection_type, rmii)
	IF_ENABLED(DT_HAS_COMPAT_STATUS_OKAY(st_stm32n6_ethernet),
	(|| DT_INST_ENUM_HAS_VALUE(0, phy_connection_type, rgmii)
	 || DT_INST_ENUM_HAS_VALUE(0, phy_connection_type, gmii))),
			"Unsupported PHY connection type");

static struct eth_stm32_hal_dev_data eth0_data = {
	.heth = {
		.Instance = (ETH_TypeDef *)DT_REG_ADDR(DT_INST_PARENT(0)),
		.Init = {
#if defined(CONFIG_ETH_STM32_HAL_API_V1)
			.AutoNegotiation = ETH_STM32_AUTO_NEGOTIATION_ENABLE ?
					   ETH_AUTONEGOTIATION_ENABLE : ETH_AUTONEGOTIATION_DISABLE,
			.PhyAddress = DT_REG_ADDR(DT_INST_PHANDLE(0, phy_handle)),
			.RxMode = ETH_RXINTERRUPT_MODE,
			.ChecksumMode = IS_ENABLED(CONFIG_ETH_STM32_HW_CHECKSUM) ?
					ETH_CHECKSUM_BY_HARDWARE : ETH_CHECKSUM_BY_SOFTWARE,
#endif /* CONFIG_ETH_STM32_HAL_API_V1 */
			.MediaInterface = STM32_ETH_PHY_MODE(0),
		},
	},
};

ETH_NET_DEVICE_DT_INST_DEFINE(0, eth_initialize,
		    NULL, &eth0_data, &eth0_config,
		    CONFIG_ETH_INIT_PRIORITY, &eth_api, ETH_STM32_HAL_MTU);

#if defined(CONFIG_PTP_CLOCK_STM32_HAL)

struct ptp_context {
	struct eth_stm32_hal_dev_data *eth_dev_data;
};

static struct ptp_context ptp_stm32_0_context;

static int ptp_clock_stm32_set(const struct device *dev,
			      struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	ETH_HandleTypeDef *heth = &eth_dev_data->heth;
	unsigned int key;

	key = irq_lock();

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACSTSUR = tm->second;
	heth->Instance->MACSTNUR = tm->nanosecond;
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSINIT;
	while (heth->Instance->MACTSCR & ETH_MACTSCR_TSINIT_Msk) {
		/* spin lock */
	}
#else
	heth->Instance->PTPTSHUR = tm->second;
	heth->Instance->PTPTSLUR = tm->nanosecond;
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSSTI;
	while (heth->Instance->PTPTSCR & ETH_PTPTSCR_TSSTI_Msk) {
		/* spin lock */
	}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	irq_unlock(key);

	return 0;
}

static int ptp_clock_stm32_get(const struct device *dev,
			      struct net_ptp_time *tm)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	ETH_HandleTypeDef *heth = &eth_dev_data->heth;
	unsigned int key;
	uint32_t second_2;

	key = irq_lock();

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	tm->second = heth->Instance->MACSTSR;
	tm->nanosecond = heth->Instance->MACSTNR;
	second_2 = heth->Instance->MACSTSR;
#else
	tm->second = heth->Instance->PTPTSHR;
	tm->nanosecond = heth->Instance->PTPTSLR;
	second_2 = heth->Instance->PTPTSHR;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	irq_unlock(key);

	if (tm->second != second_2 && tm->nanosecond < NSEC_PER_SEC / 2) {
		/* Second rollover has happened during first measurement: second register
		 * was read before second boundary and nanosecond register was read after.
		 * We will use second_2 as a new second value.
		 */
		tm->second = second_2;
	}

	return 0;
}

static int ptp_clock_stm32_adjust(const struct device *dev, int increment)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	ETH_HandleTypeDef *heth = &eth_dev_data->heth;
	int key, ret;

	if ((increment <= (int32_t)(-NSEC_PER_SEC)) ||
			(increment >= (int32_t)NSEC_PER_SEC)) {
		ret = -EINVAL;
	} else {
		key = irq_lock();

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
		heth->Instance->MACSTSUR = 0;
		if (increment >= 0) {
			heth->Instance->MACSTNUR = increment;
		} else {
			heth->Instance->MACSTNUR = ETH_MACSTNUR_ADDSUB | (NSEC_PER_SEC + increment);
		}
		heth->Instance->MACTSCR |= ETH_MACTSCR_TSUPDT;
		while (heth->Instance->MACTSCR & ETH_MACTSCR_TSUPDT_Msk) {
			/* spin lock */
		}
#else
		heth->Instance->PTPTSHUR = 0;
		if (increment >= 0) {
			heth->Instance->PTPTSLUR = increment;
		} else {
			heth->Instance->PTPTSLUR = ETH_PTPTSLUR_TSUPNS | (-increment);
		}
		heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSSTU;
		while (heth->Instance->PTPTSCR & ETH_PTPTSCR_TSSTU_Msk) {
			/* spin lock */
		}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

		ret = 0;
		irq_unlock(key);
	}

	return ret;
}

static int ptp_clock_stm32_rate_adjust(const struct device *dev, double ratio)
{
	struct ptp_context *ptp_context = dev->data;
	struct eth_stm32_hal_dev_data *eth_dev_data = ptp_context->eth_dev_data;
	ETH_HandleTypeDef *heth = &eth_dev_data->heth;
	int key, ret;
	uint32_t addend_val;

	/* No change needed */
	if (ratio == 1.0L) {
		return 0;
	}

	key = irq_lock();

	ratio *= (double)eth_dev_data->clk_ratio_adj;

	/* Limit possible ratio */
	if (ratio * 100 < CONFIG_ETH_STM32_HAL_PTP_CLOCK_ADJ_MIN_PCT ||
			ratio * 100 > CONFIG_ETH_STM32_HAL_PTP_CLOCK_ADJ_MAX_PCT) {
		ret = -EINVAL;
		goto error;
	}

	/* Save new ratio */
	eth_dev_data->clk_ratio_adj = ratio;

	/* Update addend register */
	addend_val = UINT32_MAX * (double)eth_dev_data->clk_ratio * ratio;

#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSAR = addend_val;
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSADDREG;
	while (heth->Instance->MACTSCR & ETH_MACTSCR_TSADDREG_Msk) {
		/* spin lock */
	}
#else
	heth->Instance->PTPTSAR = addend_val;
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSARU;
	while (heth->Instance->PTPTSCR & ETH_PTPTSCR_TSARU_Msk) {
		/* spin lock */
	}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	ret = 0;

error:
	irq_unlock(key);

	return ret;
}

static DEVICE_API(ptp_clock, api) = {
	.set = ptp_clock_stm32_set,
	.get = ptp_clock_stm32_get,
	.adjust = ptp_clock_stm32_adjust,
	.rate_adjust = ptp_clock_stm32_rate_adjust,
};

static int ptp_stm32_init(const struct device *port)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(mac));
	struct eth_stm32_hal_dev_data *eth_dev_data = dev->data;
	const struct eth_stm32_hal_dev_cfg *eth_cfg = dev->config;
	struct ptp_context *ptp_context = port->data;
	ETH_HandleTypeDef *heth = &eth_dev_data->heth;
	int ret;
	uint32_t ptp_hclk_rate;
	uint32_t ss_incr_ns;
	uint32_t addend_val;

	eth_dev_data->ptp_clock = port;
	ptp_context->eth_dev_data = eth_dev_data;

	/* Mask the Timestamp Trigger interrupt */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACIER &= ~(ETH_MACIER_TSIE);
#else
	heth->Instance->MACIMR &= ~(ETH_MACIMR_TSTIM);
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Enable timestamping */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSENA;
#else
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSE;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Query ethernet clock rate */
	ret = clock_control_get_rate(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
				     (clock_control_subsys_t)&eth_cfg->pclken,
#else
				     (clock_control_subsys_t)&eth_cfg->pclken_ptp,
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */
				     &ptp_hclk_rate);
	if (ret) {
		LOG_ERR("Failed to query ethernet clock");
		return -EIO;
	}

	/* Program the subsecond increment register based on the PTP clock freq */
	if (NSEC_PER_SEC % CONFIG_ETH_STM32_HAL_PTP_CLOCK_SRC_HZ != 0) {
		LOG_ERR("PTP clock period must be an integer nanosecond value");
		return -EINVAL;
	}
	ss_incr_ns = NSEC_PER_SEC / CONFIG_ETH_STM32_HAL_PTP_CLOCK_SRC_HZ;
	if (ss_incr_ns > UINT8_MAX) {
		LOG_ERR("PTP clock period is more than %d nanoseconds", UINT8_MAX);
		return -EINVAL;
	}
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACSSIR = ss_incr_ns << ETH_MACMACSSIR_SSINC_Pos;
#else
	heth->Instance->PTPSSIR = ss_incr_ns;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Program timestamp addend register */
	eth_dev_data->clk_ratio =
		((double)CONFIG_ETH_STM32_HAL_PTP_CLOCK_SRC_HZ) / ((double)ptp_hclk_rate);
	/*
	 * clk_ratio is a ratio between desired PTP clock frequency and HCLK rate.
	 * Because HCLK is defined by a physical oscillator, it might drift due
	 * to manufacturing tolerances and environmental effects (e.g. temperature).
	 * clk_ratio_adj compensates for such inaccuracies. It starts off as 1.0
	 * and gets adjusted by calling ptp_clock_stm32_rate_adjust().
	 */
	eth_dev_data->clk_ratio_adj = 1.0f;
	addend_val =
		UINT32_MAX * eth_dev_data->clk_ratio * eth_dev_data->clk_ratio_adj;
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSAR = addend_val;
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSADDREG;
	while (heth->Instance->MACTSCR & ETH_MACTSCR_TSADDREG_Msk) {
		k_yield();
	}
#else
	heth->Instance->PTPTSAR = addend_val;
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSARU;
	while (heth->Instance->PTPTSCR & ETH_PTPTSCR_TSARU_Msk) {
		k_yield();
	}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Enable fine timestamp correction method */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSCFUPDT;
#else
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSFCU;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Enable nanosecond rollover into a new second */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSCTRLSSR;
#else
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSSSR;
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

	/* Initialize timestamp */
#if DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet)
	heth->Instance->MACSTSUR = 0;
	heth->Instance->MACSTNUR = 0;
	heth->Instance->MACTSCR |= ETH_MACTSCR_TSINIT;
	while (heth->Instance->MACTSCR & ETH_MACTSCR_TSINIT_Msk) {
		k_yield();
	}
#else
	heth->Instance->PTPTSHUR = 0;
	heth->Instance->PTPTSLUR = 0;
	heth->Instance->PTPTSCR |= ETH_PTPTSCR_TSSTI;
	while (heth->Instance->PTPTSCR & ETH_PTPTSCR_TSSTI_Msk) {
		k_yield();
	}
#endif /* DT_HAS_COMPAT_STATUS_OKAY(st_stm32h7_ethernet) */

#if defined(CONFIG_ETH_STM32_HAL_API_V2)
	/* Set PTP Configuration done */
	heth->IsPtpConfigured = ETH_STM32_PTP_CONFIGURED;
#endif

	return 0;
}

DEVICE_DEFINE(stm32_ptp_clock_0, PTP_CLOCK_NAME, ptp_stm32_init,
		NULL, &ptp_stm32_0_context, NULL, POST_KERNEL,
		CONFIG_ETH_STM32_HAL_PTP_CLOCK_INIT_PRIO, &api);

#endif /* CONFIG_PTP_CLOCK_STM32_HAL */
