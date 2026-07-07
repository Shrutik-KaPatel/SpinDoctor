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
osThreadId CaptureTaskHandle;
osMutexId diagnosticsMutexHandle;
osMutexId printfMutexHandle;
osSemaphoreId uartTxSemaphoreHandle;
osSemaphoreId captureDataReadyHandle;
osSemaphoreId captureRxByteReadyHandle;
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

#define CAPTURE_BUF_SIZE 64   /* samples per axis per buffer, matches
                                 * original FFT window sizing rationale:
                                 * enough periods per buffer at expected
                                 * fan RPM, arbitrary otherwise for raw
                                 * capture, NanoEdge does its own windowing */
#define CAPTURE_WINDOW_COUNT 156   /* ~25s at 400Hz / 256-sample windows,
                                    * matches ST's 20-30s capture guidance
                                    * from earlier NanoEdge format research */

int16_t capture_buf_x[2][CAPTURE_BUF_SIZE];
int16_t capture_buf_y[2][CAPTURE_BUF_SIZE];
int16_t capture_buf_z[2][CAPTURE_BUF_SIZE];
volatile uint8_t capture_fill_idx = 0;
volatile uint16_t capture_sample_count = 0;
volatile uint8_t capture_ready_idx = 0;

/* Set true only while an active capture window is streaming; AccelTask
 * checks this before bothering to fill the capture buffers or release
 * the semaphore, so there's zero extra work happening between capture
 * sessions. */
volatile uint8_t capture_active = 0;


uint8_t uart_rx_byte;
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
void StartCaptureTask(void const * argument);

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

    /* Start interrupt-driven UART receive for the capture menu. Blocking/
     * polling-mode HAL_UART_Receive doesn't coexist safely with DMA-based
     * TX already active on this same UART (used by printf), it can return
     * immediately with a busy/error state instead of genuinely blocking.
     * Interrupt-driven RX avoids that conflict entirely. */
    HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);

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

  /* definition and creation of captureDataReady */
  osSemaphoreDef(captureDataReady);
  captureDataReadyHandle = osSemaphoreCreate(osSemaphore(captureDataReady), 1);

  /* definition and creation of captureRxByteReady */
  osSemaphoreDef(captureRxByteReady);
  captureRxByteReadyHandle = osSemaphoreCreate(osSemaphore(captureRxByteReady), 1);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* captureDataReadyHandle is created with 1 token by default in
   * CMSIS_V1 regardless of the "Available" GUI setting, drain it here
   * so CaptureTask correctly blocks until AccelTask actually signals a
   * completed window, rather than running once immediately on boot. */
  osSemaphoreWait(captureDataReadyHandle, 0);

  osSemaphoreWait(captureRxByteReadyHandle, 0);
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

  /* definition and creation of CaptureTask */
  osThreadDef(CaptureTask, StartCaptureTask, osPriorityBelowNormal, 0, 512);
  CaptureTaskHandle = osThreadCreate(osThread(CaptureTask), NULL);

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
  huart2.Init.BaudRate = 460800;
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

