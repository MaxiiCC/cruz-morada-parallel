#include "../include/api.h"
#include <curl/curl.h>
#include <omp.h>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <string>
#include <ctime>
#include <cstring>
#include <unistd.h> // Necesario para usleep()

// URLs de la API REST
#define API_BASE   "https://api.sebastian.cl/cpyd"
#define LOGIN_URL  API_BASE "/v1/login/authenticate"
#define PERSON_URL API_BASE "/v1/person/"

// ── Estado global del token ──────────────────────────────────────────
// Protegido con g_token_lock: multiples hilos pueden detectar
// expiracion simultaneamente y solo uno debe renovar.
static std::string g_token;
static long        g_token_exp = 0;
static std::string g_rut;
static std::string g_email;
static omp_lock_t  g_token_lock;

// ── Cache UUID → Genero ──────────────────────────────────────────────
// Protegido con g_cache_lock: el lock se libera antes de la peticion
// HTTP para no bloquear hilos mientras esperan respuesta del servidor.
static std::unordered_map<std::string, Genero> g_cache;
static omp_lock_t g_cache_lock;

// ── Logging ──────────────────────────────────────────────────────────
// Apertura/cierre atomico en modo append: seguro para llamadas
// concurrentes desde multiples hilos sin omp critical adicional.
static void log_err(const std::string& msg) {
    FILE* f = fopen("log.txt", "a");
    if (f) { fprintf(f, "[API] %s\n", msg.c_str()); fclose(f); }
    std::cerr << "[API] " << msg << "\n";
}

// ── Buffer HTTP ──────────────────────────────────────────────────────
// Callback de escritura para libcurl: acumula la respuesta HTTP
// en un string para procesarla despues de curl_easy_perform.
struct Buffer { std::string data; };

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userp) {
    ((Buffer*)userp)->data.append((char*)ptr, size * nmemb);
    return size * nmemb;
}

// ── Parseo JSON minimo ───────────────────────────────────────────────
// Extrae el valor de un campo JSON por nombre, sin libreria externa.
// Suficiente para la respuesta de /v1/person/{uuid} que tiene
// estructura fija y solo necesitamos el campo "gender".
static std::string json_get(const std::string& json, const std::string& key) {
    std::string buscar = "\"" + key + "\"";
    auto pos = json.find(buscar);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) pos++;
    size_t fin = pos;
    while (fin < json.size() && json[fin] != '"' && json[fin] != ',' && json[fin] != '}') fin++;
    return json.substr(pos, fin - pos);
}

// ── Decodificar base64 ───────────────────────────────────────────────
// Decodifica el payload del JWT (segunda seccion entre puntos)
// para extraer el campo "exp" y saber cuando expira el token.
// Implementado sin libreria externa: el payload tiene estructura simple.
static std::string base64_decode(std::string s) {
    while (s.size() % 4) s += '=';
    const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        if (c == '-') c = '+';
        if (c == '_') c = '/';
        auto idx = chars.find(c);
        if (idx == std::string::npos) continue;
        val = (val << 6) + (int)idx;
        bits += 6;
        if (bits >= 0) {
            decoded += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return decoded;
}

// ── Login ────────────────────────────────────────────────────────────
// Autentica contra la API y almacena el token JWT en g_token.
// Extrae el campo "exp" del payload para saber cuando expira.
// No es thread-safe: debe llamarse siempre bajo g_token_lock.
static bool hacer_login() {
    CURL* curl = curl_easy_init();
    if (!curl) { log_err("curl_easy_init fallo en login"); return false; }

    Buffer resp;
    std::string body = "{\"email\":\"" + g_email + "\",\"rut\":\"" + g_rut + "\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,          LOGIN_URL);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_err(std::string("Login fallo: ") + curl_easy_strerror(res));
        return false;
    }

    std::string jwt = json_get(resp.data, "jwt");
    if (jwt.empty()) {
        log_err("No se encontro jwt: " + resp.data);
        return false;
    }

    // Extraer exp del payload JWT (segunda seccion, decodificada en base64)
    long exp_time = 0;
    auto p1 = jwt.find('.');
    auto p2 = (p1 != std::string::npos) ? jwt.find('.', p1 + 1) : std::string::npos;
    if (p1 != std::string::npos && p2 != std::string::npos) {
        std::string payload = base64_decode(jwt.substr(p1 + 1, p2 - p1 - 1));
        auto epos = payload.find("\"exp\"");
        if (epos != std::string::npos) {
            epos = payload.find(':', epos);
            if (epos != std::string::npos)
                exp_time = std::stol(payload.substr(epos + 1));
        }
    }

    g_token     = jwt;
    g_token_exp = exp_time;
    long faltan = g_token_exp - (long)time(nullptr);
    std::cout << "[API] Token obtenido, expira en " << faltan << " segundos\n";
    return true;
}

// ── Verificar y renovar token si quedan menos de 60s ─────────────────
// Llamada antes de cada peticion HTTP. El lock garantiza que si varios
// hilos detectan expiracion simultaneamente, solo uno renueva el token
// y los demas esperan y reutilizan el token ya renovado.
static void asegurar_token() {
    omp_set_lock(&g_token_lock);
    long ahora = (long)time(nullptr);
    if (g_token.empty() || (g_token_exp - ahora) < 60) {
        std::cout << "[API] Renovando token...\n";
        hacer_login();
    }
    omp_unset_lock(&g_token_lock);
}

