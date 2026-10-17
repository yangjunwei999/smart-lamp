/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    : main.c
 * @brief   : 屿光民宿智能氛围床头灯 - 主程序
 *
 * 需求实现一览（对应甲方订单 6 条）：
 *   1. 仪式感点亮   : 上电后 0.4s 静默 -> 2.5s 缓入渐亮至 30% 暖金 -> 双脉冲收尾
 *   2. 多种灯光模式 : 常亮暖光 / 柔和呼吸 / 流光幻彩 / 深夜微光，KEY1 短按循环切换
 *   3. 深夜模式     : 固定约 9% 占空比深琥珀微光，盯看 10 秒不晃眼
 *   4. 温柔唤醒     : KEY1 长按 1.2s 模拟"早 7 点"，30 秒由 2% 深琥珀缓亮至 60% 暖白
 *   5. 火警警示     : KEY2 一键触发/解除，红灯 2Hz 快闪 + 蜂鸣器 2kHz 报警，优先级最高
 *   6. 按键一次成功 : 20ms 消抖 + 事件边沿化，一次按压只产生一次动作
 *
 * 硬件（STM32F103C8T6 开发板套件）：
 *   PA6 -> TIM3_CH1 -> RGB 灯 R      PB12 -> KEY1（低电平有效，内部上拉）
 *   PA7 -> TIM3_CH2 -> RGB 灯 G      PB13 -> KEY2（低电平有效，内部上拉）
 *   PB0 -> TIM3_CH3 -> RGB 灯 B      PB9  -> TIM4_CH4 -> 无源蜂鸣器
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/*---------------------- 用户配置区 ----------------------*/
#define PWM_MAX            99U   /* TIM3 ARR=99，CCR 0-99 即 0%-100% 占空比 */

/* 按键参数 */
#define KEY_DEBOUNCE_MS    20U   /* 20ms 电平稳定才确认，抖动一律忽略 */
#define KEY_LONGPRESS_MS   1200U /* 按住 1.2s 判定为长按 */

/* 仪式感点亮时序 */
#define RITUAL_DARK_MS     400U  /* 静默酝酿 */
#define RITUAL_FADE_MS     2500U /* 0 -> 30% 缓入渐亮 */
#define RITUAL_GLOW_MS     1000U /* 收尾"双脉冲"呼吸 */

/* 温柔唤醒时序 */
#define WAKE_TOTAL_MS      30000U /* 30 秒渐亮 */
#define WAKE_CHIRP_MS      10000U /* 最后 10 秒轻声鸣叫 */
#define WAKE_CHIRP_GAP_MS  2000U  /* 鸣叫间隔 */
#define WAKE_CHIRP_LEN_MS  60U    /* 单次鸣叫时长 */

/* 火警闪烁节拍 */
#define FIRE_HALF_MS       250U  /* 亮 250ms / 灭 250ms => 2Hz 快闪 */

/* 灯珠极性：0=共阴极（默认）；若自检发现灯亮在第三段（占空比全关那2秒），改 1 重烧 */
#define LED_INVERT         0
/*---------------------- 用户配置区 END ------------------*/
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* 灯光模式 */
typedef enum {
  MODE_WARM = 0,  /* 常亮暖光（氛围常亮） */
  MODE_BREATH,    /* 柔和呼吸            */
  MODE_FLOW,      /* 流光幻彩（色环轮转） */
  MODE_NIGHT,     /* 深夜微光            */
  MODE_NUM
} ModeType;

/* 顶层状态（火警/唤醒拥有独立最高优先级） */
typedef enum {
  ST_RITUAL = 0,  /* 仪式感点亮（仅上电一次） */
  ST_RUN,         /* 正常模式运行             */
  ST_WAKE,        /* 温柔唤醒                 */
  ST_FIRE         /* 火警紧急警示             */
} StateType;

/* 按键事件（一次按压只产生一个事件 = 需求6） */
typedef enum {
  KEVT_NONE = 0,
  KEVT_DOWN,   /* 确认按下（消抖后） */
  KEVT_SHORT,  /* 短按（松手时产生） */
  KEVT_LONG    /* 长按（按住达标时产生，松手不再重复） */
} KeyEvent;

