#include "../include/csv.h"
#include <fstream>
#include <iostream>
#include <cstdio>

// Registra errores de parseo en log.txt con prefijo [CSV].
// Se abre y cierra el archivo en cada llamada para ser seguro
// cuando multiples hilos escriben concurrentemente (modo append).
static void log_err(const std::string& archivo, const std::string& msg) {
    FILE* f = fopen("log.txt", "a");
    if (f) { fprintf(f, "[CSV] %s: %s\n", archivo.c_str(), msg.c_str()); fclose(f); }
    std::cerr << "[CSV] " << archivo << ": " << msg << "\n";
}

// Elimina comillas dobles envolventes de un campo CSV si las tiene.
static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// Divide una linea CSV por el separador ';', respetando campos entre
// comillas dobles (donde ';' puede aparecer sin ser separador).
static std::vector<std::string> split_linea(const std::string& linea) {
    std::vector<std::string> campos;
    std::string campo;
    bool en_comillas = false;

    for (char c : linea) {
        if (c == '"') {
            en_comillas = !en_comillas;
            campo += c;
        } else if (c == ';' && !en_comillas) {
            campos.push_back(strip_quotes(campo));
            campo.clear();
        } else {
            campo += c;
        }
    }
    if (!campo.empty())
        campos.push_back(strip_quotes(campo));

    return campos;
}

// Nombres exactos de las 10 columnas esperadas, en el orden exacto.
// Si el archivo no coincide campo a campo, se descarta completo.
static const std::vector<std::string> HEADERS_ESPERADOS = {
    "FECHA", "CANAL", "SKU", "PRODUCTO", "UNIDADES",
    "PORCENTAJE DESCUENTO", "MONTO APLICADO", "BOLETA", "LOCAL", "CODIGO CLIENTE"
};

namespace csv {

std::vector<Transaccion> parsear(const std::string& ruta) {
    std::vector<Transaccion> resultado;

    std::ifstream archivo(ruta);
    if (!archivo.is_open()) {
        log_err(ruta, "No se pudo abrir");
        return resultado;
    }

    std::string linea;

    // Validar headers
    if (!std::getline(archivo, linea)) {
        log_err(ruta, "Archivo vacio");
        return resultado;
    }
    // Eliminar \r para compatibilidad con archivos generados en Windows
    if (!linea.empty() && linea.back() == '\r') linea.pop_back();

    auto headers = split_linea(linea);
    if (headers.size() != HEADERS_ESPERADOS.size()) {
        log_err(ruta, "Numero de columnas incorrecto: " + std::to_string(headers.size()));
        return resultado;
    }
    for (size_t i = 0; i < HEADERS_ESPERADOS.size(); i++) {
        if (headers[i] != HEADERS_ESPERADOS[i]) {
            log_err(ruta, "Header incorrecto: esperado '" + HEADERS_ESPERADOS[i] +
                    "' encontrado '" + headers[i] + "'");
            return resultado;
        }
    }

    // Pre-reserva para evitar realocaciones durante el parseo.
    // 30000 es una cota superior razonable de transacciones por archivo diario.
    resultado.reserve(30000);

    size_t fila = 1;
    while (std::getline(archivo, linea)) {
        fila++;
        if (!linea.empty() && linea.back() == '\r') linea.pop_back();
        if (linea.empty()) continue;

        auto campos = split_linea(linea);
        // Filas con numero de campos incorrecto se descartan individualmente
        if (campos.size() != 10) {
            log_err(ruta, "Fila " + std::to_string(fila) + " malformada (" +
                    std::to_string(campos.size()) + " campos)");
            continue;
        }

        Transaccion t;
        try {
            t.fecha                = campos[0];
            t.canal                = campos[1];
            t.sku                  = std::stoi(campos[2]);
            t.producto             = campos[3];
            t.unidades             = std::stoi(campos[4]);
            t.porcentaje_descuento = std::stod(campos[5]);
            t.monto_aplicado       = std::stod(campos[6]);
            t.boleta               = std::stol(campos[7]);
            t.local                = campos[8];
            t.codigo_cliente       = campos[9];
        } catch (const std::exception& e) {
            // Error de conversion de tipo: fila descartada, resto del archivo continua
            log_err(ruta, "Fila " + std::to_string(fila) +
                    " error conversion: " + e.what());
            continue;
        }

        resultado.push_back(std::move(t));
    }

    return resultado;
}

} // namespace csv