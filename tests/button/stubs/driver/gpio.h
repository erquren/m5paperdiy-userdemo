#pragma once
enum gpio_num_t { GPIO_NUM_3 = 3, GPIO_NUM_4 = 4 };
int gpio_get_level(gpio_num_t pin);
