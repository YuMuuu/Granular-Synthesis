import type { Meta, StoryObj } from '@storybook/react-vite';

import Interface from './Interface';

const defaultState = {
  size: 0.35,
  decay: 0.62,
  mod: 0.28,
  mix: 0.45,
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
      size: 0.18,
      decay: 0.32,
      mod: 0.12,
      mix: 0.2,
    },
  },
};

export const WideLush: Story = {
  args: {
    state: {
      "size": 0.11,
      "decay": 0.86,
      "mod": 0.68,
      "mix": 0.72
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
