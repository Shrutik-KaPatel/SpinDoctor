#include "LIS3DSHTR.h"

void LIS3_WriteReg(LIS3_HandleTypeDef *hlis, uint8_t reg, uint8_t data)
{
	 uint8_t tx[2] = { reg & 0x7F, data };
	 HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_RESET);
	 HAL_SPI_Transmit(hlis->hspi, tx, 2, HAL_MAX_DELAY);
	 HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_SET);
}

uint8_t LIS3_ReadReg(LIS3_HandleTypeDef *hlis, uint8_t reg)
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
	data->x = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_X_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_X_L) );
	data->y = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_Y_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_Y_L) );
	data->z = (int16_t)(LIS3_ReadReg(hlis, LIS3_OUT_Z_H) << 8 | LIS3_ReadReg(hlis, LIS3_OUT_Z_L) );
}
/* ---------------------------------------------------------------------
 * DMA burst-read pipeline. Same SPI transaction shape as LIS3_ReadXYZ
 * (transmit address, then receive data), just chained across two DMA
 * completion callbacks instead of blocking sequentially. Single active
 * device assumed, fine for this project since there's only one LIS3DSH.
 * ------------------------------------------------------------------- */
volatile uint8_t LIS3_DataReady = 0;

static LIS3_HandleTypeDef *active_hlis;
static LIS3_DataTypeDef   *active_data;
static uint8_t burst_tx_addr;
static uint8_t burst_rx_buf[6];  /* X_L,X_H,Y_L,Y_H,Z_L,Z_H */

void LIS3_StartBurstRead_DMA(LIS3_HandleTypeDef *hlis, LIS3_DataTypeDef *data)
{
    active_hlis    = hlis;
    active_data    = data;
    burst_tx_addr  = LIS3_OUT_X_L | 0x80;  /* read bit set, ADD_INC handles the rest */

    HAL_GPIO_WritePin(hlis->cs_port, hlis->cs_pin, GPIO_PIN_RESET);
    HAL_SPI_Transmit_DMA(hlis->hspi, &burst_tx_addr, 1);
}

/* Called from HAL_SPI_TxCpltCallback once the address byte has landed */
void LIS3_DMA_TxCpltHandler(void)
{
    HAL_SPI_Receive_DMA(active_hlis->hspi, burst_rx_buf, 6);
}

/* Called from HAL_SPI_RxCpltCallback once all 6 data bytes have landed */
void LIS3_DMA_RxCpltHandler(void)
{
    HAL_GPIO_WritePin(active_hlis->cs_port, active_hlis->cs_pin, GPIO_PIN_SET);

    active_data->x = (int16_t)(burst_rx_buf[1] << 8 | burst_rx_buf[0]);
    active_data->y = (int16_t)(burst_rx_buf[3] << 8 | burst_rx_buf[2]);
    active_data->z = (int16_t)(burst_rx_buf[5] << 8 | burst_rx_buf[4]);

    LIS3_DataReady = 1;
}