/* 按键扫描状态 */
typedef struct {
  uint8_t  rawLast;     /* 上次原始电平 */
  uint8_t  stable;      /* 消抖后稳定电平：0=按下 1=松开 */
  uint32_t lastChange;  /* 原始电平最近变化时刻 */
  uint32_t pressStart;  /* 本次按下起始时刻 */
  uint8_t  longFired;   /* 长按事件已触发标记 */
} KeyState;

static KeyState key1, key2;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void     Lamp_SetRGB(uint8_t r, uint8_t g, uint8_t b);
static void     Lamp_Warm(uint8_t pct);
static void     Lamp_Hue(uint16_t hue, uint8_t val);
static void     Buzzer_Set(uint16_t freqHz, uint8_t dutyPct);
static void     Buzzer_Off(void);
static KeyEvent Key_Scan(KeyState *k, GPIO_TypeDef *port, uint16_t pin);
static void     Key_Init(KeyState *k);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief  直接设置 RGB 三路 PWM 占空比（0-99）
 */
static void Lamp_SetRGB(uint8_t r, uint8_t g, uint8_t b)
{
#if LED_INVERT
  /* 共阳极灯珠：占空比语义取反 */
  r = (uint8_t)(PWM_MAX - r);
  g = (uint8_t)(PWM_MAX - g);
  b = (uint8_t)(PWM_MAX - b);
#endif
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, r);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, g);
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_3, b);
}

/**
 * @brief  暖色光：pct% 亮度的暖琥珀（色温配比 R:G:B = 100:55:8）
 */
static void Lamp_Warm(uint8_t pct)
{
  if (pct > 100U) { pct = 100U; }
  Lamp_SetRGB((uint8_t)((uint32_t)PWM_MAX * pct / 100U),
              (uint8_t)((uint32_t)PWM_MAX * pct * 55U / 10000U),
              (uint8_t)((uint32_t)PWM_MAX * pct * 8U / 10000U));
}

/**
 * @brief  HSV 色环取色（S=100%），用于流光幻彩模式
 * @param  hue 0-359 度；val 明度 0-100
 */
static void Lamp_Hue(uint16_t hue, uint8_t val)
{
  uint16_t h = (uint16_t)(hue % 360U);
  uint8_t  region = (uint8_t)(h / 60U);
  uint16_t rem = (uint16_t)((h % 60U) * PWM_MAX / 60U); /* 0-99 */
  uint8_t p = (uint8_t)((uint32_t)PWM_MAX * val / 100U);
  uint8_t q = (uint8_t)((uint32_t)(PWM_MAX - rem) * val / 100U);
  uint8_t t = (uint8_t)((uint32_t)rem * val / 100U);
  uint8_t r = 0, g = 0, b = 0;

  switch (region) {
    case 0: r = p; g = t; b = 0; break;
    case 1: r = q; g = p; b = 0; break;
    case 2: r = 0; g = p; b = t; break;
    case 3: r = 0; g = q; b = p; break;
    case 4: r = t; g = 0; b = p; break;
    default: r = p; g = 0; b = q; break;
  }
  Lamp_SetRGB(r, g, b);
}

/**
 * @brief  无源蜂鸣器发声：freqHz 频率，dutyPct 占空比（越小越柔和）
 */
static void Buzzer_Set(uint16_t freqHz, uint8_t dutyPct)
{
  uint32_t arr;
  if (freqHz == 0U) { Buzzer_Off(); return; }
  arr = 1000000U / freqHz;                 /* TIM4 计数时钟 = 72MHz/72 = 1MHz */
  if (arr > 0U) { __HAL_TIM_SET_AUTORELOAD(&htim4, arr - 1U); }
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_4, arr * dutyPct / 100U);
}

static void Buzzer_Off(void)
{
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_4, 0U);
}

/**
 * @brief  按键初始化状态
 */
static void Key_Init(KeyState *k)
{
  k->rawLast = 1U;
  k->stable = 1U;
  k->lastChange = HAL_GetTick();
  k->pressStart = 0U;
  k->longFired = 0U;
}

