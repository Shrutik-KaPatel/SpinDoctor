/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <LIS3DSHTR.h>
#include "DHT11.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

IWDG_HandleTypeDef hiwdg;

SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_rx;
DMA_HandleTypeDef hdma_spi1_tx;

UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart2_tx;

osThreadId AccelTaskHandle;
osThreadId DHT11TaskHandle;
osThreadId WatchdogTaskHandle;
osThreadId FFTTaskHandle;
osMutexId diagnosticsMutexHandle;
osMutexId printfMutexHandle;
osSemaphoreId uartTxSemaphoreHandle;
osSemaphoreId fftDataReadySemaphoreHandle;
/* USER CODE BEGIN PV */
LIS3_HandleTypeDef hlis;
LIS3_DataTypeDef   data;

/* Single-element circular DMA buffer for ADC1 temperature reads.
 * volatile because DMA writes it from hardware, not CPU code.
 * uint16_t because ADC is 12-bit (max 4095), needs 16-bit storage. */
volatile uint16_t adc_temp_raw;

/* DWT cycle counter, used for precise microsecond delays needed by
 * the 1-Wire protocol. Cortex-M4 core feature, not a normal STM32
 * peripheral, has nothing to do with HAL and needs no CubeMX setup. */
#define DWT_CYCCNT  (*(volatile uint32_t*)0xE0001004)
#define DWT_CTRL    (*(volatile uint32_t*)0xE0001000)
#define DEM_CR      (*(volatile uint32_t*)0xE000EDFC)

DiagnosticsData diagnostics = {0};

float32_t fft_buf_x[2][FFT_SIZE];
float32_t fft_buf_y[2][FFT_SIZE];
float32_t fft_buf_z[2][FFT_SIZE];
volatile uint8_t fft_fill_idx = 0;
volatile uint16_t fft_sample_count = 0;

arm_rfft_fast_instance_f32 fft_instance;

/* Output magnitude buffers, one per axis. arm_rfft_fast_f32 produces
 * FFT_SIZE complex-interleaved floats; arm_cmplx_mag_f32 collapses
 * that to FFT_SIZE/2 real magnitude bins (bin 0 = DC, bins 1..127 =
 * real frequency content up to Nyquist at our 400Hz ODR). */
float32_t fft_mag_x[FFT_SIZE / 2];
float32_t fft_mag_y[FFT_SIZE / 2];
float32_t fft_mag_z[FFT_SIZE / 2];

/* Which buffer half is ready for FFTTask to process, set by AccelTask
 * right before releasing fftDataReadySemaphore. Safe without a mutex
 * since there's exactly one producer and one consumer, and the write
 * always happens-before the semaphore release that wakes the reader. */
