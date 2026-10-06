#ifndef LINUX_LEDS_H
#define LINUX_LEDS_H
#include <linux/types.h>
struct led_classdev { const char *name; };
enum led_brightness { LED_OFF = 0, LED_ON = 1, LED_FULL = 255 };
#endif
