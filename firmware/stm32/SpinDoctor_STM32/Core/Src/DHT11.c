#include "DHT11.h"

extern void delay_us(uint32_t us);  /* defined in main.c USER CODE 4 */

/* All GPIO calls reference DHT11_DATA_GPIO_Port/Pin, the CubeMX-
 * generated macros from PB6's User Label, instead of hardcoding
 * GPIOA/PIN_1 like the original PA1 version. This is the only real
 * change needed to port this driver to a different pin. */

uint8_t DHT11_Start(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin   = DHT11_DATA_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);

    HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_RESET);
    delay_us(18000);

    HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_SET);
    delay_us(30);

    GPIO_InitStruct.Pin  = DHT11_DATA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);

    uint32_t timeout = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == GPIO_PIN_SET)
    {
        if ((DWT->CYCCNT - timeout) > (100 * (SystemCoreClock / 1000000U)))
            return 0;
    }

    timeout = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == GPIO_PIN_RESET)
    {
        if ((DWT->CYCCNT - timeout) > (100 * (SystemCoreClock / 1000000U)))
            return 0;
    }

    timeout = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == GPIO_PIN_SET)
    {
        if ((DWT->CYCCNT - timeout) > (100 * (SystemCoreClock / 1000000U)))
            return 0;
    }

    return 1;
}

uint8_t DHT11_ReadBit(void)
{
    uint32_t timeout;

    timeout = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == GPIO_PIN_RESET)
    {
        if ((DWT->CYCCNT - timeout) > (100 * (SystemCoreClock / 1000000U)))
            return 0xFF;
    }

    uint32_t start = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == GPIO_PIN_SET)
    {
        if ((DWT->CYCCNT - start) > (150 * (SystemCoreClock / 1000000U)))
            return 0xFF;
    }
    uint32_t elapsed_us = (DWT->CYCCNT - start) / (SystemCoreClock / 1000000U);

    return (elapsed_us > 40) ? 1 : 0;
}

uint8_t DHT11_Read(DHT11_Data *data)
{
    if (!DHT11_Start())
        return 0;

    uint8_t bytes[5] = {0};

    for (int byte_idx = 0; byte_idx < 5; byte_idx++)
    {
        for (int bit_idx = 0; bit_idx < 8; bit_idx++)
        {
            uint8_t bit = DHT11_ReadBit();
            if (bit == 0xFF)
                return 0;

            bytes[byte_idx] = (bytes[byte_idx] << 1) | bit;
        }
    }

    uint8_t sum = bytes[0] + bytes[1] + bytes[2] + bytes[3];
    if (sum != bytes[4])
        return 0;

    data->humidity_int = bytes[0];
    data->humidity_dec = bytes[1];
    data->temp_int     = bytes[2];
    data->temp_dec     = bytes[3];
    data->checksum     = bytes[4];

    return 1;
}
