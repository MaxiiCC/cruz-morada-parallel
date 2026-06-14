# Cruz Morada - Procesamiento Paralelo de Ventas (OpenMP)

Proyecto del curso Computación Paralela y Distribuida - UTEM.

Programa en C++ que descarga reportes de ventas desde un servidor SFTP,
consulta una API REST para obtener el género de cada cliente, y calcula
el promedio de monto de compra por género (FEMENINO / MASCULINO),
utilizando paralelismo con OpenMP.

---

## ⚠️ PASO OBLIGATORIO ANTES DE COMPILAR

El repositorio incluye una caché pre-poblada con los géneros de ~2.9
millones de clientes (`cache_generos.zip`), necesaria para que el
procesamiento se ejecute en segundos en lugar de horas (sin caché,
cada cliente nuevo requiere una consulta HTTP a la API).

**Antes de compilar, descomprime la caché en la raíz del proyecto:**

```bash
unzip cache_generos.zip
```

Esto genera `cache_generos.txt` en la raíz del proyecto (mismo nivel
que el `Makefile`).

---

## Requisitos

- GCC/G++ con soporte OpenMP y C++17 (compatible con GCC 13, incluido
  por defecto en Ubuntu 24.04 LTS)
- libssh2 (con headers de desarrollo)
- libcurl (con headers de desarrollo)
- make

En Ubuntu 24.04:
```bash
sudo apt install build-essential libssh2-1-dev libcurl4-openssl-dev
```

---

## Compilación

```bash
make clean && make
```

Esto genera el ejecutable `cruz_morada` en la raíz del proyecto.

---

## Ejecución

```bash
export OMP_NUM_THREADS=128
./cruz_morada
```

> Se recomienda un número alto de hilos (ej. 128) porque el programa
> es I/O-bound: la mayoría del tiempo los hilos esperan respuestas de
> red (SFTP/API), no realizan cómputo intensivo. Esto permite muchas
> más operaciones de E/S concurrentes que el número de núcleos físicos.

### Qué hace el programa al ejecutarse

1. **Conexión SFTP**: se conecta al servidor remoto. Primero escanea
   secuencialmente qué archivos `reporte_*.csv` faltan en
   `csv_files/`, y luego los descarga en paralelo (8 hilos, cada uno
   con su propia sesión SSH/SFTP).
2. **Parseo de CSV**: cada archivo se parsea y valida en paralelo
   (`#pragma omp parallel for` con `schedule(dynamic)`).
3. **Consulta API REST**: para cada `CODIGO CLIENTE` único, se
   consulta el género vía API (autenticación JWT, con renovación
   automática de token). Los resultados se cachean en RAM
   (`unordered_map` + lock) y se persisten en disco
   (`cache_generos.txt`) para evitar consultas redundantes.
4. **Cálculo de métricas**: promedio de `MONTO APLICADO` por género,
   acumulado mediante `reduction` de OpenMP (sin locks explícitos
   sobre los acumuladores).
5. **Resultados**: se imprimen en consola y se guardan en
   `resultados.txt`. Los errores (filas malformadas, fallos de red,
   tokens expirados, etc.) se registran en `log.txt`.

---


## Estructura del proyecto