/*
 * PAudioEngineIndex.cpp
 * ---------------------
 * Punto de entrada pequeno y compartible del motor de audio.
 *
 * Convencion:
 * - Archivos P*: contexto portable, contratos e indices para compartir con IA.
 * - Archivos AudioEngineDev*.cpp: implementacion profunda y pesada.
 *
 * Pipeline resumido:
 * 1. initialize() prepara config, backend y estado de proyecto.
 * 2. start() levanta hilos de audio, mantenimiento y soporte.
 * 3. El motor procesa bloques live y anticipativos.
 * 4. La UI consume snapshots consistentes publicados por el engine.
 *
 * Expansion futura:
 * - Mantener este archivo corto.
 * - Agregar aqui AudioEngineDev2.cpp, AudioEngineDev3.cpp, etc.
 * - Reservar AudioEngineDev1.cpp para el bloque historico o core inicial.
 */

#include "PAudioEngineIndex.h"
#include "AudioEngineDev1.cpp"
// Add future engine modules here, for example:
// #include "AudioEngineDev2.cpp"
// #include "AudioEngineDev3.cpp"