/* HAL calls this automatically once one byte has been received via
 * HAL_UART_Receive_IT. Releases the semaphore CaptureTask blocks on,
 * then immediately re-arms itself to listen for the next byte. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        osSemaphoreRelease(captureRxByteReadyHandle);
        HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);
    }
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
	 for(;;)
	  {
	      if (LIS3_DataReady)
	      {
	          LIS3_DataReady = 0;

	          /* Capture buffer fill happens on EVERY sample, unconditional
	           * on print_counter, since the capture buffer needs the real
	           * 400Hz stream, not a throttled/sparse subset. Gated only by
	           * capture_active so this costs nothing between capture
	           * sessions. */
	          if (capture_active)
	          {

	        	  capture_buf_x[capture_fill_idx][capture_sample_count] = data.x;
 	              capture_buf_y[capture_fill_idx][capture_sample_count] = data.y;
 	              capture_buf_z[capture_fill_idx][capture_sample_count] = data.z;
 	              capture_sample_count++;

	              if (capture_sample_count >= CAPTURE_BUF_SIZE)
	              {
	                  capture_ready_idx = capture_fill_idx;
	                  capture_fill_idx = 1 - capture_fill_idx;
	                  capture_sample_count = 0;
	                  osSemaphoreRelease(captureDataReadyHandle);
	              }
	          }
	          else
	          {
	              /* Normal throttled diagnostic print, only when NOT
	               * capturing, keeps the UART stream clean and parseable
	               * during an active capture window. */
	              static uint16_t print_counter = 0;
	              if (++print_counter >= 40)
	              {
	                  print_counter = 0;

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
	  if (!capture_active)
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
	      }
	      osDelay(3000);
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

/* USER CODE BEGIN Header_StartCaptureTask */
/**
* @brief Function implementing the CaptureTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartCaptureTask */
void StartCaptureTask(void const * argument)
{
  /* USER CODE BEGIN StartCaptureTask */
	 uint8_t rx_byte;
	  char speed_char = 0;
	  char class_char = 0;

	  for(;;)
	  {
	      /* --- Menu: prompt for speed + class --- */
		  osMutexWait(printfMutexHandle, osWaitForever);
		  printf("\r\n=== SpinDoctor Capture Menu ===\r\n");
		  printf("Speed:  1) Low  2) Medium  3) High\r\n");
		  printf("Class:  H) Healthy  I) Imbalance  O) Obstruction\r\n");
		  printf("Enter as two chars, e.g. \"1H\": ");
		  osMutexRelease(printfMutexHandle);

	      /* Blocking receive, one byte at a time. This task has nothing
	       * else to do while waiting for menu input, so a simple blocking
	       * HAL_UART_Receive call here is fine, no interrupt/queue needed. */
		  osSemaphoreWait(captureRxByteReadyHandle, osWaitForever);
		  rx_byte = uart_rx_byte;
	      speed_char = (char)rx_byte;
	      osSemaphoreWait(captureRxByteReadyHandle, osWaitForever);
	      rx_byte = uart_rx_byte;
	      class_char = (char)rx_byte;

	      uint8_t valid_speed = (speed_char == '1' || speed_char == '2' || speed_char == '3');
	      uint8_t valid_class = (class_char == 'H' || class_char == 'I' || class_char == 'O' ||
	                              class_char == 'h' || class_char == 'i' || class_char == 'o');

	      if (!valid_speed || !valid_class)
	      {
	          osMutexWait(printfMutexHandle, osWaitForever);
	          printf("\r\nInvalid input, try again.\r\n");
	          osMutexRelease(printfMutexHandle);
	          continue;
	      }

	      const char *class_name;
	      switch (class_char)
	      {
	          case 'H': case 'h': class_name = "healthy";    break;
	          case 'I': case 'i': class_name = "imbalance";  break;
	          default:            class_name = "obstruction"; break;
	      }
	      char label[32];
	      sprintf(label, "speed%c_%s", speed_char, class_name);

	      osMutexWait(printfMutexHandle, osWaitForever);
	      printf("\r\nSelected: Speed %c, %s. Set up the fan now.\r\n", speed_char, class_name);
	      printf("Send 'S' when stable and ready to capture, 'X' to cancel and return to menu.\r\n");
	      osMutexRelease(printfMutexHandle);

	      char cmd;
	      do
	      {
	    	  osSemaphoreWait(captureRxByteReadyHandle, osWaitForever);
	    	     cmd = (char)uart_rx_byte;
	      } while (cmd != 'S' && cmd != 's' && cmd != 'X' && cmd != 'x');

	      if (cmd == 'X' || cmd == 'x')
	      {
	          osMutexWait(printfMutexHandle, osWaitForever);
	          printf("\r\nCancelled, returning to menu.\r\n");
	          osMutexRelease(printfMutexHandle);
	          continue;
	      }

	      /* --- Active capture --- */
	      osMutexWait(printfMutexHandle, osWaitForever);
	      printf("\r\n=== CAPTURE START: %s ===\r\n", label);
	      osMutexRelease(printfMutexHandle);

	      osSemaphoreWait(captureDataReadyHandle, 0);
	      capture_fill_idx = 0;
	      capture_sample_count = 0;
	      capture_active = 1;

	      for (int w = 0; w < CAPTURE_WINDOW_COUNT; w++)
	      {
	          osSemaphoreWait(captureDataReadyHandle, osWaitForever);
	          uint8_t idx = capture_ready_idx;

	          static char row_buf[4][6000];
	                   static uint8_t row_buf_idx = 0;

	                   int pos = 0;
	                   for (int i = 0; i < CAPTURE_BUF_SIZE; i++)
	                   {
	                       pos += sprintf(row_buf[row_buf_idx] + pos, "%d,%d,%d",
	                                      capture_buf_x[idx][i],
	                                      capture_buf_y[idx][i],
	                                      capture_buf_z[idx][i]);
	                       if (i < CAPTURE_BUF_SIZE - 1) row_buf[row_buf_idx][pos++] = ',';
	                   }
	                   row_buf[row_buf_idx][pos++] = '\r';
	                   row_buf[row_buf_idx][pos++] = '\n';
	                   row_buf[row_buf_idx][pos] = '\0';

	                   uint32_t t_start = DWT_CYCCNT;

	                             osMutexWait(printfMutexHandle, osWaitForever);
	                             printf("%s", row_buf[row_buf_idx]);
	                             osMutexRelease(printfMutexHandle);

	                             uint32_t t_elapsed_us = (DWT_CYCCNT - t_start) / (SystemCoreClock / 1000000);

	                             static uint16_t dbg_counter = 0;
	                             if (++dbg_counter >= 20)
	                             {
	                                 dbg_counter = 0;
	                                 osMutexWait(printfMutexHandle, osWaitForever);
	                                 printf("[row took %luus]\r\n", (unsigned long)t_elapsed_us);
	                                 osMutexRelease(printfMutexHandle);
	                             }

	                             row_buf_idx = (row_buf_idx + 1) % 4;
	      }

	      capture_active = 0;

	      osMutexWait(printfMutexHandle, osWaitForever);
	      printf("=== CAPTURE END: %s ===\r\n", label);
	      osMutexRelease(printfMutexHandle);
	  }
  /* USER CODE END StartCaptureTask */
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