/**
 * @brief  按键扫描（每轮主循环调用，非阻塞）
 *         消抖策略（需求6）：原始电平需稳定保持 20ms 才被接受；
 *         事件边沿化：短按在"松手"时产生、长按在"按住 1.2s"时产生一次，
 *         因此一次物理按压永远只产生一个动作，绝不"按一次响应两次"。
 */
static KeyEvent Key_Scan(KeyState *k, GPIO_TypeDef *port, uint16_t pin)
{
  uint8_t raw = (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET) ? 0U : 1U;
  uint32_t now = HAL_GetTick();

  /* 原始电平刚变化：记录时刻，等待稳定 */
  if (raw != k->rawLast) {
    k->rawLast = raw;
    k->lastChange = now;
    return KEVT_NONE;
  }

  /* 稳定时间达标才翻转状态 */
  if ((raw != k->stable) && ((now - k->lastChange) >= KEY_DEBOUNCE_MS)) {
    k->stable = raw;
    if (raw == 0U) {                 /* 确认按下 */
      k->pressStart = now;
      k->longFired = 0U;
      return KEVT_DOWN;
    }
    return (k->longFired != 0U) ? KEVT_NONE : KEVT_SHORT; /* 松手出短按 */
  }

  /* 按住达标：长按只触发一次 */
  if ((k->stable == 0U) && (k->longFired == 0U) &&
      ((now - k->pressStart) >= KEY_LONGPRESS_MS)) {
    k->longFired = 1U;
    return KEVT_LONG;
  }
  return KEVT_NONE;
}

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
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  MX_TIM4_Init();                              /* 蜂鸣器定时器（手动扩展） */

  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);    /* RGB 三路 PWM 输出 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4);    /* 蜂鸣器 PWM 输出 */
  Buzzer_Off();

  /*========== 上电自检（约 7 秒）：先证明固件活着，再暴露灯珠极性 ==========
    段1 (3s)：RGB 2Hz 闪烁 + 蜂鸣叫 —— 只要接了线，任何极性都一定闪/响
    段2 (2s)：占空比全开 99     段3 (2s)：占空比全关 0
    判据：灯亮在段2 = 共阴极（现状即可用）；亮在段3 = 共阳极（LED_INVERT 改 1 重烧） */
  {
    uint8_t bi;
    for (bi = 0; bi < 6U; bi++) {
      Lamp_SetRGB(99U, 99U, 99U);
      Buzzer_Set(2000U, 20U);
      HAL_Delay(250);
      Lamp_SetRGB(0U, 0U, 0U);
      Buzzer_Off();
      HAL_Delay(250);
    }
    Lamp_SetRGB(99U, 99U, 99U);
    HAL_Delay(2000);
    Lamp_SetRGB(0U, 0U, 0U);
    HAL_Delay(2000);
  }

  /*---------------- 应用状态变量 ----------------*/
  StateType state = ST_RITUAL;
  ModeType  mode = MODE_WARM;        /* 当前灯光模式 */
  ModeType  modeBeforeWake = MODE_WARM; /* 唤醒前的模式（唤醒结束后可恢复） */
  uint8_t   warmLevel = 30U;         /* 常亮暖光亮度（%） */
  uint32_t  effectStart = HAL_GetTick(); /* 当前光效起始时刻（呼吸/流光/唤醒/火警复用） */
  uint8_t   breathPos = 0U;          /* 呼吸相位 0-99 */
  int8_t    breathDir = 1;           /* 呼吸方向 */
  uint32_t  lastTick = HAL_GetTick();    /* 10ms 光效节拍 */
  uint32_t  lastFire = HAL_GetTick();    /* 火警闪烁节拍 */
  uint32_t  nextChirp = 0U;          /* 唤醒鸣叫时刻表 */
  uint32_t  chirpUntil = 0U;         /* 鸣叫截止时刻 */
  (void)modeBeforeWake;

  Key_Init(&key1);
  Key_Init(&key2);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint32_t now = HAL_GetTick();
    KeyEvent e1 = Key_Scan(&key1, KEY1_GPIO_Port, KEY1_Pin);
    KeyEvent e2 = Key_Scan(&key2, KEY2_GPIO_Port, KEY2_Pin);

    /*============== 需求5：火警一键触发/解除（优先级最高）==============*/
    if (e2 == KEVT_DOWN) {
      if (state == ST_FIRE) {          /* KEY2 再按：解除火警，恢复原模式 */
        Buzzer_Off();
        state = ST_RUN;
        effectStart = now;
      } else {                          /* KEY2 按下：立即进入火警警示 */
        state = ST_FIRE;
        lastFire = now;
        Buzzer_Off();
      }
    }

    /*============== KEY1：模式切换 / 唤醒（火警时屏蔽）==============*/
    if (state != ST_FIRE) {
      if (e1 == KEVT_LONG) {
        if (state == ST_WAKE) {         /* 唤醒中长按：取消唤醒 */
          Buzzer_Off();
          state = ST_RUN;
          Lamp_Warm(warmLevel);
        } else {                        /* 长按 1.2s：模拟"早 7 点"温柔唤醒 */
          modeBeforeWake = mode;
          state = ST_WAKE;
          effectStart = now;
          nextChirp = now + (WAKE_TOTAL_MS - WAKE_CHIRP_MS);
          chirpUntil = 0U;
        }
      } else if (e1 == KEVT_SHORT) {
        if (state == ST_RITUAL) {       /* 仪式点亮中短按：直接进入常亮 */
          state = ST_RUN;
          mode = MODE_WARM;
          warmLevel = 30U;
          Lamp_Warm(warmLevel);
        } else if (state == ST_WAKE) {  /* 唤醒中短按：跳过，直接常亮 */
          Buzzer_Off();
          state = ST_RUN;
          warmLevel = 60U;
          mode = MODE_WARM;
          Lamp_Warm(warmLevel);
        } else {                        /* 正常状态：循环切换 4 种模式（需求2）*/
          mode = (ModeType)((mode + 1U) % MODE_NUM);
          effectStart = now;
          breathPos = 0U;
          breathDir = 1;
          if (mode == MODE_NIGHT) {
            Lamp_SetRGB(9U, 5U, 1U);    /* 需求3：深夜微光 */
          } else if (mode == MODE_WARM) {
            Lamp_Warm(warmLevel);
          }
        }
      }
    }

    /*============== 10ms 节拍：更新光效 ==============*/
    if ((now - lastTick) >= 10U) {
      lastTick = now;

      switch (state) {

        /*------ 需求1：仪式感点亮 ------*/
        case ST_RITUAL: {
          uint32_t t = now - effectStart;
          if (t < RITUAL_DARK_MS) {
            Lamp_SetRGB(0U, 0U, 0U);                       /* 0-0.4s：静默酝酿 */
          } else if (t < RITUAL_DARK_MS + RITUAL_FADE_MS) {
            /* 2.5s 缓入渐亮 0 -> 30%（二次缓动：慢起 -> 饱和）*/
            uint32_t u = (t - RITUAL_DARK_MS) * 255U / RITUAL_FADE_MS; /* 0-255 */
            uint32_t e = (u * (510U - u)) >> 8;                         /* 缓动 0-255 */
            Lamp_Warm((uint8_t)(30U * e / 255U));
          } else if (t < RITUAL_DARK_MS + RITUAL_FADE_MS + RITUAL_GLOW_MS) {
            /* 收尾双脉冲：30% -> 60% -> 30% -> 55% -> 30%，像一次深呼吸（加强版） */
            uint32_t p = t - RITUAL_DARK_MS - RITUAL_FADE_MS; /* 0-1000ms */
            uint8_t lv;
            if (p < 250U)       { lv = (uint8_t)(30U + 30U * p / 250U); }
            else if (p < 500U)  { lv = (uint8_t)(60U - 30U * (p - 250U) / 250U); }
            else if (p < 700U)  { lv = (uint8_t)(30U + 25U * (p - 500U) / 200U); }
            else                { lv = (uint8_t)(55U - 25U * (p - 700U) / 300U); }
            Lamp_Warm(lv);
          } else {
            state = ST_RUN;            /* 点亮完成，进入常亮暖光 */
            mode = MODE_WARM;
            warmLevel = 30U;
            Lamp_Warm(warmLevel);
          }
          break;
        }

        /*------ 4 种灯光模式（需求2/3）------*/
        case ST_RUN:
          switch (mode) {
            case MODE_WARM:
              Lamp_Warm(warmLevel);    /* 常亮暖光 */
              break;

            case MODE_BREATH:
              /* 柔和呼吸：CCR 每 10ms 步进 +/-1，0->99->0，周期约 2 秒 */
              if (breathDir > 0) {
                if (++breathPos >= 99U) { breathDir = -1; }
              } else {
                if (--breathPos == 0U) { breathDir = 1; }
              }
              Lamp_Warm(breathPos);    /* 0-99% 暖色呼吸 */
              break;

            case MODE_FLOW:
              /* 流光幻彩：8 秒轮转一整圈色环 */
              Lamp_Hue((uint16_t)(((now - effectStart) * 9U / 200U) % 360U), 80U);
              break;

            case MODE_NIGHT:
            default:
              Lamp_SetRGB(9U, 5U, 1U); /* 深夜微光：约 9% 占空比深琥珀 */
              break;
          }
          break;

        /*------ 需求4：温柔唤醒（30 秒渐亮）------*/
        case ST_WAKE: {
          uint32_t t = now - effectStart;
          if (t >= WAKE_TOTAL_MS) {
            Buzzer_Off();
            warmLevel = 60U;
            mode = MODE_WARM;
            state = ST_RUN;
            Lamp_Warm(warmLevel);
          } else {
            /* 亮度：2% -> 60%，缓入曲线（慢起，不惊醒）*/
            uint32_t p = t * 255U / WAKE_TOTAL_MS;      /* 0-255 */
            uint32_t e = (p * p) / 255U;                /* 缓入 0-255 */
            uint8_t  pct = (uint8_t)(2U + 58U * e / 255U);
            /* 色温：深琥珀(R:G:B=100:30:0) -> 暖白(100:55:8) */
            uint8_t  gRatio = (uint8_t)(30U + 25U * p / 255U);
            uint8_t  bRatio = (uint8_t)(8U * p / 255U);
            Lamp_SetRGB((uint8_t)((uint32_t)PWM_MAX * pct / 100U),
                        (uint8_t)((uint32_t)PWM_MAX * pct * gRatio / 10000U),
                        (uint8_t)((uint32_t)PWM_MAX * pct * bRatio / 10000U));
            /* 最后 10 秒：每 2 秒轻声鸣叫 60ms（3kHz 柔和短啾）*/
            if (chirpUntil != 0U && now >= chirpUntil) {
              chirpUntil = 0U;
              Buzzer_Off();
            }
            if ((t >= (WAKE_TOTAL_MS - WAKE_CHIRP_MS)) && (now >= nextChirp)) {
              nextChirp = now + WAKE_CHIRP_GAP_MS;
              chirpUntil = now + WAKE_CHIRP_LEN_MS;
              Buzzer_Set(3000U, 10U);
            }
          }
          break;
        }

        /*------ 需求5：火警紧急警示 ------*/
        case ST_FIRE:
        default:
          /* 500ms 周期：前 250ms 红灯+蜂鸣，后 250ms 静默 => 2Hz 刺眼快闪 */
          if (((now - lastFire) % (FIRE_HALF_MS * 2U)) < FIRE_HALF_MS) {
            /* 前半拍：红灯全亮 + 蜂鸣 */
            Lamp_SetRGB(PWM_MAX, 0U, 0U);
            Buzzer_Set(2000U, 20U);
          } else {
            Lamp_SetRGB(0U, 0U, 0U);
            Buzzer_Off();
          }
          break;
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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    /* HSE 晶振不可用时退回 HSI 时钟，保证程序绝不卡死 */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSEState = RCC_HSE_OFF;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
      Error_Handler();
    }
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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
  (void)file;
  (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
