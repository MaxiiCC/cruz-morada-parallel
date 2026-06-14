#pragma once
#include <vector>
#include <string>

struct Transaccion {
    std::string fecha;
    std::string canal;
    int         sku;
    std::string producto;
    int         unidades;
    double      porcentaje_descuento;
    double      monto_aplicado;
    long        boleta;
    std::string local;
    std::string codigo_cliente;
};

namespace csv {
    std::vector<Transaccion> parsear(const std::string& ruta);
}