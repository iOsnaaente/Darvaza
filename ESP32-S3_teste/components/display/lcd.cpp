#include "lcd.hpp"

#include <cassert>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

#include "board.hpp"
#include "render.hpp"

namespace display {
namespace {

const char *const TAG = "lcd";

constexpr uint32_t LCD1_PCLK_HZ = 40 * 1000 * 1000;   // IOMUX: pode testar 80 MHz
constexpr uint32_t LCD2_PCLK_HZ = 40 * 1000 * 1000;   // GPIO matrix: manter 40 MHz

// ISR: fim de uma transferência de cor -> libera um buffer
bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<Eye *>(ctx)->free_bufs, &woken);
    return woken == pdTRUE;
}

void lcd_create(Eye &eye, spi_host_device_t host, gpio_num_t sclk, gpio_num_t mosi, gpio_num_t dc, gpio_num_t cs,
                gpio_num_t rst, uint32_t pclk_hz, BaseType_t isr_core)
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = sclk;
    bus.mosi_io_num     = mosi;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = BAND_BYTES;
    // ISR no mesmo core da task que desenha. Com ela no core 0, o callback de fim de
    // DMA acordava a task no core 1 enquanto a ISR ainda terminava, e o próximo
    // draw_bitmap às vezes esperava para sempre uma transação que o driver perdeu
    // (ver 'isr_cpu_id' na doc do SPI master).
    bus.isr_cpu_id      = isr_core == 0 ? ESP_INTR_CPU_AFFINITY_0 : ESP_INTR_CPU_AFFINITY_1;
    ESP_ERROR_CHECK(spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO));

    // Semáforo e buffers antes do IO: o callback já pode disparar
    eye.free_bufs = xSemaphoreCreateCounting(2, 2);
    for (auto &b : eye.buf) {
        b = static_cast<uint16_t *>(heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        assert(b && "sem RAM interna DMA para o buffer do LCD");
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num         = cs;
    io_cfg.dc_gpio_num         = dc;
    io_cfg.spi_mode            = 0;
    io_cfg.pclk_hz             = pclk_hz;
    io_cfg.trans_queue_depth   = 10;
    io_cfg.on_color_trans_done = on_color_done;
    io_cfg.user_ctx            = &eye;
    io_cfg.lcd_cmd_bits        = 8;
    io_cfg.lcd_param_bits      = 8;

    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = rst;
    pcfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR;
    pcfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &pcfg, &eye.panel));
}

}  // namespace

void lcd_init_all(Eye (&eyes)[2], BaseType_t isr_core)
{
    lcd_create(eyes[0], SPI2_HOST, pin::LCD1_SCLK, pin::LCD1_MOSI, pin::LCD1_DC, pin::LCD1_CS,
               pin::LCD_RST, LCD1_PCLK_HZ, isr_core);
    // RST compartilhado: o display 2 não controla o pino (usa software reset)
    lcd_create(eyes[1], SPI3_HOST, pin::LCD2_SCLK, pin::LCD2_MOSI, pin::LCD2_DC, pin::LCD2_CS,
               GPIO_NUM_NC, LCD2_PCLK_HZ, isr_core);

    for (auto &eye : eyes) {
        ESP_ERROR_CHECK(esp_lcd_panel_reset(eye.panel));   // olho 0: reset HW reseta os dois
        ESP_ERROR_CHECK(esp_lcd_panel_init(eye.panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(eye.panel, true));   // cores negativas? troque para false
        // ESP_ERROR_CHECK(esp_lcd_panel_mirror(eye.panel, true, false)); // se vier espelhado
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(eye.panel, true));
    }
    ESP_LOGI(TAG, "displays inicializados");
}

// ---------------------------------------------------------------------------
// Backlight: LEDC a 20 kHz (fora da faixa audível, não aparece nos mics)
// ---------------------------------------------------------------------------
void backlight_init()
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = LEDC_TIMER_0;
    t.freq_hz         = 20000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = pin::LCD_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 0;   // apagado até o primeiro frame (esconde lixo da GRAM)
    c.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void backlight_set(int percent)
{
    const uint32_t duty = (1023u * static_cast<uint32_t>(percent)) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

}  // namespace display
