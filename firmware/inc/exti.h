#ifndef _EXTI_H_
#define _EXTI_H_

#include <stdbool.h>
#include <plat/inc/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ExtiTrigger
{
    EXTI_TRIGGER_RISING = 0,
    EXTI_TRIGGER_FALLING,
    EXTI_TRIGGER_BOTH,
};

enum ExtiLine
{
    EXTI_LINE_P0 = 0,
    EXTI_LINE_P1,
    EXTI_LINE_P2,
    EXTI_LINE_P3,
    EXTI_LINE_P4,
    EXTI_LINE_P5,
    EXTI_LINE_P6,
    EXTI_LINE_P7,
    EXTI_LINE_P8,
    EXTI_LINE_P9,
    EXTI_LINE_P10,
    EXTI_LINE_P11,
    EXTI_LINE_P12,
    EXTI_LINE_P13,
    EXTI_LINE_P14,
    EXTI_LINE_P15,
    EXTI_LINE_PVD = 16,
    EXTI_LINE_RTC_ALARM = 17,
    EXTI_LINE_USB_OTG_FS_WKUP = 18,
    EXTI_LINE_RTC_TAMPER_TS = 21,
    EXTI_LINE_RTC_WKUP = 22,
};

void extiEnableIntLine(const enum ExtiLine line, enum ExtiTrigger trigger);
void extiDisableIntLine(const enum ExtiLine line);
bool extiIsPendingLine(const enum ExtiLine line);
void extiClearPendingLine(const enum ExtiLine line);

static inline void extiEnableIntGpio(const struct Gpio *__restrict gpio, enum ExtiTrigger trigger)
{
    extiEnableIntLine(gpio->gpio & GPIO_PIN_MASK, trigger);
}
static inline void extiDisableIntGpio(const struct Gpio *__restrict gpio)
{
    extiDisableIntLine(gpio->gpio & GPIO_PIN_MASK);
}
static inline bool extiIsPendingGpio(const struct Gpio *__restrict gpio)
{
    return extiIsPendingLine(gpio->gpio & GPIO_PIN_MASK);
}
static inline void extiClearPendingGpio(const struct Gpio *__restrict gpio)
{
    extiClearPendingLine(gpio->gpio & GPIO_PIN_MASK);
}

#ifdef __cplusplus
}
#endif

#endif
