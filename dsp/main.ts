import {Renderer, el} from '@elemaudio/core';
import type {ElemNode} from '@elemaudio/core';
import {parseDspState, parseJsonObject, type HydratedNode, type DspState, type JsonValue} from './types';

// First, we initialize a custom Renderer instance that marshals our instruction
// batches through the __postNativeMessage__ function to direct the underlying native
// engine.
type RenderBatch = JsonValue[];

const core = new Renderer((batch: RenderBatch) => {
  globalThis.__postNativeMessage__?.(JSON.stringify(batch));
});

// Holding onto the previous state allows us a quick way to differentiate
// when we need to fully re-render versus when we can just update refs
let prevState: DspState | null = null;

const maxVoices = 16;
const maxGrains = 128;
const grainControlOffset = maxVoices * 4;
const grainPositionOffset = grainControlOffset;
const grainPhaseOffset = grainControlOffset + maxGrains;
const grainLeftGainOffset = grainControlOffset + maxGrains * 2;
const grainRightGainOffset = grainControlOffset + maxGrains * 3;

function readNumber(state: DspState, key: string, fallback: number): number {
  const value = state[key];
  return typeof value === 'number' ? value : fallback;
}

function readBoolean(state: DspState, key: string, fallback: boolean): boolean {
  const value = state[key];
  return typeof value === 'boolean' ? value : fallback;
}

function readTransportBpm(state: DspState): number {
  const transport = state.transport;

  if (typeof transport !== 'object' || transport === null || Array.isArray(transport))
    return 120;

  const bpm = transport.bpm;
  return typeof bpm === 'number' && Number.isFinite(bpm) && bpm > 0 ? bpm : 120;
}

function effectiveDensity(state: DspState): number {
  if (!readBoolean(state, 'syncEnabled', false))
    return Math.max(1, readNumber(state, 'density', 20));

  const divisionQuarterNotes = [0.125, 0.25, 0.5, 1, 2, 4];
  const index = Math.max(0, Math.min(
    divisionQuarterNotes.length - 1,
    Math.round(readNumber(state, 'densityDivision', 2)),
  ));
  return (readTransportBpm(state) / 60) / divisionQuarterNotes[index];
}

function readSampleString(state: DspState, key: 'status' | 'resourceId'): string {
  const value = state.sample?.[key];
  return typeof value === 'string' ? value : '';
}

function graphSignature(state: DspState): string {
  return JSON.stringify({
    sampleRate: state.sampleRate,
    sampleStatus: readSampleString(state, 'status'),
    resourceId: readSampleString(state, 'resourceId'),
    windowType: readNumber(state, 'windowType', 0),
    density: effectiveDensity(state),
    grainSize: readNumber(state, 'grainSize', 100),
    outputGain: readNumber(state, 'outputGain', -6),
  });
}

function shouldRender(prevState: DspState | null, nextState: DspState) {
  return prevState === null || graphSignature(prevState) !== graphSignature(nextState);
}

function blackman(phase: ElemNode): ElemNode {
  return el.add(
    0.42,
    el.mul(-0.5, el.cos(el.mul(2 * Math.PI, phase))),
    el.mul(0.08, el.cos(el.mul(4 * Math.PI, phase))),
  );
}

function tukey(phase: ElemNode): ElemNode {
  const alpha = 0.5;
  const left = el.mul(
    0.5,
    el.add(1, el.cos(el.mul(Math.PI, el.sub(el.div(el.mul(2, phase), alpha), 1)))),
  );
  const right = el.mul(
    0.5,
    el.add(
      1,
      el.cos(el.mul(
        Math.PI,
        el.add(el.sub(el.div(el.mul(2, phase), alpha), el.div(2, alpha)), 1),
      )),
    ),
  );

  return el.select(
    el.le(phase, alpha / 2),
    left,
    el.select(el.ge(phase, 1 - alpha / 2), right, 1),
  );
}

function grainWindow(windowType: number, phase: ElemNode): ElemNode {
  if (windowType === 1)
    return blackman(phase);

  if (windowType === 2)
    return tukey(phase);

  return el.hann(phase);
}

