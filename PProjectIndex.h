#pragma once

/*
 * PProjectIndex.h
 * ---------------
 * Mapa maestro y convencion del proyecto para trabajar con IAs sin gastar tokens
 * cargando primero los archivos mas grandes.
 *
 * Regla de nombres:
 * - P*: archivo prioritario, portable y compartible.
 * - *Dev*.cpp: implementacion profunda, pesada o historica.
 *
 * Entrada recomendada para contexto rapido:
 * - PProjectIndex.h
 * - PMain.cpp
 * - PUIIndex.h
 * - PUIIndex.cpp
 * - PAudioEngineIndex.h
 * - PAudioEngineIndex.cpp
 *
 * Entrada solo cuando hace falta API publica:
 * - PUI.h
 * - PAudioEngine.h
 *
 * Entrada solo cuando hace falta desarrollo profundo:
 * - UIDev1.cpp
 * - AudioEngineDev1.cpp
 * - Futuros UIDev2.cpp / AudioEngineDev2.cpp / etc.
 *
 * Regla de crecimiento:
 * - No volver a crear wrappers UI.cpp o AudioEngine.cpp.
 * - Mantener los indices P... cortos y descriptivos.
 * - Acoplar nuevos modulos profundos agregandolos en PUIIndex.cpp o PAudioEngineIndex.cpp.
 */
