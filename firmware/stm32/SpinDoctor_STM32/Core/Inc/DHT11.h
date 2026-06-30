#ifndef DHT11_H
#define DHT11_H

#include "main.h"

typedef struct
{
    uint8_t humidity_int;
    uint8_t humidity_dec;
    uint8_t temp_int;
    uint8_t temp_dec;
    uint8_t checksum;
} DHT11_Data;

uint8_t DHT11_Start(void);
uint8_t DHT11_ReadBit(void);
uint8_t DHT11_Read(DHT11_Data *data);

#endif
