/// <reference types="vite/client" />

declare global {
  type SampleLoadStatus = 'empty' | 'loading' | 'ready' | 'error' | 'missing';

  type SampleState = {
    status: SampleLoadStatus;
    sampleId: string;
    resourceId: string;
    fileName: string;
    originalFileName: string;
    fileHash: string;
    sampleRate: number;
    numFrames: number;
    durationSeconds: number;
    waveformPeaks: number[];
    error: string;
  };

  type MeterState = {
    activeVoices: number;
    activeGrains: number;
    cpuOverload: boolean;
  };

  type TransportState = {
    bpm: number;
    tempoAvailable: boolean;
    isPlaying: boolean;
  };

  type PresetSummary = {
    id: string;
    name: string;
    modifiedAt: string;
  };

  type PresetState = {
    items: PresetSummary[];
    activePresetId: string;
  };

  type PluginState = Record<string, unknown> & {
    schemaVersion?: number;
    sample?: SampleState;
    meters?: MeterState;
    transport?: TransportState;
    presets?: PresetState;
  };

  type PluginError = {
    name?: string;
    message: string;
  };

  const __COMMIT_HASH__: string;
  const __BUILD_DATE__: string;

  var __postNativeMessage__:
    | ((message: string, payload?: Record<string, unknown>) => void)
    | undefined;
  var __receiveStateChange__: (state: string) => void;
  var __receiveError__: (error: PluginError) => void;
  var __receiveHydrationData__: (data: string) => void;
}

export {};
