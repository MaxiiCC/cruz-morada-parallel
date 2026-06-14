#pragma once
#include <string>

enum class Genero { FEMENINO, MASCULINO, DESCONOCIDO };

namespace api {
    // Inicializa curl y obtiene token JWT
    void init(const std::string& rut, const std::string& email);

    // Consulta /v1/person/{uuid}, con cache thread-safe y renovacion de token
    Genero consultar(const std::string& uuid);

    // Libera recursos
    void cleanup();
}