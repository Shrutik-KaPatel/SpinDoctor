#include "LIS3DSHTR.h"

void LIS3_WriteReg(LIS3_HandleTypeDef *hlis, uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = { reg & 0x7F, data };
    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(hlis->hspi, tx, 2, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_SET);
}

static uint8_t LIS3_ReadReg(LIS3_HandleTypeDef *hlis, uint8_t reg)
{
    uint8_t tx = reg | 0x80;
    uint8_t rx = 0;
    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit(hlis->hspi, &tx, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(hlis->hspi, &rx, 1, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_SET);
    return rx;
}

void LIS3_Init(LIS3_HandleTypeDef *hlis)
{
    LIS3_WriteReg(hlis, LIS3_CTRL_REG4, LIS3_CTRL_REG4_400HZ_XYZ);
    HAL_Delay(10);
}

void LIS3_ReadXYZ(LIS3_HandleTypeDef *hlis, LIS3_DataTypeDef *data)
{
    data->x = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_X_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_X_L));
    data->y = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_Y_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_Y_L));
    data->z = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_Z_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_Z_L));
}

/* ---------------------------------------------------------------------
 * DMA burst-read pipeline with ping-pong double buffering.
 *
 * Two 6-byte receive buffers (ping and pong) alternate on each DMA
 * completion. DMA always fills whichever buffer the consumer is NOT
 * currently reading, so AccelTask can safely read a completed buffer
 * while the next 400Hz sample is already being received into the other.
 * Without this, a slow consumer (FFT, NanoEdge inference) could still
 * be reading burst_rx_buf when the next DRDY fires and DMA overwrites
 * it with new data, corrupting the sample mid-read.
 *
 * active_buf_idx tracks which buffer DMA just finished writing.
 * The consumer reads buf[active_buf_idx], DMA writes into the other.
 * ------------------------------------------------------------------- */
volatile uint8_t LIS3_DataReady = 0;

static LIS3_HandleTypeDef *active_hlis;
static LIS3_DataTypeDef   *active_data;
static uint8_t burst_tx_addr;

/* Two buffers, layout: [X_L, X_H, Y_L, Y_H, Z_L, Z_H] */
static uint8_t burst_rx_buf[2][6];
static uint8_t active_buf_idx = 0;  /* index of the buffer DMA just finished filling */

void LIS3_StartBurstRead_DMA(LIS3_HandleTypeDef *hlis, LIS3_DataTypeDef *data)
{
    active_hlis   = hlis;
    active_data   = data;
    burst_tx_addr = LIS3_OUT_X_L | 0x80;

    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit_DMA(hlis->hspi, &burst_tx_addr, 1);
}

/* Called from HAL_SPI_TxCpltCallback once the address byte has landed.
 * DMA receives into whichever buffer is NOT the one AccelTask is
 * currently reading. Toggle first, then point DMA at the new target. */
void LIS3_DMA_TxCpltHandler(void)
{
    uint8_t write_buf = 1 - active_buf_idx;  /* the buffer AccelTask is NOT using */
    HAL_SPI_Receive_DMA(active_hlis->hspi, burst_rx_buf[write_buf], 6);
}

/* Called from HAL_SPI_RxCpltCallback once all 6 bytes have landed.
 * Flip active_buf_idx to point at the freshly filled buffer, then
 * reconstruct XYZ from it and signal AccelTask. */
void LIS3_DMA_RxCpltHandler(void)
{
    HAL_GPIO_WritePin(active_hlis->cs_port, active_hlis->cs_pin, GPIO_PIN_SET);

    /* Flip: the buffer DMA just wrote into is now the active (readable) one */
    active_buf_idx = 1 - active_buf_idx;

    uint8_t *buf = burst_rx_buf[active_buf_idx];
    active_data->x = (int16_t)(buf[1] << 8 | buf[0]);
    active_data->y = (int16_t)(buf[3] << 8 | buf[2]);
    active_data->z = (int16_t)(buf[5] << 8 | buf[4]);

    LIS3_DataReady = 1;
}
