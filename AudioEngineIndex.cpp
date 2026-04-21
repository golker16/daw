/*
 * AudioEngineIndex.cpp
 * --------------------
 * Punto de entrada compacto del motor de audio.
 *
 * Objetivo:
 * - Tener un archivo pequeno que describa el motor sin exponer de inmediato sus miles
 *   de lineas de implementacion.
 * - Centralizar la inclusion de modulos profundos numerados:
 *   AudioEngine1.cpp, AudioEngine2.cpp, AudioEngine3.cpp, etc.
 *
 * Mapa actual:
 * - AudioEngine.h: API publica, estados, snapshots y estructuras del motor.
 * - AudioEngineIndex.h: resumen detallado y guia de extension.
 * - AudioEngine1.cpp: implementacion profunda actual.
 *
 * Pipeline principal:
 * 1. initialize() prepara config, estado de proyecto y backend.
 * 2. start() levanta audio thread, mantenimiento, telemetria y workers auxiliares.
 * 3. El callback renderiza bloques live y trabajo anticipativo.
 * 4. Se aplican automatizacion, PDC, mezcla por buses y cache de clips.
 * 5. La UI consume snapshots atomicos y comandos del motor.
 *
 * Areas funcionales contenidas hoy en AudioEngine1.cpp:
 * - Ciclo de vida del motor y backend.
 * - Transporte.
 * - Grafo de audio y compilacion.
 * - Automatizacion y compensacion de latencia.
 * - Plugins, sandbox y watchdogs.
 * - Offline render, cache de clips, proyecto/undo-redo.
 *
 * Regla para futuras divisiones:
 * - AudioEngine1.cpp: lifecycle/backend/core timing.
 * - AudioEngine2.cpp: graph/render.
 * - AudioEngine3.cpp: project editing/serialization.
 * - AudioEngine4.cpp: plugins/sandbox/offline render.
 */

#include "AudioEngineIndex.h"
#include "AudioEngine1.cpp"
