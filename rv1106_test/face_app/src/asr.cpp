/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "asr.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http.h"
#include "util.h"

static const char *TAG = "asr";

/* Saída de erro dos motores (o log do whisper.cpp/Vosk, com -v no engines.tsv). */
#define ENGINE_LOG  "/tmp/asr_bench.log"
#define REC_PREFIX  "rec-"
#define MAX_NAME    64

struct engine_t {
    std::string name, cmd;
};

static std::string s_dir;   /* com "/" no fim */
static std::function<void(bool)> s_pause_ai;

/* A medição em andamento (ou a última) e o results.jsonl: tudo sob s_lock. */
static std::mutex s_lock;
static struct {
    bool running = false;
    bool pause_ai = false;
    bool stop = false;
    pid_t pid = 0;              /* processo do motor rodando agora */
    std::string engine;         /* motor rodando agora */
    std::string clip;           /* frase que ele está transcrevendo */
    int done = 0, total = 0;    /* linhas recebidas / esperadas (motores x frases) */
    int64_t started_us = 0, ended_us = 0;
    std::string error;          /* da última medição: motor que terminou mal */
} s_job;

void asr_init(const std::string &dir, std::function<void(bool)> pause_ai)
{
    s_dir = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    s_pause_ai = std::move(pause_ai);
    if (access((s_dir + "engines.tsv").c_str(), R_OK) != 0) {
        LOGW(TAG, "%sengines.tsv não existe: a página /asr fica sem motores (scripts/asr.sh deploy)", s_dir.c_str());
    }
}

/* ---- Arquivos do asr_bench --------------------------------------------------- */

static std::string results_path()
{
    return s_dir + "results.jsonl";
}

static std::string clip_path(const std::string &name, const char *ext)
{
    return s_dir + "clips/" + name + ext;
}

/* Nomes vão para o shell e para caminhos: só letras, dígitos, "-", "_" e ".". */
static bool valid_name(const std::string &s)
{
    if (s.empty() || s.size() > MAX_NAME || s[0] == '.') {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return isalnum((unsigned char)c) || strchr("-_.", c); });
}

static std::vector<engine_t> read_engines()
{
    std::vector<engine_t> out;
    std::ifstream f(s_dir + "engines.tsv");
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) {
            continue;
        }
        engine_t e{line.substr(0, tab), line.substr(tab + 1)};
        if (valid_name(e.name)) {
            out.push_back(e);
        }
    }
    return out;
}