function windowMeanSquare(windowType: number): number {
  if (windowType === 1)
    return 0.3046;

  if (windowType === 2)
    return 0.6875;

  return 0.375;
}

function renderGraph(state: DspState) {
  const resourceId = readSampleString(state, 'resourceId');
  const sampleReady = readSampleString(state, 'status') === 'ready';

  if (!sampleReady || resourceId.length === 0) {
    const silence = el.mul(0, el.sr());
    return core.render(silence, silence);
  }

  const windowType = Math.round(readNumber(state, 'windowType', 0));
  const density = effectiveDensity(state);
  const grainSizeSeconds = Math.max(0.005, readNumber(state, 'grainSize', 100) / 1000);
  const expectedOverlap = Math.max(1, density * grainSizeSeconds);
  const overlapGain = 1 / Math.sqrt(expectedOverlap * windowMeanSquare(windowType));
  const outputGain = Math.pow(10, readNumber(state, 'outputGain', -6) / 20);
  const leftGrains: ElemNode[] = [];
  const rightGrains: ElemNode[] = [];

  for (let grain = 0; grain < maxGrains; grain += 1) {
    const position = el.in({key: `grain:${grain}:position`, channel: grainPositionOffset + grain});
    const phase = el.in({key: `grain:${grain}:phase`, channel: grainPhaseOffset + grain});
    const leftGain = el.in({key: `grain:${grain}:leftGain`, channel: grainLeftGainOffset + grain});
    const rightGain = el.in({key: `grain:${grain}:rightGain`, channel: grainRightGainOffset + grain});
    const sample = el.table({key: `grain:${grain}:sample`, path: resourceId}, position);
    const window = grainWindow(windowType, phase);

    leftGrains.push(el.mul(sample, window, leftGain));
    rightGrains.push(el.mul(sample, window, rightGain));
  }

  const normalization = overlapGain * outputGain / Math.sqrt(maxVoices);
  const left = el.mul(normalization, el.add(...leftGrains));
  const right = el.mul(normalization, el.add(...rightGrains));
  return core.render(left, right);
}

function getRendererNodeMap(renderer: Renderer): Map<number, HydratedNode> {
  const delegate = Object.getOwnPropertyDescriptor(renderer, '_delegate')?.value;

  if (typeof delegate !== 'object' || delegate === null) {
    throw new Error('Cannot hydrate DSP graph: renderer delegate is unavailable');
  }

  const nodeMap = Object.getOwnPropertyDescriptor(delegate, 'nodeMap')?.value;

  if (!(nodeMap instanceof Map)) {
    throw new Error('Cannot hydrate DSP graph: renderer node map is unavailable');
  }

  return nodeMap;
}

// The important piece: here we register a state change callback with the native
// side. This callback will be hit with the current processor state any time that
// state changes.
//
// Given the new state, we simply update our refs or perform a full render depending
// on the result of our `shouldRender` check.
globalThis.__receiveStateChange__ = (serializedState: string) => {
  const state = parseDspState(serializedState);

  if (shouldRender(prevState, state)) {
    console.log(renderGraph(state));
  }

  prevState = state;
};

// NOTE: This is highly experimental and should not yet be relied on
// as a consistent feature.
//
// This hook allows the native side to inject serialized graph state from
// the running elem::Runtime instance so that we can throw away and reinitialize
// the JavaScript engine and then inject necessary state for coordinating with
// the underlying engine.
globalThis.__receiveHydrationData__ = (data: string) => {
  const payload = parseJsonObject(data, 'hydration data');
  const nodeMap = getRendererNodeMap(core);

  for (const [k, v] of Object.entries(payload)) {
    nodeMap.set(parseInt(k, 16), {
      symbol: '__ELEM_NODE__',
      kind: '__HYDRATED__',
      hash: parseInt(k, 16),
      props: v,
      generation: {
        current: 0,
      },
    });
  }
};

// Finally, an error callback which just logs back to native
globalThis.__receiveError__ = (err: PluginError) => {
  console.log(`[Error: ${err.name}] ${err.message}`);
};