volatile uint8_t fft_ready_idx = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_ADC1_Init(void);
static void MX_IWDG_Init(void);
void StartAccelTask(void const * argument);
void StartDHT11Task(void const * argument);
void StartWatchdogTask(void const * argument);
void StartFFTTask(void const * argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();
  MX_ADC1_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */
  hlis.hspi    = &hspi1;
    hlis.cs_port = GPIOE;
    hlis.cs_pin  = GPIO_PIN_3;

    LIS3_Init(&hlis);                                                /* sets ODR=400Hz, all axes enabled */
    LIS3_WriteReg(&hlis, LIS3_CTRL_REG3, LIS3_CTRL_REG3_DRDY_INT1);   /* NEW: routes DRDY to INT1/PE0, without
                                                                        this the chip never pulses PE0 and
                                                                        no EXTI interrupt ever fires */
    /* Enable the DWT cycle counter. Required by delay_us() and DHT11's
         * bit-timing. Missing since Session 8 cleanup, silently hangs
         * DHT11Task forever without it, no crash, just permanently stuck. */
        DEM_CR |= (1 << 24);
        DWT_CYCCNT = 0;
        DWT_CTRL |= 1;

    /* Start ADC1 in DMA circular mode. From this point the DMA keeps
     * adc_temp_raw updated automatically on every completed conversion,
     * no further software trigger needed. HAL_ADC_Start_DMA takes the
     * buffer pointer and length (1 element here, one channel only). */
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)&adc_temp_raw, 1);

    /* One-time FFT init, computes twiddle factors etc. for this FFT size.
     * ifftFlag=0 (arg to the process call later, not here) since we only
     * ever go time->frequency, never inverse. */
    arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE);
  /* USER CODE END 2 */

  /* Create the mutex(es) */
  /* definition and creation of diagnosticsMutex */
  osMutexDef(diagnosticsMutex);
  diagnosticsMutexHandle = osMutexCreate(osMutex(diagnosticsMutex));

  /* definition and creation of printfMutex */
  osMutexDef(printfMutex);
  printfMutexHandle = osMutexCreate(osMutex(printfMutex));

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* definition and creation of uartTxSemaphore */
  osSemaphoreDef(uartTxSemaphore);
  uartTxSemaphoreHandle = osSemaphoreCreate(osSemaphore(uartTxSemaphore), 1);

  /* definition and creation of fftDataReadySemaphore */
  osSemaphoreDef(fftDataReadySemaphore);
  fftDataReadySemaphoreHandle = osSemaphoreCreate(osSemaphore(fftDataReadySemaphore), 1);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* fftDataReadySemaphore is created with 1 token available by default
   * in CMSIS_V1 regardless of the "Depleted" GUI setting, drain it here
   * so FFTTask correctly blocks until AccelTask actually signals a
   * completed window, rather than running once immediately on boot
   * with garbage/zeroed buffer data. */
  osSemaphoreWait(fftDataReadySemaphoreHandle, 0);
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of AccelTask */
  osThreadDef(AccelTask, StartAccelTask, osPriorityNormal, 0, 128);
  AccelTaskHandle = osThreadCreate(osThread(AccelTask), NULL);

  /* definition and creation of DHT11Task */
  osThreadDef(DHT11Task, StartDHT11Task, osPriorityLow, 0, 128);
  DHT11TaskHandle = osThreadCreate(osThread(DHT11Task), NULL);

  /* definition and creation of WatchdogTask */
  osThreadDef(WatchdogTask, StartWatchdogTask, osPriorityLow, 0, 128);
  WatchdogTaskHandle = osThreadCreate(osThread(WatchdogTask), NULL);

  /* definition and creation of FFTTask */
  osThreadDef(FFTTask, StartFFTTask, osPriorityBelowNormal, 0, 512);
  FFTTaskHandle = osThreadCreate(osThread(FFTTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  /* Nothing blocking here anymore. DRDY interrupt -> DMA burst read
   * runs entirely on its own; this loop just checks the flag the
   * library sets once a fresh sample has actually landed. */
	  if (LIS3_DataReady)
	  {
	      LIS3_DataReady = 0;



	      static uint16_t print_counter = 0;
	      if (++print_counter >= 40)   /* ~10 prints/sec at 400Hz ODR, readable */
	      {
	          print_counter = 0;
	          printf("X:%d Y:%d Z:%d\r\n", data.x, data.y, data.z);

	          DHT11_Data dht;
	          if (DHT11_Read(&dht))
	          {
	              printf("DHT11: %d.%d%% RH, %d.%dC\r\n",
	                     dht.humidity_int, dht.humidity_dec,
	                     dht.temp_int, dht.temp_dec);
	          }
	          else
	          {
	              printf("DHT11: read failed\r\n");
	          }

	      }
	  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Reload = 1999;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  /* DMA2_Stream4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream4_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream4_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LIS3DSH_CS_GPIO_Port, LIS3DSH_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : LIS3DSH_CS_Pin */
  GPIO_InitStruct.Pin = LIS3DSH_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LIS3DSH_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DHT11_DATA_Pin */
  GPIO_InitStruct.Pin = DHT11_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PE0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
int _write(int file, char *ptr, int len)
{
    /* Wait until any previous DMA transfer has actually finished,
     * osWaitForever is fine here since transfers complete in well
     * under a millisecond, this is never a meaningful stall. Without
     * this wait, two tasks calling printf close together could start
     * a second DMA transfer from a different buffer while the first
     * one is still mid-flight, corrupting output. */
    osSemaphoreWait(uartTxSemaphoreHandle, osWaitForever);

    HAL_UART_Transmit_DMA(&huart2, (uint8_t*)ptr, len);

    return len;
}
/* FreeRTOS calls this automatically if stack overflow checking
 * (Method 2) detects a task has overrun its allocated stack. Uses
 * direct blocking HAL_UART_Transmit, not printf: the offending
 * task's stack may already be corrupted, and printf's internal
 * formatting pushes more onto that same stack, risking further
 * corruption before the message gets out. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    char msg[64];
    int len = 0;
    const char *prefix = "!!! STACK OVERFLOW in task: ";
    while (prefix[len]) { msg[len] = prefix[len]; len++; }
    int i = 0;
    while (pcTaskName[i] && len < 60) { msg[len++] = pcTaskName[i++]; }
    msg[len++] = '\r';
    msg[len++] = '\n';

    HAL_UART_Transmit(&huart2, (uint8_t*)msg, len, HAL_MAX_DELAY);

    while(1) { }
}
/* HAL calls this automatically once a UART DMA transmit completes.
 * Releasing the semaphore here, not inside _write() itself, is what
 * makes this safe across tasks, the next printf can't start a new
 * transfer until the hardware confirms the previous one is done. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        osSemaphoreRelease(uartTxSemaphoreHandle);
    }
}

/* Fires on every DRDY rising edge from the LIS3DSH, i.e. every new
 * sample at 400Hz. Kicks off the DMA burst read; the rest of the
 * sequence (Tx complete -> Rx complete) continues in the two
 * callbacks below, this function does not wait for any of it. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
    {
        LIS3_StartBurstRead_DMA(&hlis, &data);
    }
}

/* HAL calls this automatically once the 1-byte address transmit (started
 * inside LIS3_StartBurstRead_DMA) completes. Library handler starts the
 * matching 6-byte receive. */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    LIS3_DMA_TxCpltHandler();
}

/* HAL calls this once the 6 data bytes have landed. Library handler
 * raises CS, reconstructs X/Y/Z, and sets LIS3_DataReady for the main
 * loop to pick up. */
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    LIS3_DMA_RxCpltHandler();
}

/* Busy-wait for a precise number of microseconds, using the DWT cycle
 * counter. SystemCoreClock is 168000000 (168MHz) in this project, so
 * cycles-per-microsecond = 168. Unsigned subtraction handles the
 * counter wrapping the same way the timer capture did earlier. */
void delay_us(uint32_t us)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000);
    while ((DWT_CYCCNT - start) < cycles) { }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartAccelTask */
