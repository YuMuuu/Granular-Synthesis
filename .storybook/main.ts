import type { StorybookConfig } from '@storybook/react-vite';
import { mergeConfig } from 'vite';

const config: StorybookConfig = {
  stories: ['../src/**/*.stories.@(ts|tsx)'],
  addons: [
    '@storybook/addon-docs',
    '@storybook/addon-a11y',
  ],
  framework: {
    name: '@storybook/react-vite',
    options: {},
  },
  viteFinal: (config) => mergeConfig(config, {
    define: {
      __COMMIT_HASH__: JSON.stringify('storybook'),
      __BUILD_DATE__: JSON.stringify('storybook'),
    },
  }),
};

export default config;