// ── API publica ──────────────────────────────────────────────────────
namespace api {

// Inicializa curl, los locks de OpenMP, carga la cache desde disco
// y obtiene el primer token JWT. Debe llamarse antes del bloque paralelo.
void init(const std::string& rut, const std::string& email) {
    g_rut   = rut;
    g_email = email;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    omp_init_lock(&g_token_lock);
    omp_init_lock(&g_cache_lock);
    
    // Cargar cache desde disco: cada linea es "UUID F|M"
    std::ifstream in("cache_generos.txt");
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find(' ');
            if (pos != std::string::npos) {
                std::string id = line.substr(0, pos);
                std::string gen = line.substr(pos + 1);
                if (gen == "F") g_cache[id] = Genero::FEMENINO;
                else if (gen == "M") g_cache[id] = Genero::MASCULINO;
            }
        }
        std::cout << "[API] Cache local cargada: " << g_cache.size() << " registros.\n";
    }
    
    hacer_login();
}

// Serializa la cache RAM a disco y libera locks y recursos de curl.
// Debe llamarse despues del bloque paralelo, desde un solo hilo.
void cleanup() {
    // Guardar la cache en disco para reutilizar en la proxima ejecucion
    std::ofstream out("cache_generos.txt");
    if (out.is_open()) {
        for (const auto& par : g_cache) {
            out << par.first << " " 
                << (par.second == Genero::FEMENINO ? "F" : "M") << "\n";
        }
        std::cout << "[API] Cache local guardada en disco.\n";
    }

    omp_destroy_lock(&g_token_lock);
    omp_destroy_lock(&g_cache_lock);
    curl_global_cleanup();
}

Genero consultar(const std::string& uuid) {
    if (uuid.empty()) return Genero::DESCONOCIDO;

    // 1. Revisar cache: el lock se toma y libera antes de cualquier
    //    peticion HTTP para no bloquear hilos durante la espera de red.
    omp_set_lock(&g_cache_lock);
    auto it = g_cache.find(uuid);
    if (it != g_cache.end()) {
        Genero g = it->second;
        omp_unset_lock(&g_cache_lock);
        return g;
    }
    omp_unset_lock(&g_cache_lock);

    // 2. Verificar token antes de la peticion
    asegurar_token();

    // 3. Peticion HTTP con handle thread_local y reintentos
    // thread_local: cada hilo reutiliza su propio handle de curl,
    // evitando el overhead del handshake TLS en cada consulta.
    thread_local CURL* curl = nullptr;
    if (!curl) {
        curl = curl_easy_init();
    } else {
        curl_easy_reset(curl);
    }

    if (!curl) { log_err("curl_easy_init fallo"); return Genero::DESCONOCIDO; }

    Buffer resp;
    std::string url = std::string(PERSON_URL) + uuid;

    // Leer el token bajo lock para evitar leer un valor parcialmente escrito
    omp_set_lock(&g_token_lock);
    std::string auth = "Authorization: Bearer " + g_token;
    omp_unset_lock(&g_token_lock);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,    &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,      10L); 
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L); 

    int max_reintentos = 3;
    Genero genero = Genero::DESCONOCIDO;
    bool exito = false;

    for (int intento = 1; intento <= max_reintentos && !exito; intento++) {
        resp.data.clear();
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) {
            std::string g = json_get(resp.data, "gender");
            if      (g == "FEMENINO")  genero = Genero::FEMENINO;
            else if (g == "MASCULINO") genero = Genero::MASCULINO;
            else log_err("Gender desconocido uuid=" + uuid + " val=" + g);
            exito = true;
        } 
        else if (http_code == 401) {
            // Token expirado en medio de una peticion: forzar renovacion inmediata
            log_err("Token expirado (401), renovando...");
            omp_set_lock(&g_token_lock);
            g_token_exp = 0;
            omp_unset_lock(&g_token_lock);
            asegurar_token();
            
            // Actualizar header con el nuevo token antes de reintentar
            omp_set_lock(&g_token_lock);
            auth = "Authorization: Bearer " + g_token;
            omp_unset_lock(&g_token_lock);
            
            curl_slist_free_all(headers);
            headers = nullptr;
            headers = curl_slist_append(headers, auth.c_str());
            headers = curl_slist_append(headers, "accept: application/json");
        } 
        else {
            if (intento == max_reintentos) {
                // Fallo definitivo: UUID quedara como DESCONOCIDO
                log_err("Fallo definitivo tras " + std::to_string(max_reintentos) + 
                        " intentos. uuid=" + uuid + " HTTP " + std::to_string(http_code) + 
                        " CURL: " + curl_easy_strerror(res));
            } else {
                usleep(500000); // 500ms de espera entre reintentos
            }
        }
    }

    curl_slist_free_all(headers);

    // 4. Guardar en cache RAM para evitar consultas repetidas del mismo UUID
    if (genero != Genero::DESCONOCIDO) {
        omp_set_lock(&g_cache_lock);
        g_cache[uuid] = genero;
        omp_unset_lock(&g_cache_lock);
    }

    return genero;
}

} // namespace api