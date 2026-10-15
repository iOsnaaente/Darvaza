#pragma once
#include "who_recognition.hpp"

// Mesmo papel do WhoRecognitionAppTerm do esp-who (resultado no log serial),
// mas sem BSP: os comandos chegam pelo botão BOOT e pelo console serial.
class FaceRecognitionApp {
public:
    FaceRecognitionApp(who::frame_cap::WhoFrameCap *frame_cap, const char *db_path);
    bool run();

private:
    static void btn_event_handler(void *button_handle, void *usr_data);
    static int console_cmd_handler(void *context, int argc, char **argv);
    void send(EventBits_t event);
    void start_button();
    void start_console();
    void detect_result_cb(const who::detect::WhoDetect::result_t &result);
    void recognition_result_cb(const std::string &result);

    who::frame_cap::WhoFrameCap *m_frame_cap;
    who::recognition::WhoRecognition *m_recognition;
    HumanFaceRecognizer *m_recognizer;
    size_t m_faces_reported;
    size_t m_faces_candidate;
    int m_candidate_frames;
};
