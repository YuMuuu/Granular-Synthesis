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
const triggerOffset = 0;
const gateOffset = maxVoices;
const pitchOffset = maxVoices * 2;
const velocityOffset = maxVoices * 3;

function readNumber(state: DspState, key: string, fallback: number): number {
  const value = state[key];
  return typeof value === 'number' ? value : fallback;
}

function readSampleNumber(state: DspState, key: 'sampleRate' | 'numFrames'): number {
  const value = state.sample?.[key];
  return typeof value === 'number' ? value : 0;
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
    sourceSampleRate: readSampleNumber(state, 'sampleRate'),
    numFrames: readSampleNumber(state, 'numFrames'),
    regionStart: readNumber(state, 'regionStart', 0),
    regionEnd: readNumber(state, 'regionEnd', 1),
    attack: readNumber(state, 'attack', 0.01),
    decay: readNumber(state, 'decay', 0.1),
    sustain: readNumber(state, 'sustain', 0.8),
    release: readNumber(state, 'release', 0.5),
    stereoWidth: readNumber(state, 'stereoWidth', 0.5),
    outputGain: readNumber(state, 'outputGain', -6),
  });
}

function shouldRender(prevState: DspState | null, nextState: DspState) {
  return prevState === null || graphSignature(prevState) !== graphSignature(nextState);
}

function param(state: DspState, key: string, fallback: number) {
  return el.const({key: `param:${key}`, value: readNumber(state, key, fallback)});
}

function renderGraph(state: DspState) {
  const resourceId = readSampleString(state, 'resourceId');
  const sampleReady = readSampleString(state, 'status') === 'ready';
  const sourceSampleRate = readSampleNumber(state, 'sampleRate');
  const numFrames = readSampleNumber(state, 'numFrames');

  if (!sampleReady || resourceId.length === 0 || sourceSampleRate <= 0 || numFrames <= 1) {
    const silence = el.mul(0, el.sr());
    return core.render(silence, silence);
  }

  const regionStart = Math.max(0, Math.min(1, readNumber(state, 'regionStart', 0)));
  const regionEnd = Math.max(regionStart, Math.min(1, readNumber(state, 'regionEnd', 1)));
  const startOffset = Math.floor(regionStart * (numFrames - 1));
  const stopOffset = Math.floor((1 - regionEnd) * (numFrames - 1));
  const sampleRateCorrection = sourceSampleRate / state.sampleRate;
  const attack = param(state, 'attack', 0.01);
  const decay = param(state, 'decay', 0.1);
  const sustain = param(state, 'sustain', 0.8);
  const release = param(state, 'release', 0.5);
  const outputGain = el.db2gain(param(state, 'outputGain', -6));
  const stereoWidth = Math.max(0, Math.min(1, readNumber(state, 'stereoWidth', 0.5)));
  const leftVoices: ElemNode[] = [];
  const rightVoices: ElemNode[] = [];

  for (let voice = 0; voice < maxVoices; voice += 1) {
    const trigger = el.in({key: `voice:${voice}:trigger`, channel: triggerOffset + voice});
    const gate = el.in({key: `voice:${voice}:gate`, channel: gateOffset + voice});
    const pitch = el.in({key: `voice:${voice}:pitch`, channel: pitchOffset + voice});
    const velocity = el.in({key: `voice:${voice}:velocity`, channel: velocityOffset + voice});
    const reader = el.sample(
      {
        key: `voice:${voice}:sample`,
        path: resourceId,
        mode: 'trigger',
        startOffset,
        stopOffset,
      },
      trigger,
      el.mul(sampleRateCorrection, pitch),
    );
    const retriggeredGate = el.mul(gate, el.sub(1, trigger));
    const envelope = el.adsr(attack, decay, sustain, release, retriggeredGate);
    const signal = el.mul(reader, envelope, velocity);
    const pan = ((voice / (maxVoices - 1)) * 2 - 1) * stereoWidth;

    leftVoices.push(el.mul(signal, (1 - pan) * 0.5));
    rightVoices.push(el.mul(signal, (1 + pan) * 0.5));
  }

  const normalization = 1 / Math.sqrt(maxVoices);
  const left = el.mul(normalization, outputGain, el.add(...leftVoices));
  const right = el.mul(normalization, outputGain, el.add(...rightVoices));
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
