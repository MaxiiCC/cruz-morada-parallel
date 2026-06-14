#include "../include/sftp.h"
#include "../include/csv.h"
#include "../include/api.h"
#include <omp.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>

#define RUT   "21.371.052-4"
#define EMAIL "mcamposc@utem.cl"

namespace fs = std::filesystem;

static void log_err(const std::string& msg) {
    FILE* f = fopen("log.txt", "a");
    if (f) { fprintf(f, "[MAIN] %s\n", msg.c_str()); fclose(f); }
    std::cerr << "[MAIN] " << msg << "\n";
}

static std::vector<std::string> listar_csvs(const std::string& dir) {
    std::vector<std::string> rutas;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto& p = entry.path();
        std::string nombre = p.filename().string();
        if (nombre.rfind("reporte_", 0) == 0 && p.extension() == ".csv")
            rutas.push_back(p.string());
    }
    std::sort(rutas.begin(), rutas.end());
    return rutas;
}

int main() {
    double t_inicio = omp_get_wtime();

    // ── 1. Descarga SFTP ─────────────────────────────────────────────
    // Conecta al servidor y descarga solo los archivos reporte_*.csv
    // que aun no existan localmente en csv_files/. Si el volumen de
    // datos del servidor crece, esta funcion trae automaticamente
    // los archivos nuevos en la siguiente ejecucion.
    std::cout << "[INFO] Conectando SFTP...\n";
    int desc = sftp::descargar_archivos();
    if (desc <= 0) {
        log_err("No se descargaron archivos (revisar conexion SFTP)");
        return 1;
    }

    // ── 2. Listar CSVs ───────────────────────────────────────────────
    auto archivos = listar_csvs("csv_files");
    if (archivos.empty()) { log_err("No hay CSVs en csv_files/"); return 1; }
    int n = (int)archivos.size();
    std::cout << "[INFO] " << n << " archivos CSV\n";
    std::cout << "[INFO] Hilos: " << omp_get_max_threads() << "\n";

    // ── 3. Token JWT ─────────────────────────────────────────────────
    api::init(RUT, EMAIL);

    // ── 4. Procesamiento paralelo ────────────────────────────────────
    // Cada hilo toma un archivo CSV (schedule dynamic balancea archivos
    // de tamanos muy distintos), lo parsea y consulta el genero de cada
    // cliente vía API (con cache). Los acumuladores se combinan al final
    // mediante reduction, evitando condiciones de carrera sin usar locks
    // explicitos sobre las variables de suma/conteo.
    double suma_f  = 0.0, suma_m  = 0.0;
    long   count_f = 0,   count_m = 0;

    #pragma omp parallel for schedule(dynamic, 1) \
        reduction(+: suma_f, suma_m, count_f, count_m)
    for (int i = 0; i < n; i++) {
        int tid = omp_get_thread_num();
        auto transacciones = csv::parsear(archivos[i]);
        if (transacciones.empty()) continue;

        std::cout << "[Hilo " << tid << "] " << archivos[i]
                  << " -> " << transacciones.size() << " tx\n";

        double lsf = 0.0, lsm = 0.0;
        long   lcf = 0,   lcm = 0;

        for (const auto& t : transacciones) {
            Genero g = api::consultar(t.codigo_cliente);
            if      (g == Genero::FEMENINO)  { lsf += t.monto_aplicado; lcf++; }
            else if (g == Genero::MASCULINO) { lsm += t.monto_aplicado; lcm++; }
        }

        suma_f  += lsf; suma_m  += lsm;
        count_f += lcf; count_m += lcm;
    }

    // ── 5. Resultados ────────────────────────────────────────────────
    double prom_f = (count_f > 0) ? suma_f / count_f : 0.0;
    double prom_m = (count_m > 0) ? suma_m / count_m : 0.0;
    double tiempo = omp_get_wtime() - t_inicio;

    std::cout << "\n=== RESULTADOS ===\n";
    std::cout << "FEMENINO  = " << (long)prom_f << "\n";
    std::cout << "MASCULINO = " << (long)prom_m << "\n";
    std::cout << "TIEMPO    = " << tiempo << " segundos\n";
    std::cout << "==================\n";
    std::cout << "[DEBUG] TX Femenino: " << count_f << " | Masculino: " << count_m << "\n";

    std::ofstream out("resultados.txt");
    if (out.is_open()) {
        out << "FEMENINO = "  << (long)prom_f << "\n";
        out << "MASCULINO = " << (long)prom_m << "\n";
        out << "TIEMPO = "    << tiempo << " segundos\n";
        out.close();
        std::cout << "[OK] resultados.txt guardado\n";
    } else {
        log_err("No se pudo escribir resultados.txt");
    }

    api::cleanup();
    return 0;
}