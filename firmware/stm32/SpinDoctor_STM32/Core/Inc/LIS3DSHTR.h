#ifndef LIS3DSHTR_H
#define LIS3DSHTR_H

#include "stm32f4xx_hal.h"

/* Register addresses */
#define LIS3_WHO_AM_I   0x0F
#define LIS3_CTRL_REG4  0x20
#define LIS3_OUT_X_L    0x28
#define LIS3_OUT_X_H    0x29
#define LIS3_OUT_Y_L    0x2A
#define LIS3_OUT_Y_H    0x2B
#define LIS3_OUT_Z_L    0x2C
#define LIS3_OUT_Z_H    0x2D
#define LIS3_CTRL_REG4_100HZ_XYZ  0x67	/* CTRL_REG4: ODR=100Hz (0110), BDU=0, Zen=1, Yen=1, Xen=1 */

#define LIS3_CTRL_REG3  0x23
#define LIS3_CTRL_REG4_400HZ_XYZ  0x77
#define LIS3_CTRL_REG3_DRDY_INT1  0xE8  /* DRDY routed to INT1 */


/* Handle struct — makes library portable across any SPI and CS pin */

typedef struct {
	SPI_HandleTypeDef *hspi;
	GPIO_TypeDef *cs_port;
	uint16_t cs_pin;

}LIS3_HandleTypeDef;

/* Raw XYZ data struct */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} LIS3_DataTypeDef;

/* Public API */
void LIS3_Init(LIS3_HandleTypeDef *hlis);
void LIS3_ReadXYZ(LIS3_HandleTypeDef *hlis, LIS3_DataTypeDef *data);
void LIS3_WriteReg(LIS3_HandleTypeDef *hlis, uint8_t reg, uint8_t data);
void LIS3_StartBurstRead_DMA(LIS3_HandleTypeDef *hlis, LIS3_DataTypeDef *data);
void LIS3_DMA_TxCpltHandler(void);
void LIS3_DMA_RxCpltHandler(void);
extern volatile uint8_t LIS3_DataReady;

#endif /* LIS3DSHTR_H */









