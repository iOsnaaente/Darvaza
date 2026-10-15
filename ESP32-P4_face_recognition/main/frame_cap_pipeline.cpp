#include "frame_cap_pipeline.hpp"
#include "esp_video_init.h"
#include "video_capture.hpp"

using namespace who::frame_cap;

// num of frames the model take to get result
#define MODEL_TIME 3

// Pi Camera Module 3 (IMX708) no conector CSI da Waveshare ESP32-P4-Module-DEV-KIT.
// O SCCB da câmera divide o I2C interno da placa (port 0, SCL=GPIO8, SDA=GPIO7);
// o conector de 15 pinos não leva reset/pwdn, então o sensor fica sempre ligado.
#define CAM_SCCB_I2C_PORT 0
#define CAM_SCCB_SCL_PIN  GPIO_NUM_8
#define CAM_SCCB_SDA_PIN  GPIO_NUM_7
#define CAM_SCCB_FREQ_HZ  100000
// Imagem de cabeça para baixo (depende de como a câmera está montada): true
#define CAM_ROTATE_180    false

static void camera_init()
{
    esp_video_init_sccb_config_t sccb_config = {};
    sccb_config.init_sccb = true;
    sccb_config.i2c_config.port = CAM_SCCB_I2C_PORT;
    sccb_config.i2c_config.scl_pin = CAM_SCCB_SCL_PIN;
    sccb_config.i2c_config.sda_pin = CAM_SCCB_SDA_PIN;
    sccb_config.freq = CAM_SCCB_FREQ_HZ;

    static esp_video_init_csi_config_t csi_config = {};
    csi_config.sccb_config = sccb_config;
    csi_config.reset_pin = GPIO_NUM_NC;
    csi_config.pwdn_pin = GPIO_NUM_NC;

    // O motor do autofoco (DW9807, I2C 0x0c) é outro chip no mesmo barramento:
    // sem esta entrada o esp_video nem procura por ele e a lente não se mexe.
    static esp_video_init_cam_motor_config_t motor_config = {};
    motor_config.sccb_config = sccb_config;
    motor_config.reset_pin = GPIO_NUM_NC;
    motor_config.pwdn_pin = GPIO_NUM_NC;
    motor_config.signal_pin = GPIO_NUM_NC;

    static esp_video_init_config_t cam_config = {};
    cam_config.csi = &csi_config;
    cam_config.cam_motor = &motor_config;
    ESP_ERROR_CHECK(esp_video_init(&cam_config));
}

// The size of the fb_count and ringbuf_len must be big enough. If you have no idea how to set them, try with 5 and
// larger.
WhoFrameCap *get_mipi_csi_frame_cap_pipeline()
{
    esp_log_level_set("ISP_AWB", ESP_LOG_ERROR);
    camera_init();

    // O ISP converte o RAW10 da IMX708 para RGB565 no tamanho do modo do sensor
    // (CONFIG_CAMERA_IMX708_MIPI_IF_FORMAT_INDEX_DEFAULT, 1280x720).
    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, V4L2_PIX_FMT_RGB565, MODEL_TIME + 3)
                   .set_hflip(CAM_ROTATE_180)
                   .set_vflip(CAM_ROTATE_180);
    ESP_ERROR_CHECK(cap->init(cfg));
    ESP_ERROR_CHECK(cap->start());

    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}
