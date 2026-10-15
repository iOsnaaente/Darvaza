#include "face_recognition_app.hpp"
#include "button_gpio.h"
#include "esp_console.h"
#include "human_face_detect.hpp"
#include "iot_button.h"
#include "who_yield2idle.hpp"

using namespace who;
using recognition::WhoRecognitionCore;

static const char *TAG = "FaceRecognition";

// BOOT da placa: GPIO35 (strapping do modo de boot), ativo em nível baixo
#define BOOT_BUTTON_GPIO   35
// Apagar é o único comando destrutivo: exige segurar 2 s
#define BOOT_LONG_PRESS_MS 2000
// Frames seguidos com a mesma contagem de rostos antes de logar a mudança
#define PRESENCE_FRAMES    5

static constexpr struct {
    const char *name;
    const char *help;
    EventBits_t event;
} s_commands[] = {
    {"recognize", "Reconhece o rosto do próximo frame (BOOT: clique)", WhoRecognitionCore::RECOGNIZE},
    {"enroll", "Cadastra o rosto do próximo frame (BOOT: duplo clique)", WhoRecognitionCore::ENROLL},
    {"delete", "Apaga o último rosto cadastrado (BOOT: segurar 2 s)", WhoRecognitionCore::DELETE},
};

FaceRecognitionApp::FaceRecognitionApp(frame_cap::WhoFrameCap *frame_cap, const char *db_path) :
    m_frame_cap(frame_cap),
    m_recognition(new recognition::WhoRecognition(frame_cap->get_last_node())),
    m_recognizer(new HumanFaceRecognizer(db_path)),
    m_faces_reported(0),
    m_faces_candidate(0),
    m_candidate_frames(0)
{
    m_recognition->set_recognizer(m_recognizer);
    m_recognition->set_detect_model(new HumanFaceDetect());

    // A tarefa de reconhecimento troca o callback do detector durante um
    // enroll/recognize e depois restaura o dela, então os dois precisam dele.
    auto detect_cb = std::bind(&FaceRecognitionApp::detect_result_cb, this, std::placeholders::_1);
    m_recognition->get_detect_task()->set_detect_result_cb(detect_cb);
    auto recognition_task = m_recognition->get_recognition_task();
    recognition_task->set_detect_result_cb(detect_cb);
    recognition_task->set_recognition_result_cb(
        std::bind(&FaceRecognitionApp::recognition_result_cb, this, std::placeholders::_1));
}

bool FaceRecognitionApp::run()
{
    // Carrega o face.db antes das tarefas começarem a usar o recognizer
    ESP_LOGI(TAG, "%d rosto(s) cadastrado(s)", m_recognizer->get_num_feats());

    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, 2, 0);
    }
    ret &= m_recognition->get_detect_task()->run(3584, 2, 1);
    ret &= m_recognition->get_recognition_task()->run(3584, 2, 1);
    if (!ret) {
        return false;
    }
    start_button();
    start_console();
    return true;
}

void FaceRecognitionApp::send(EventBits_t event)
{
    auto task = m_recognition->get_recognition_task();
    if (task->is_active()) {
        xEventGroupSetBits(task->get_event_group(), event);
    }
}

void FaceRecognitionApp::btn_event_handler(void *button_handle, void *usr_data)
{
    auto app = static_cast<FaceRecognitionApp *>(usr_data);
    switch (iot_button_get_event(static_cast<button_handle_t>(button_handle))) {
    case BUTTON_SINGLE_CLICK:
        app->send(WhoRecognitionCore::RECOGNIZE);
        break;
    case BUTTON_DOUBLE_CLICK:
        app->send(WhoRecognitionCore::ENROLL);
        break;
    case BUTTON_LONG_PRESS_START:
        app->send(WhoRecognitionCore::DELETE);
        break;
    default:
        break;
    }
}

void FaceRecognitionApp::start_button()
{
    button_config_t btn_config = {};
    btn_config.long_press_time = BOOT_LONG_PRESS_MS;
    button_gpio_config_t gpio_config = {};
    gpio_config.gpio_num = BOOT_BUTTON_GPIO;
    gpio_config.active_level = 0;

    button_handle_t btn = nullptr;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_config, &gpio_config, &btn));
    for (auto event : {BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START}) {
        ESP_ERROR_CHECK(iot_button_register_cb(btn, event, nullptr, btn_event_handler, this));
    }
}

int FaceRecognitionApp::console_cmd_handler(void *context, int argc, char **argv)
{
    auto app = static_cast<FaceRecognitionApp *>(context);
    for (const auto &cmd : s_commands) {
        if (strcmp(argv[0], cmd.name) == 0) {
            app->send(cmd.event);
            return 0;
        }
    }
    return 1;
}

void FaceRecognitionApp::start_console()
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "face>";
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (const auto &cmd : s_commands) {
        esp_console_cmd_t console_cmd = {};
        console_cmd.command = cmd.name;
        console_cmd.help = cmd.help;
        console_cmd.func_w_context = console_cmd_handler;
        console_cmd.context = this;
        ESP_ERROR_CHECK(esp_console_cmd_register(&console_cmd));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

// Sem display, este log faz o papel das caixas na tela: avisa quando um rosto
// entra ou sai da imagem, para saber se já dá para cadastrar/reconhecer.
void FaceRecognitionApp::detect_result_cb(const detect::WhoDetect::result_t &result)
{
    size_t n = result.det_res.size();
    if (n != m_faces_candidate) {
        m_faces_candidate = n;
        m_candidate_frames = 0;
    }
    if (n == m_faces_reported || ++m_candidate_frames < PRESENCE_FRAMES) {
        return;
    }
    m_faces_reported = n;
    if (n == 0) {
        ESP_LOGI(TAG, "nenhum rosto na imagem");
    } else {
        ESP_LOGI(TAG, "%u rosto(s) na imagem (score %.2f)", (unsigned)n, result.det_res.front().score);
    }
}

void FaceRecognitionApp::recognition_result_cb(const std::string &result)
{
    ESP_LOGI(TAG, "%s", result.c_str());
}
