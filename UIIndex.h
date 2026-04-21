#pragma once

/*
 * UIIndex.h
 * ---------
 * Sintesis detallada del modulo UI para compartir contexto con una IA sin subir
 * la implementacion completa.
 *
 * Contrato base:
 * - Clase central: UI
 * - Dependencia principal: AudioEngine&
 * - Tecnologia: Win32 + GDI + controles clasicos
 *
 * Capas conceptuales:
 * - Snapshot visible:
 *   UI transforma AudioEngine::EngineSnapshot en VisibleEngineState.
 * - Workspace:
 *   Mantiene paneo visible, patron activo, zoom, herramientas y foco.
 * - Superficies:
 *   Browser, Channel Rack, Piano Roll, Playlist, Mixer y Plugin surface.
 * - Interaccion:
 *   Mouse y comandos traducidos a seleccion, drag, resize y acciones de engine.
 * - Ventanas auxiliares:
 *   Plugin manager y paneles desacoplados.
 *
 * Tipos clave expuestos por UI.h:
 * - VisibleClip / VisibleTrack / VisibleBus / VisibleProjectState
 * - VisibleEngineState
 * - PluginManagerState
 * - SurfaceInteractionState
 * - WorkspaceState / WorkspaceModel / PatternState
 *
 * Dependencias relevantes desde UI hacia motor:
 * - Lectura:
 *   getUiSnapshot(), getProjectSnapshot(), getLoadedPluginDescriptors(), getTransportInfo()
 * - Escritura:
 *   start(), stop(), play(), stopTransport(), pause()
 *   addTrack(), addBus(), addClipToTrack(), moveClip()
 *   setTempo(), requestGraphRebuild(), renderOffline()
 *   saveProject(), loadProject(), undoLastEdit(), redoLastEdit()
 *
 * Puntos calientes para refactor futuro:
 * - Constructor UI::UI(...) concentra mucho bootstrap.
 * - buildVisibleEngineState() mezcla adaptacion de snapshot y logica de presentacion.
 * - Manejo de superficies y dibujo viven en el mismo modulo grande.
 * - Plugin manager y ventanas detached son buenos candidatos a archivo separado.
 *
 * Convencion recomendada para crecer sin gastar tokens:
 * - Compartir UIIndex.h + UIIndex.cpp para contexto.
 * - Compartir solo el modulo numerado puntual cuando el cambio sea profundo.
 * - Mantener cada nuevo UI<n>.cpp enfocado a un area funcional unica.
 */
