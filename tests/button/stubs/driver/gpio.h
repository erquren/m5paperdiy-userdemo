#pragma once
#include <esp_err.h>
enum gpio_num_t { GPIO_NUM_3 = 3, GPIO_NUM_4 = 4 };
enum gpio_int_type_t { GPIO_INTR_LOW_LEVEL };
using gpio_isr_t = void (*)(void*);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_isr_handler_add(gpio_num_t, gpio_isr_t, void*);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
