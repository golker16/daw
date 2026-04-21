#pragma once

/*
 * AudioEngineIndex.h
 * ------------------
 * Sintesis detallada del modulo AudioEngine para contexto rapido.
 *
 * Contrato base:
 * - Clase central: AudioEngine
 * - Rol: backend de audio, transporte, grafo, proyecto y snapshots para UI
 * - Modelo: estado atomico + buffers + workers + snapshots double-buffered
 *
 * Areas publicas en AudioEngine.h:
 * - Ciclo de vida:
 *   initialize(), shutdown(), start(), stop(), initializeAudioBackend(), recoverAudioDevice()
 * - Transporte:
 *   play(), pause(), stopTransport(), setTempo(), setTimelinePosition(), setSamplePosition()
 * - Grafo/render:
 *   buildInitialGraph(), compileGraph(), requestGraphRebuild(), processLiveBlock()
 * - Automatizacion/PDC:
 *   enqueueAutomationEvent(), applyAutomationForBlock(), recalculateLatencyModel()
 * - Proyecto:
 *   addTrack(), addBus(), addClipToTrack(), moveClip(), newProject(), saveProject(), loadProject()
 *   undoLastEdit(), redoLastEdit()
 * - Integracion UI:
 *   getUiSnapshot(), getProjectSnapshot(), getMetrics(), getTransportInfo()
 *
 * Estado interno importante:
 * - config_, state_, initialized_, running_
 * - deviceState_, audioThreadState_, realtimeBuffer_, anticipativeBuffer_
 * - projectState_, undoStack_, redoStack_
 * - editableGraph_, pendingGraph_, compiledGraph_
 * - automationLanes_, latencyStates_, delayLines_
 * - loadedPlugins_, clipCache_, diskReadQueue_
 * - commandQueue_ y uiSnapshots_
 *
 * Flujo de datos resumido:
 * - UI postea comandos o llama API de alto nivel.
 * - Engine actualiza proyecto/transport/config.
 * - Audio thread procesa bloques y mezcla nodos.
 * - Maintenance/disk/plugin workers sostienen cache, sandbox y tareas auxiliares.
 * - publishSnapshot() deja una vista consistente para UI.
 *
 * Puntos de corte recomendados:
 * - AudioEngine1.cpp: bootstrap, backend, transporte y snapshots.
 * - AudioEngine2.cpp: render graph, mezcla, automation y PDC.
 * - AudioEngine3.cpp: proyecto, serializacion, undo/redo.
 * - AudioEngine4.cpp: plugins, sandbox, offline render y disk streaming.
 *
 * Convencion recomendada para ahorrar contexto:
 * - Compartir AudioEngineIndex.h + AudioEngineIndex.cpp como mapa general.
 * - Adjuntar solo AudioEngine<n>.cpp del area concreta a modificar.
 */
