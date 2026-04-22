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
 * Organizacion actual:
 * - PAudioEngineIndex.cpp incluye AudioEngineDev1.cpp.
 * - AudioEngineDev1.cpp encadena AudioEngineDev2.cpp y AudioEngineDev3.cpp.
 * - Asi mantenemos una sola unidad de compilacion y evitamos romper el build.
 */

#include "PAudioEngineIndex.h"
#include "AudioEngineDev1.cpp"
