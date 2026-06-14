#include "../include/sftp.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <vector>
#include <string>
#include <omp.h>

#define SFTP_HOST "137.184.45.251"
#define SFTP_PORT 22
#define SFTP_USER "utem"
#define SFTP_PASS "CPyD.2026"
#define SFTP_ROOT "/"
#define LOCAL_DIR "csv_files"

static void log_err(const std::string& msg) {
    // PROTECCIÓN: Evita que los hilos choquen al escribir en el log
    #pragma omp critical
    {
        FILE* f = fopen("log.txt", "a");
        if (f) { fprintf(f, "[SFTP] %s\n", msg.c_str()); fclose(f); }
        std::cerr << "[SFTP] " << msg << "\n";
    }
}

static int conectar_tcp() {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof(port), "%d", SFTP_PORT);
    if (getaddrinfo(SFTP_HOST, port, &hints, &res) != 0) {
        log_err("No se pudo resolver host");
        return -1;
    }
    int sock = -1;
    for (auto* p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock); sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) log_err("No se pudo conectar al servidor SFTP");
    return sock;
}

static int bajar_archivo(LIBSSH2_SFTP* sftp, const char* remoto, const char* local) {
    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, remoto, LIBSSH2_FXF_READ, 0);
    if (!fh) { log_err(std::string("No se pudo abrir: ") + remoto); return -1; }

    FILE* out = fopen(local, "wb");
    if (!out) {
        log_err(std::string("No se pudo crear: ") + local);
        libssh2_sftp_close(fh);
        return -1;
    }

    char buf[65536];
    ssize_t n;
    while ((n = libssh2_sftp_read(fh, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, n, out);

    fclose(out);
    libssh2_sftp_close(fh);
    return (n < 0) ? -1 : 0;
}

namespace sftp {

int descargar_archivos() {
    mkdir(LOCAL_DIR, 0755);

    if (libssh2_init(0) != 0) { log_err("libssh2_init fallo"); return -1; }

    // ==========================================================
    // FASE 1: Escaneo Secuencial (Obtener la lista de faltantes)
    // ==========================================================
    int sock = conectar_tcp();
    if (sock < 0) { libssh2_exit(); return -1; }

    LIBSSH2_SESSION* session = libssh2_session_init();
    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, sock) != 0) {
        log_err("Handshake SSH fallo");
        libssh2_session_free(session); close(sock); libssh2_exit(); return -1;
    }

    if (libssh2_userauth_password(session, SFTP_USER, SFTP_PASS) != 0) {
        log_err("Autenticacion SSH fallida");
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session); close(sock); libssh2_exit(); return -1;
    }

    LIBSSH2_SFTP* sftp_sess = libssh2_sftp_init(session);
    if (!sftp_sess) {
        log_err("No se pudo iniciar sesion SFTP");
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session); close(sock); libssh2_exit(); return -1;
    }

    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(sftp_sess, SFTP_ROOT);
    if (!dir) {
        log_err("No se pudo abrir directorio raiz");
        libssh2_sftp_shutdown(sftp_sess);
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session); close(sock); libssh2_exit(); return -1;
    }

    int descargados_previos = 0;
    std::vector<std::string> faltantes;
    char nombre[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    std::cout << "[INFO] Escaneando servidor remoto...\n";

    while (libssh2_sftp_readdir(dir, nombre, sizeof(nombre), &attrs) > 0) {
        size_t len = strlen(nombre);
        if (len < 12) continue;
        if (strncmp(nombre, "reporte_", 8) != 0) continue;
        if (strcmp(nombre + len - 4, ".csv") != 0) continue;

        char local[600];
        snprintf(local, sizeof(local), "%s/%s", LOCAL_DIR, nombre);

        FILE* test = fopen(local, "r");
        if (test) {
            fclose(test);
            // Comentado para no spamear la consola si ya hay 900 archivos
            // std::cout << "[SFTP] Ya existe: " << nombre << "\n";
            descargados_previos++;
        } else {
            faltantes.push_back(std::string(nombre));
        }
    }

    // Cerramos la conexion de escaneo
    libssh2_sftp_closedir(dir);
    libssh2_sftp_shutdown(sftp_sess);
    libssh2_session_disconnect(session, "Normal shutdown");
    libssh2_session_free(session);
    close(sock);

    int total_faltantes = faltantes.size();
    
    if (total_faltantes == 0) {
        std::cout << "[SFTP] Entorno al dia. No hay archivos nuevos.\n";
        std::cout << "[SFTP] Total archivos en disco: " << descargados_previos << "\n";
        libssh2_exit();
        return descargados_previos;
    }

    std::cout << "[SFTP] Se detectaron " << total_faltantes << " archivos nuevos.\n";
    std::cout << "[SFTP] Iniciando descarga paralela (Limite: 8 hilos)...\n";

    // ==========================================================
    // FASE 2: Descarga Paralela de archivos faltantes
    // ==========================================================
    int descargas_nuevas = 0;

    // 8 hilos para no saturar el MaxStartups del servidor
    #pragma omp parallel for schedule(dynamic, 1) num_threads(8) reduction(+:descargas_nuevas)
    for (int i = 0; i < total_faltantes; i++) {
        std::string archivo = faltantes[i];
        char local[600];
        char remoto[600];
        snprintf(local, sizeof(local), "%s/%s", LOCAL_DIR, archivo.c_str());
        snprintf(remoto, sizeof(remoto), "%s%s", SFTP_ROOT, archivo.c_str());

        // Cada hilo crea SU propia conexion
        int t_sock = conectar_tcp();
        if (t_sock < 0) continue;

        LIBSSH2_SESSION* t_session = libssh2_session_init();
        libssh2_session_set_blocking(t_session, 1);

        if (libssh2_session_handshake(t_session, t_sock) != 0 || 
            libssh2_userauth_password(t_session, SFTP_USER, SFTP_PASS) != 0) {
            libssh2_session_free(t_session); close(t_sock); continue;
        }

        LIBSSH2_SFTP* t_sftp = libssh2_sftp_init(t_session);
        if (t_sftp) {
            if (bajar_archivo(t_sftp, remoto, local) == 0) {
                #pragma omp critical
                {
                    std::cout << "[Hilo " << omp_get_thread_num() << "] Descargó: " << archivo << "\n";
                }
                descargas_nuevas++;
            }
            libssh2_sftp_shutdown(t_sftp);
        }
        
        libssh2_session_disconnect(t_session, "Normal shutdown");
        libssh2_session_free(t_session);
        close(t_sock);
    }

    libssh2_exit();

    int total_final = descargados_previos + descargas_nuevas;
    std::cout << "[SFTP] Total: " << total_final << " archivos disponibles\n";
    return total_final;
}

} // namespace sftp