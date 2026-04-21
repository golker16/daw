/*
 * PUIIndex.cpp
 * ------------
 * Punto de entrada pequeno y compartible del subsistema UI.
 *
 * Convencion:
 * - Archivos P*: contexto portable, contratos e indices para compartir con IA.
 * - Archivos UIDev*.cpp: implementacion profunda y pesada.
 *
 * Flujo:
 * 1. PMain.cpp crea UI con una referencia a AudioEngine.
 * 2. La UI adapta snapshots del motor a un modelo visual.
 * 3. Los handlers Win32 transforman input en acciones de editor.
 * 4. Este index agrega uno o mas modulos UIDev*.cpp al build.
 *
 * Expansion futura:
 * - Mantener este archivo corto.
 * - Agregar aqui nuevos modulos como UIDev2.cpp, UIDev3.cpp, etc.
 * - Reservar UIDev1.cpp para el bloque historico o core inicial.
 */

#include "PUIIndex.h"
#include "UIDev1.cpp"
#include "UIDev2.cpp"
#include "UIDev3.cpp"
