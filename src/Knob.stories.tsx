import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import Knob, { type KnobProps } from './Knob';

function ControlledKnob(args: KnobProps) {
  const [value, setValue] = useState(args.value);

  return (
    <div className="flex h-48 w-48 items-center justify-center bg-slate-800">
      <Knob {...args} value={value} onChange={setValue} />
    </div>
  );
}

const meta = {
  title: 'Controls/Knob',
  component: Knob,
  parameters: {
    layout: 'centered',
  },
  args: {
    className: 'h-24 w-24',
    value: 0.35,
    meterColor: '#EC4899',
    knobColor: '#64748B',
    thumbColor: '#F8FAFC',
    onChange: () => {},
  },
  argTypes: {
    onChange: {
      table: {
        disable: true,
      },
    },
    value: {
      control: {
        type: 'range',
        min: 0,
        max: 1,
        step: 0.01,
      },
    },
    meterColor: {
      control: 'color',
    },
    knobColor: {
      control: 'color',
    },
    thumbColor: {
      control: 'color',
    },
  },
  render: (args) => <ControlledKnob {...args} />,
} satisfies Meta<typeof Knob>;

export default meta;
type Story = StoryObj<typeof meta>;

export const Default: Story = {
  args: {
    value: 0.17
  }
};

export const Minimum: Story = {
  args: {
    value: 0,
  },
};

export const Maximum: Story = {
  args: {
    value: 1,
  },
};
