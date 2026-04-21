/*
 * UIIndex.cpp
 * -----------
 * Punto de entrada compacto del subsistema de interfaz.
 *
 * Objetivo:
 * - Permitir compartir con otra IA un archivo corto que explique la arquitectura de UI
 *   sin tener que adjuntar el bloque completo de implementacion.
 * - Mantener una puerta de entrada estable para futuras particiones:
 *   UI1.cpp, UI2.cpp, UI3.cpp, etc.
 *
 * Mapa actual:
 * - UI.h: contrato publico, tipos visibles y estado persistente de la UI.
 * - UIIndex.h: sintesis detallada para contexto rapido.
 * - UI1.cpp: implementacion profunda actual de la ventana Win32 y las superficies.
 *
 * Flujo general:
 * 1. main.cpp crea UI con una referencia a AudioEngine.
 * 2. UI registra clases Win32, construye la ventana principal y los paneles.
 * 3. Un temporizador refresca el estado visible desde AudioEngine.
 * 4. Los handlers de mensajes convierten input Win32 en acciones de workspace.
 * 5. La UI recompone vistas derivadas: browser, piano roll, playlist, mixer y plugin manager.
 *
 * Responsabilidades de UI1.cpp:
 * - Bootstrap Win32.
 * - Layout de controles y paneles.
 * - Pintado GDI.
 * - Interaccion del usuario en superficies.
 * - Sincronizacion entre modelo visual y snapshot del motor.
 *
 * Regla para futuras divisiones:
 * - UI1.cpp puede quedar como "core bootstrap + message pump".
 * - UI2.cpp puede alojar layout/pintado.
 * - UI3.cpp puede alojar playlist/piano roll/browser.
 * - UI4.cpp puede alojar plugin manager y ventanas desacopladas.
 */

#include "UIIndex.h"
#include "UI1.cpp"
