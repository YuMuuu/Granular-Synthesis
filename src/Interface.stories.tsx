import type { Meta, StoryObj } from '@storybook/react-vite';

import Interface from './Interface';

const defaultState = {
  grainSize: 100,
  density: 20,
  presets: {
    activePresetId: 'preset-1',
    items: [
      { id: 'preset-1', name: 'Wide Texture', modifiedAt: '2026-06-13T00:00:00Z' },
      { id: 'preset-2', name: 'Frozen Cloud', modifiedAt: '2026-06-13T00:00:00Z' },
    ],
  },
  sample: {
    status: 'ready' as const,
    sampleId: 'demo',
    resourceId: 'sample:demo',
    fileName: 'demo.wav',
    originalFileName: 'demo.wav',
    fileHash: 'demo',
    sampleRate: 48000,
    numFrames: 144000,
    durationSeconds: 3,
    waveformPeaks: [],
    error: '',
  },
};

const meta = {
  title: 'Plugin/Interface',
  component: Interface,
  parameters: {
    layout: 'fullscreen',
  },
  args: {
    state: defaultState,
    error: null,
    openSample: () => {},
    savePreset: () => {},
    savePresetAs: () => {},
    loadPreset: () => {},
    renamePreset: () => {},
    deletePreset: () => {},
    requestParamValueUpdate: () => {},
    resetErrorState: () => {},
  },
} satisfies Meta<typeof Interface>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {};

export const DrySmallRoom: Story = {
  args: {
    state: {
      ...defaultState,
      grainSize: 40,
      density: 8,
    },
  },
};

export const WideLush: Story = {
  args: {
    state: {
      ...defaultState,
      grainSize: 220,
      density: 45,
    },
  },
};

export const WithError: Story = {
  args: {
    error: {
      message: 'DSP graph failed to render',
    },
  },
};
