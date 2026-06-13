import type { Meta, StoryObj } from '@storybook/react-vite';
import { useState, type ComponentProps } from 'react';

import Interface from './Interface';

const waveformPeaks = Array.from({ length: 512 }, (_, index) => {
  const envelope = Math.sin(Math.PI * index / 511);
  const carrier = 0.35 + 0.65 * Math.abs(Math.sin(index * 0.17));
  return envelope * carrier;
});

const defaultState = {
  grainSize: 100,
  density: 20,
  regionStart: 0.12,
  regionEnd: 0.86,
  position: 0.46,
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
    waveformPeaks,
    error: '',
  },
};

function InteractiveInterface(args: ComponentProps<typeof Interface>) {
  const [state, setState] = useState(args.state);

  return (
    <Interface
      {...args}
      state={state}
      requestParamValueUpdate={(paramId, value) => {
        args.requestParamValueUpdate(paramId, value);
        setState((current) => ({ ...current, [paramId]: value }));
      }}
    />
  );
}

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

export const Default: Story = {
  render: (args) => <InteractiveInterface {...args} />,
};

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