/**
  * @brief  Function implementing the AccelTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartAccelTask */
void StartAccelTask(void const * argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
	  /* Nothing blocking here anymore. DRDY interrupt -> DMA burst read
	    * runs entirely on its own; this loop just checks the flag the
	    * library sets once a fresh sample has actually landed. */
	  if (LIS3_DataReady)
	  {
	      LIS3_DataReady = 0;

	      /* Append this sample into the currently-filling half of each
	                 * axis's ping-pong buffer. */
	                fft_buf_x[fft_fill_idx][fft_sample_count] = (float32_t)data.x;
	                fft_buf_y[fft_fill_idx][fft_sample_count] = (float32_t)data.y;
	                fft_buf_z[fft_fill_idx][fft_sample_count] = (float32_t)data.z;
	                fft_sample_count++;

	                if (fft_sample_count >= FFT_SIZE)
	                {
	                    /* Window complete. Record which half is ready, wake FFTTask,
	                     * then swap to the other half and reset the counter so
	                     * accumulation continues without missing samples while
	                     * FFTTask processes the completed one. */
	                    fft_ready_idx = fft_fill_idx;
	                    fft_fill_idx = 1 - fft_fill_idx;
	                    fft_sample_count = 0;

	                    osSemaphoreRelease(fftDataReadySemaphoreHandle);
	                }

	      static uint16_t print_counter = 0;
	      if (++print_counter >= 40)
	      {
	          print_counter = 0;
	          /* Hold the mutex only as long as it takes to write three
	           * int16_t values, a handful of microseconds, then release
	           * immediately. Never hold a mutex across something slow like
	           * a printf call, that would needlessly block DHT11Task or
	           * anything else waiting on the same lock. */
	          osMutexWait(diagnosticsMutexHandle, osWaitForever);
	          diagnostics.accel_x = data.x;
	          diagnostics.accel_y = data.y;
	          diagnostics.accel_z = data.z;
	          osMutexRelease(diagnosticsMutexHandle);

	          osMutexWait(printfMutexHandle, osWaitForever);
	          printf("X:%d Y:%d Z:%d\r\n", data.x, data.y, data.z);
	          osMutexRelease(printfMutexHandle);
	      }
	  }
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartDHT11Task */
/**
* @brief Function implementing the DHT11Task thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartDHT11Task */
void StartDHT11Task(void const * argument)
{
  /* USER CODE BEGIN StartDHT11Task */
  /* Infinite loop */
  for(;;)
  {
	  DHT11_Data dht;
		  if (DHT11_Read(&dht))
		  {
		      osMutexWait(diagnosticsMutexHandle, osWaitForever);
		      diagnostics.humidity_int = dht.humidity_int;
		      diagnostics.humidity_dec = dht.humidity_dec;
		      diagnostics.temp_int     = dht.temp_int;
		      diagnostics.temp_dec     = dht.temp_dec;
		      osMutexRelease(diagnosticsMutexHandle);

		      osMutexWait(printfMutexHandle, osWaitForever);
		      printf("DHT11: %d.%d%% RH, %d.%dC\r\n",
		             dht.humidity_int, dht.humidity_dec,
		             dht.temp_int, dht.temp_dec);
		      osMutexRelease(printfMutexHandle);
		  }
		  else
		  {
		      osMutexWait(printfMutexHandle, osWaitForever);
		      printf("DHT11: read failed\r\n");
		      osMutexRelease(printfMutexHandle);
		  }
		      osDelay(3000);  /* temperature has no urgency, 3s between reads is plenty,
		                        * and gives the bus a clean recovery window between*/
  }
  /* USER CODE END StartDHT11Task */
}

/* USER CODE BEGIN Header_StartWatchdogTask */
/**
* @brief Function implementing the WatchdogTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartWatchdogTask */
void StartWatchdogTask(void const * argument)
{
  /* USER CODE BEGIN StartWatchdogTask */

	  /* Reset-cause check using DIRECT blocking UART, not printf, so this
	     * cannot touch any mutex/semaphore and cannot deadlock. Confirming
	     * whether this pre-FFT baseline resets at all, same instrumentation
	     * used to isolate the FFT-branch regression earlier today. */
	    char msg[80];
	    int len = 0;
	    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))
	        len = sprintf(msg, "RESET: IWDG\r\n");
	    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST))
	        len = sprintf(msg, "RESET: LOW POWER / BROWNOUT\r\n");
	    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))
	        len = sprintf(msg, "RESET: POWER-ON\r\n");
	    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))
	        len = sprintf(msg, "RESET: PIN/NRST\r\n");
	    else
	        len = sprintf(msg, "RESET: OTHER/UNKNOWN\r\n");
	    HAL_UART_Transmit(&huart2, (uint8_t*)msg, len, HAL_MAX_DELAY);
	    __HAL_RCC_CLEAR_RESET_FLAGS();

	    /* Infinite loop */
	    for(;;)
	    {
	        /* Refresh every 500ms, comfortably under the 2-second IWDG
	         * window. This task only ever runs if the scheduler is still
	         * actually switching between tasks, if any task hangs hard
	         * enough to starve the scheduler entirely, this refresh stops
	         * happening and IWDG forces a full chip reset within 2 seconds. */
	        HAL_IWDG_Refresh(&hiwdg);
	        osDelay(500);
	    }

  /* USER CODE END StartWatchdogTask */
}

