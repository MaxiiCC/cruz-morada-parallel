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

- GCC/G++ con soporte OpenMP y C++17 (probado con GCC 13.3.0,
  incluido por defecto en Ubuntu 24.04 LTS)
- libssh2 (con headers de desarrollo)
- libcurl (con headers de desarrollo)
- make

En Ubuntu 24.04:
```bash
sudo apt install build-essential libssh2-1-dev libcurl4-openssl-dev
```

---

## Compilación

> Asegúrate de haber instalado las dependencias de la sección anterior
> antes de compilar.

```bash
make clean && make
```

Esto genera el ejecutable `cruz_morada` en la raíz del proyecto.

---

## Ejecución

```bash
./cruz_morada
```

> Por defecto, el programa utiliza el número de hilos lógicos
> disponibles en el sistema (`omp_get_max_threads()`). La descarga
> SFTP está limitada internamente a 8 hilos (ver "Notas sobre
> paralelismo"), independientemente del valor de `OMP_NUM_THREADS`.

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

## Tiempos de ejecución

- **Descarga SFTP inicial** (~910 archivos, ~2GB, 8 hilos paralelos):
  el tiempo depende del ancho de banda disponible hacia el servidor
  SFTP. En nuestras pruebas tomó aproximadamente 15 minutos.
- **Procesamiento** (parseo de ~15.4M transacciones + cálculo de
  promedios con `reduction`): ~22-34 segundos en una máquina con
  múltiples cores físicos.
- **Ejecuciones posteriores**: si los CSV ya están en `csv_files/` y
  `cache_generos.txt` ya existe, solo se ejecuta la fase de
  procesamiento (no hay descarga ni consultas a la API).

## Notas sobre paralelismo

- **Descarga SFTP**: limitada a 8 hilos por diseño. `libssh2` no
  permite múltiples hilos sobre la misma conexión, y un número mayor
  de conexiones simultáneas arriesga ser bloqueado por el firewall
  del servidor.
- **Parseo y cálculo**: escalan con el número de cores disponibles
  (`omp_get_max_threads()`), usando `schedule(dynamic)` para balancear
  archivos de distinto tamaño y `reduction` para acumular resultados
  sin necesidad de secciones críticas.

---

## Estructura del proyecto