/* Nomes das frases (os .wav de clips/, sem o .wav), em ordem. */
static std::vector<std::string> list_clips()
{
    std::vector<std::string> out;
    DIR *d = opendir((s_dir + "clips").c_str());
    if (!d) {
        return out;
    }
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0 && valid_name(n.substr(0, n.size() - 4))) {
            out.push_back(n.substr(0, n.size() - 4));
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static std::string read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void json_str(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            appendf(out, "\\u%04x", c);
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

static std::string trim(std::string s)
{
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

static void append_result(const std::string &line)
{
    FILE *f = fopen(results_path().c_str(), "a");
    if (!f) {
        LOGE(TAG, "não consegui gravar %s: %s", results_path().c_str(), strerror(errno));
        return;
    }
    fputs(line.c_str(), f);
    fputc('\n', f);
    fclose(f);
}

/* ---- Medição ----------------------------------------------------------------- */

/* Roda "sh -c cmd" num grupo de processos próprio (o /asr/stop mata o grupo),
 * com a saída padrão num pipe. O motor é o primeiro que o kernel mata se
 * faltar memória: o face_app (a página) fica. */
static pid_t spawn(const std::string &cmd, int *out_fd)
{
    int p[2];
    if (pipe(p) != 0) {
        return -1;
    }
    std::string full = "echo 1000 > /proc/self/oom_score_adj; " + cmd;
    long max_fd = sysconf(_SC_OPEN_MAX);
    pid_t pid = fork();
    if (pid == 0) {
        /* Só chamadas seguras depois de fork num processo com threads. */
        setpgid(0, 0);
        dup2(p[1], STDOUT_FILENO);
        for (long fd = 3; fd < max_fd; fd++) {
            close((int)fd);     /* câmera, NPU, sockets: nada disso vai junto */
        }
        execl("/bin/sh", "sh", "-c", full.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    *out_fd = p[0];
    return pid;
}

static std::string exit_reason(int status, bool stopped)
{
    if (stopped) {
        return "interrompido";
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        return sig == SIGKILL ? "o processo foi morto (SIGKILL): faltou memória?"
                              : "o processo morreu com o sinal " + std::to_string(sig);
    }
    return "o processo saiu com código " + std::to_string(WEXITSTATUS(status)) + " (veja " ENGINE_LOG ")";
}

/*
 * Um motor sobre todas as frases. Cada linha JSON do processo vira uma linha
 * do results.jsonl, com o motor, a medição (run), se a IA estava parada e a
 * memória livre antes de começar. Frase sem linha (o processo morreu) vira
 * uma linha de erro, a não ser que tenha sido o /asr/stop.
 */
static void run_engine(const engine_t &e, const std::vector<std::string> &clips, long run, bool pause_ai)
{
    unsigned long long mem_before_kb = mem_available() / 1024;
    std::string cmd = "cd '" + s_dir + "' && exec " + e.cmd;
    for (const std::string &c : clips) {
        cmd += " clips/" + c + ".wav";
    }
    cmd += " 2>>" ENGINE_LOG " </dev/null";

    std::string head;
    appendf(head, "{\"engine\":\"%s\",\"run\":%ld,\"pause_ai\":%s,\"mem_before_kb\":%llu,", e.name.c_str(), run,
            pause_ai ? "true" : "false", mem_before_kb);

    int fd = -1;
    pid_t pid = spawn(cmd, &fd);
    if (pid < 0) {
        std::lock_guard<std::mutex> lock(s_lock);
        s_job.error = e.name + ": não consegui criar o processo";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_job.pid = pid;
        s_job.engine = e.name;
        s_job.clip = clips[0];
    }
    LOGI(TAG, "%s: %u frase(s), %llu MB livres", e.name.c_str(), (unsigned)clips.size(), mem_before_kb / 1024);

    std::set<std::string> got;
    FILE *out = fdopen(fd, "r");
    char *line = nullptr;
    size_t cap = 0;
    while (out && getline(&line, &cap, out) > 0) {
        std::string l = trim(line);
        if (l.size() < 2 || l[0] != '{') {
            continue;
        }
        /* "file":"x.wav" -> x */
        size_t f = l.find("\"file\":\"");
        if (f != std::string::npos) {
            size_t end = l.find('"', f + 8);
            std::string file = l.substr(f + 8, end - f - 8);
            got.insert(file.size() > 4 ? file.substr(0, file.size() - 4) : file);
        }
        std::lock_guard<std::mutex> lock(s_lock);
        append_result(head + l.substr(1));
        s_job.done++;
        s_job.clip = clips[std::min(got.size(), clips.size() - 1)];
    }
    free(line);
    if (out) {
        fclose(out);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    std::lock_guard<std::mutex> lock(s_lock);
    s_job.pid = 0;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && got.size() == clips.size();
    if (ok) {
        return;
    }
    std::string why = exit_reason(status, s_job.stop);
    LOGW(TAG, "%s: %s", e.name.c_str(), why.c_str());
    s_job.error = e.name + ": " + why;
    if (s_job.stop) {
        return;
    }
    for (const std::string &c : clips) {
        if (!got.count(c)) {
            std::string l = head + "\"file\":";
            json_str(l, c + ".wav");
            l += ",\"error\":";
            json_str(l, why);
            append_result(l + "}");
            s_job.done++;
        }
    }
}

static void job_loop(std::vector<engine_t> engines, std::vector<std::string> clips, bool pause_ai)
{
    long run = (long)time(nullptr);
    if (pause_ai && s_pause_ai) {
        s_pause_ai(true);
    }
    for (const engine_t &e : engines) {
        {
            std::lock_guard<std::mutex> lock(s_lock);
            if (s_job.stop) {
                break;
            }
        }
        run_engine(e, clips, run, pause_ai);
    }
    if (pause_ai && s_pause_ai) {
        s_pause_ai(false);
    }
    std::lock_guard<std::mutex> lock(s_lock);
    s_job.running = false;
    s_job.ended_us = now_us();
    LOGI(TAG, "medição terminou em %.1f s%s", (s_job.ended_us - s_job.started_us) / 1e6,
         s_job.stop ? " (interrompida)" : "");
}

/* ---- HTTP -------------------------------------------------------------------- */

static void send_json(int fd, const char *status, const std::string &body)
{
    http_respond(fd, status ? status : "200 OK", "application/json; charset=utf-8", body);
}

static void send_error(int fd, const char *status, const char *msg)
{
    std::string body = "{\"ok\":false,\"error\":";
    json_str(body, msg);
    send_json(fd, status, body + "}");
}

static std::vector<std::string> split_list(const std::string &s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

static void page_handler(HttpRequest &req)
{
    std::string html = read_file(exe_dir() + "asr.html");
    if (html.empty()) {
        http_respond(req.fd, "500 Internal Server Error", "text/plain", "asr.html nao encontrado\n");
        return;
    }
    http_respond(req.fd, "200 OK", "text/html; charset=utf-8", html);
}

/*
 * {"ready":bool,"dir":"...","mem_avail":N,
 *  "engines":["vosk-small-pt",...],
 *  "clips":[{"name":"fleurs-01","ms":N,"ref":"..."},...],
 *  "job":{"running":bool,"engine":"","clip":"","done":N,"total":N,"elapsed_ms":N,"pause_ai":bool,"error":""},
 *  "results":[<linhas do results.jsonl>]}
 */
static void state_handler(HttpRequest &req)
{
    std::vector<engine_t> engines = read_engines();
    std::string s;
    appendf(s, "{\"ready\":%s,\"dir\":", engines.empty() ? "false" : "true");
    json_str(s, s_dir);
    appendf(s, ",\"mem_avail\":%llu,\"engines\":[", (unsigned long long)mem_available());
    for (size_t i = 0; i < engines.size(); i++) {
        s += i ? "," : "";
        json_str(s, engines[i].name);
    }
    s += "],\"clips\":[";
    std::vector<std::string> clips = list_clips();
    for (size_t i = 0; i < clips.size(); i++) {
        struct stat st = {};
        stat(clip_path(clips[i], ".wav").c_str(), &st);
        s += i ? ",{\"name\":" : "{\"name\":";
        json_str(s, clips[i]);
        /* WAV de 16 kHz mono 16 bits: 32 bytes por ms depois do cabeçalho. */
        appendf(s, ",\"ms\":%lld,\"ref\":", (long long)std::max<off_t>(0, st.st_size - 44) / 32);
        json_str(s, trim(read_file(clip_path(clips[i], ".txt"))));
        s += "}";
    }

    std::lock_guard<std::mutex> lock(s_lock);
    int64_t end = s_job.running ? now_us() : s_job.ended_us;
    appendf(s, "],\"job\":{\"running\":%s,\"engine\":", s_job.running ? "true" : "false");
    json_str(s, s_job.engine);
    s += ",\"clip\":";
    json_str(s, s_job.clip);
    appendf(s, ",\"done\":%d,\"total\":%d,\"elapsed_ms\":%lld,\"pause_ai\":%s,\"error\":", s_job.done, s_job.total,
            (long long)(s_job.started_us ? (end - s_job.started_us) / 1000 : 0), s_job.pause_ai ? "true" : "false");
    json_str(s, s_job.error);
    s += "},\"results\":[";
    std::ifstream f(results_path());
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '{') {
            s += first ? "" : ",";
            s += line;
            first = false;
        }
    }
    s += "]}\n";
    send_json(req.fd, nullptr, s);
}

static void clip_handler(HttpRequest &req)
{
    std::string name;
    if (!http_query_value(req.query, "name", name) || !valid_name(name = http_url_decode(name))) {
        http_respond(req.fd, "400 Bad Request", "text/plain", "use /asr/clip?name=X\n");
        return;
    }
    std::string wav = read_file(clip_path(name, ".wav"));
    if (wav.empty()) {
        http_respond(req.fd, "404 Not Found", "text/plain", "frase nao encontrada\n");
        return;
    }
    http_respond(req.fd, "200 OK", "audio/wav", wav);
}

static void run_handler(HttpRequest &req)
{
    std::string val;
    std::vector<engine_t> all = read_engines(), engines;
    if (all.empty()) {
        return send_error(req.fd, "503 Service Unavailable", "asr_bench não instalado na placa (scripts/asr.sh deploy)");
    }
    http_query_value(req.query, "engines", val);
    for (const std::string &n : split_list(http_url_decode(val))) {
        auto it = std::find_if(all.begin(), all.end(), [&n](const engine_t &e) { return e.name == n; });
        if (it == all.end()) {
            return send_error(req.fd, "400 Bad Request", ("motor desconhecido: " + n).c_str());
        }
        engines.push_back(*it);
    }
    val.clear();
    http_query_value(req.query, "clips", val);
    std::vector<std::string> clips = split_list(http_url_decode(val));
    for (const std::string &c : clips) {
        if (!valid_name(c) || access(clip_path(c, ".wav").c_str(), R_OK) != 0) {
            return send_error(req.fd, "400 Bad Request", ("frase não encontrada: " + c).c_str());
        }
    }
    if (engines.empty() || clips.empty()) {
        return send_error(req.fd, "400 Bad Request", "escolha ao menos um motor e uma frase");
    }
    val.clear();
    bool pause_ai = http_query_value(req.query, "pause", val) && val == "1";

    {
        std::lock_guard<std::mutex> lock(s_lock);
        if (s_job.running) {
            return send_error(req.fd, "409 Conflict", "já tem uma medição rodando");
        }
        s_job.running = true;
        s_job.stop = false;
        s_job.pause_ai = pause_ai;
        s_job.engine = engines[0].name;
        s_job.clip = clips[0];
        s_job.done = 0;
        s_job.total = (int)(engines.size() * clips.size());
        s_job.started_us = now_us();
        s_job.error.clear();
    }
    LOGI(TAG, "medição: %u motor(es) x %u frase(s)%s", (unsigned)engines.size(), (unsigned)clips.size(),
         pause_ai ? ", IA de rostos parada" : "");
    std::thread(job_loop, engines, clips, pause_ai).detach();
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

static void stop_handler(HttpRequest &req)
{
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_job.running) {
        s_job.stop = true;
        if (s_job.pid > 0) {
            kill(-s_job.pid, SIGKILL);
        }
    }
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

/* O cabeçalho que a página escreve (44 bytes, PCM): 16 kHz, mono, 16 bits. */
static bool canonical_wav(const std::string &b)
{
    auto u16 = [&b](size_t o) { return (unsigned)(uint8_t)b[o] | (unsigned)(uint8_t)b[o + 1] << 8; };
    return b.size() > 44 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 8, "WAVEfmt ") == 0 && u16(20) == 1 &&
           u16(22) == 1 && (u16(24) | u16(26) << 16) == 16000 && u16(34) == 16;
}

static void upload_handler(HttpRequest &req)
{
    if (!canonical_wav(req.body)) {
        return send_error(req.fd, "400 Bad Request", "o corpo precisa ser um WAV PCM de 16 kHz, mono, 16 bits");
    }
    std::string ref;
    if (http_query_value(req.query, "ref", ref)) {
        ref = trim(http_url_decode(ref));
    }

    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof(stamp), REC_PREFIX "%Y%m%d-%H%M%S", localtime(&t));
    std::string name = stamp;
    for (int i = 2; access(clip_path(name, ".wav").c_str(), F_OK) == 0; i++) {
        name = std::string(stamp) + "-" + std::to_string(i);
    }
    mkdir((s_dir + "clips").c_str(), 0755);
    std::ofstream wav(clip_path(name, ".wav"), std::ios::binary), txt(clip_path(name, ".txt"), std::ios::binary);
    wav << req.body;
    txt << ref << "\n";
    if (!wav.good() || !txt.good()) {
        return send_error(req.fd, "500 Internal Server Error", "não consegui gravar o arquivo na placa");
    }
    LOGI(TAG, "gravação %s: %.1f s", name.c_str(), (req.body.size() - 44) / 32000.0);
    std::string body = "{\"ok\":true,\"name\":";
    json_str(body, name);
    send_json(req.fd, nullptr, body + "}");
}

static void delete_handler(HttpRequest &req)
{
    std::string name;
    http_query_value(req.query, "name", name);
    name = http_url_decode(name);
    if (!valid_name(name) || name.compare(0, strlen(REC_PREFIX), REC_PREFIX) != 0) {
        return send_error(req.fd, "400 Bad Request", "só as gravações (rec-...) podem ser apagadas");
    }
    if (unlink(clip_path(name, ".wav").c_str()) != 0) {
        return send_error(req.fd, "404 Not Found", "gravação não encontrada");
    }
    unlink(clip_path(name, ".txt").c_str());
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

static void clear_handler(HttpRequest &req)
{
    std::lock_guard<std::mutex> lock(s_lock);
    if (truncate(results_path().c_str(), 0) != 0 && errno != ENOENT) {
        return send_error(req.fd, "500 Internal Server Error", strerror(errno));
    }
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

void asr_routes(HttpServer &server)
{
    server.on("GET", "/asr", page_handler);
    server.on("GET", "/asr/state", state_handler);
    server.on("GET", "/asr/clip", clip_handler);
    server.on("POST", "/asr/run", run_handler);
    server.on("POST", "/asr/stop", stop_handler);
    server.on("POST", "/asr/upload", upload_handler);
    server.on("POST", "/asr/delete", delete_handler);
    server.on("POST", "/asr/clear", clear_handler);
}
