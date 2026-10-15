/*
 * Reconhecimento facial no ESP32-P4 com a Pi Camera Module 3 (IMX708),
 * adaptado de esp-who/examples/human_face_recognition para a Waveshare
 * ESP32-P4-Module-DEV-KIT, sem display.
 *
 *   recognize  BOOT: clique        reconhece o rosto do próximo frame
 *   enroll     BOOT: duplo clique  cadastra o rosto do próximo frame
 *   delete     BOOT: segurar 2 s   apaga o último cadastrado
 *
 * Os comandos também podem ser digitados no monitor serial (prompt "face>").
 * Enroll/recognize usam o frame seguinte ao comando: espere o log
 * "1 rosto(s) na imagem" antes. Os cadastros ficam em /spiflash/face.db.
 */
#include "face_recognition_app.hpp"
#include "frame_cap_pipeline.hpp"
#include "who_spiflash_fatfs.hpp"

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
    ESP_ERROR_CHECK(spiflash_fatfs_mount());

    auto frame_cap = get_mipi_csi_frame_cap_pipeline();
    auto recognition_app = new FaceRecognitionApp(frame_cap, CONFIG_SPIFLASH_MOUNT_POINT "/face.db");
    recognition_app->run();
}
