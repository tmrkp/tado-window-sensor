// Fix the pull resistor the BK7231N SDK applies to GPIO wake pins.
//
// When entering deep sleep, sctrl_enter_rtos_deep_sleep_fix() forces the pull
// from the wake edge: falling edge -> pull-up, rising edge -> pull-down.
// That suits a button pulling *to* VCC on rising, but our reed switch pulls to
// GND: to detect the switch *opening* we wake on the rising edge, and a
// pull-down would hold the pin LOW forever, so the module would never wake.
//
// The SDK configures the pin via sddev_control(), which lives in another
// object file, so we intercept it with `-Wl,--wrap=sddev_control` and turn
// that one pull-down into a pull-up.

#include <stdint.h>

// From the SDK's gpio_pub.h (not included to avoid pulling in SDK typedefs).
#define GPIO_DEV_NAME "gpio"
#define GPIO_CMD_MAGIC (0xcaa0000)
#define CMD_GPIO_CFG (GPIO_CMD_MAGIC + 0)
#define GPIO_CFG_PARAM(id, mode) ((id) + (((mode)&0xff) << 8))
#define GPIO_CFG_PARAM_DEMUX_ID(param) ((param)&0xff)
#define GPIO_CFG_PARAM_DEMUX_MODE(param) (((param) >> 8) & 0xff)
#define GMODE_INPUT_PULLDOWN 0
#define GMODE_INPUT_PULLUP 3

// GPIO number to keep pulled up while sleeping, or -1 for none.
// Set right before entering deep sleep.
volatile int deep_sleep_pullup_gpio = -1;

uint32_t __real_sddev_control(char* dev_name, uint32_t cmd, void* param);

uint32_t __wrap_sddev_control(char* dev_name, uint32_t cmd, void* param) {
    if (deep_sleep_pullup_gpio >= 0 && cmd == CMD_GPIO_CFG && param &&
        dev_name[0] == 'g' && dev_name[1] == 'p' && dev_name[2] == 'i' && dev_name[3] == 'o' &&
        dev_name[4] == '\0') {
        uint32_t cfg = *(uint32_t*)param;
        if ((int)GPIO_CFG_PARAM_DEMUX_ID(cfg) == deep_sleep_pullup_gpio &&
            GPIO_CFG_PARAM_DEMUX_MODE(cfg) == GMODE_INPUT_PULLDOWN) {
            uint32_t fixed = GPIO_CFG_PARAM(deep_sleep_pullup_gpio, GMODE_INPUT_PULLUP);
            return __real_sddev_control(dev_name, cmd, &fixed);
        }
    }
    return __real_sddev_control(dev_name, cmd, param);
}
