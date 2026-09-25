#pragma once
// Pinagem da placa (ESP32-S3)
#include "driver/gpio.h"

namespace pin {
// Display 1 (olho esquerdo) - SPI2, pinos IOMUX
constexpr gpio_num_t LCD1_SCLK = GPIO_NUM_12;
constexpr gpio_num_t LCD1_MOSI = GPIO_NUM_11;
constexpr gpio_num_t LCD1_DC   = GPIO_NUM_10;
constexpr gpio_num_t LCD1_CS   = GPIO_NUM_9;
// Display 2 (olho direito) - SPI3, via GPIO matrix
constexpr gpio_num_t LCD2_SCLK = GPIO_NUM_4;
constexpr gpio_num_t LCD2_MOSI = GPIO_NUM_5;
constexpr gpio_num_t LCD2_DC   = GPIO_NUM_6;
constexpr gpio_num_t LCD2_CS   = GPIO_NUM_7;
// Compartilhados
constexpr gpio_num_t LCD_RST   = GPIO_NUM_8;
constexpr gpio_num_t LCD_BL    = GPIO_NUM_13;   // gate do MOSFET dos backlights
// INMP441 (L/R: mic esquerdo -> GND, mic direito -> 3V3)
constexpr gpio_num_t I2S_BCLK  = GPIO_NUM_15;
constexpr gpio_num_t I2S_WS    = GPIO_NUM_16;
constexpr gpio_num_t I2S_DIN   = GPIO_NUM_17;
}  // namespace pin