/* USER CODE BEGIN Header_StartFFTTask */
/**
* @brief Function implementing the FFTTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartFFTTask */
void StartFFTTask(void const * argument)
{
  /* USER CODE BEGIN StartFFTTask */
  /* Infinite loop */
  for(;;)
  {
	  /* Block here until AccelTask signals a completed 256-sample
	         * window. osWaitForever is correct, this task has nothing
	         * useful to do until real data exists, no reason to poll. */
	        osSemaphoreWait(fftDataReadySemaphoreHandle, osWaitForever);

	        uint8_t idx = fft_ready_idx;  /* snapshot which half is safe to read */

	        /* arm_rfft_fast_f32 processes one real-valued array at a time,
	         * there's no 3-axis variant, so run it three times sequentially.
	         * ifftFlag=0 means forward transform (time->frequency). Output
	         * is FFT_SIZE floats in complex-interleaved format:
	         * [re0, im0, re1, im1, ...], NOT FFT_SIZE/2 complex pairs plus
	         * a separate DC/Nyquist pair, arm_rfft_fast_f32 packs this
	         * slightly differently than a naive complex FFT would, but
	         * arm_cmplx_mag_f32 below handles the interleaved format
	         * correctly regardless. */
	        static float32_t fft_out_x[FFT_SIZE];
	        static float32_t fft_out_y[FFT_SIZE];
	        static float32_t fft_out_z[FFT_SIZE];

	        arm_rfft_fast_f32(&fft_instance, fft_buf_x[idx], fft_out_x, 0);
	        arm_rfft_fast_f32(&fft_instance, fft_buf_y[idx], fft_out_y, 0);
	        arm_rfft_fast_f32(&fft_instance, fft_buf_z[idx], fft_out_z, 0);

	        /* Collapse complex output to real magnitude, FFT_SIZE/2 bins
	         * per axis. Bin 0 = DC/gravity offset, bins 1..127 = real
	         * frequency content, bin_hz = 400Hz / 256 = 1.5625Hz per bin. */
	        arm_cmplx_mag_f32(fft_out_x, fft_mag_x, FFT_SIZE / 2);
	        arm_cmplx_mag_f32(fft_out_y, fft_mag_y, FFT_SIZE / 2);
	        arm_cmplx_mag_f32(fft_out_z, fft_mag_z, FFT_SIZE / 2);

	        /* Find peak bin per axis, skipping bin 0 (DC/gravity), same
	         * approach validated in the mini-project bring-up. */
	        float32_t bin_hz = 400.0f / FFT_SIZE;

	        float32_t peak_x = 0, peak_y = 0, peak_z = 0;
	        uint16_t peak_bin_x = 1, peak_bin_y = 1, peak_bin_z = 1;

	        for (int i = 1; i < FFT_SIZE / 2; i++)
	        {
	            if (fft_mag_x[i] > peak_x) { peak_x = fft_mag_x[i]; peak_bin_x = i; }
	            if (fft_mag_y[i] > peak_y) { peak_y = fft_mag_y[i]; peak_bin_y = i; }
	            if (fft_mag_z[i] > peak_z) { peak_z = fft_mag_z[i]; peak_bin_z = i; }
	        }

	        printf("FFT X: %.2fHz (%.1f)  Y: %.2fHz (%.1f)  Z: %.2fHz (%.1f)\r\n",
	               peak_bin_x * bin_hz, peak_x,
	               peak_bin_y * bin_hz, peak_y,
	               peak_bin_z * bin_hz, peak_z);
    osDelay(1);
  }
  /* USER CODE END StartFFTTask */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
