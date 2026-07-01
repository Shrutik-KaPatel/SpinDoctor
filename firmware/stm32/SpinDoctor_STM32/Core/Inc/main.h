/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Shared diagnostics struct, written by AccelTask and DHT11Task,
 * eventually read by a future UART/ESP32-handoff task. Protected by
 * diagnosticsMutex, any code touching this struct must hold that
 * mutex first, no exceptions, that's the whole point of having it. */

/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>

/* Shared diagnostics struct, written by AccelTask and DHT11Task,
 * eventually read by a future UART/ESP32-handoff task. Protected by
 * diagnosticsMutex, any code touching this struct must hold that
 * mutex first, no exceptions, that's the whole point of having it. */
typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    uint8_t humidity_int;
    uint8_t humidity_dec;
    uint8_t temp_int;
    uint8_t temp_dec;
} DiagnosticsData;

extern DiagnosticsData diagnostics;

#define FFT_SIZE 256

/* Raw sample accumulation buffers, filled by AccelTask one sample at
 * a time. Once fft_sample_count reaches FFT_SIZE, AccelTask swaps to
 * the other half of a ping-pong pair and signals FFTTask to process
 * the completed half. This is a block-level ping-pong, separate from
 * the single-sample ping-pong already in LIS3DSHTR.c, needed because
 * FFT processing takes long enough that the next accumulation window
 * could otherwise start overwriting data FFTTask hasn't read yet. */
#include <stdint.h>
#include "arm_math.h"
extern float32_t fft_buf_x[2][FFT_SIZE];
extern float32_t fft_buf_y[2][FFT_SIZE];
extern float32_t fft_buf_z[2][FFT_SIZE];
extern volatile uint8_t fft_fill_idx;   /* which half AccelTask is currently filling */
extern volatile uint16_t fft_sample_count;
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LIS3DSH_CS_Pin GPIO_PIN_3
#define LIS3DSH_CS_GPIO_Port GPIOE
#define DHT11_DATA_Pin GPIO_PIN_6
#define DHT11_DATA_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
