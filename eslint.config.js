import js from '@eslint/js';
import { FlatCompat } from '@eslint/eslintrc';
import globals from 'globals';
import importPlugin from 'eslint-plugin-import';
import react from 'eslint-plugin-react';
import reactHooks from 'eslint-plugin-react-hooks';
import reactRefresh from 'eslint-plugin-react-refresh';
import tseslint from 'typescript-eslint';

const compat = new FlatCompat({
  recommendedConfig: js.configs.recommended,
});

const reactJsxRuntime = react.configs.flat['jsx-runtime'];
const reactHooksRecommended = reactHooks.configs.flat.recommended;
const reactRefreshVite = reactRefresh.configs.vite;
const tsFiles = ['src/**/*.{ts,tsx}', 'dsp/**/*.ts', '.storybook/**/*.ts', 'vite.config.ts'];

export default tseslint.config(
  {
    ignores: [
      'dist',
      'native',
      'node_modules',
      'public/dsp.main.js',
    ],
  },
  ...compat.extends(
    'eslint:recommended',
    'plugin:react/recommended',
    'plugin:@typescript-eslint/recommended',
  ),
  {
    settings: {
      react: {
        version: 'detect',
      },
    },
  },
  importPlugin.flatConfigs.recommended,
  {
    files: tsFiles,
    ...importPlugin.flatConfigs.typescript,
    languageOptions: {
      ...importPlugin.flatConfigs.typescript.languageOptions,
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.es2022,
      },
    },
    settings: {
      ...importPlugin.flatConfigs.typescript.settings,
      'import/resolver': {
        typescript: {
          project: './tsconfig.json',
        },
        node: true,
      },
    },
    rules: {
      ...importPlugin.flatConfigs.typescript.rules,
      'no-undef': 'off',
    },
  },
  {
    files: ['src/**/*.{ts,tsx}'],
    plugins: {
      ...reactJsxRuntime.plugins,
      ...reactHooksRecommended.plugins,
      ...reactRefreshVite.plugins,
    },
    languageOptions: {
      ...reactJsxRuntime.languageOptions,
      ...reactHooksRecommended.languageOptions,
      ...reactRefreshVite.languageOptions,
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      ...reactJsxRuntime.rules,
      ...reactHooksRecommended.rules,
      ...reactRefreshVite.rules,
      'react/prop-types': 'off',
    },
  },
  {
    files: ['src/main.tsx'],
    rules: {
      'react-refresh/only-export-components': 'off',
    },
  },
  {
    files: ['dsp/**/*.ts'],
    languageOptions: {
      globals: {
        ...globals.browser,
      },
    },
  },
  {
    files: ['.storybook/main.ts', 'vite.config.ts'],
    languageOptions: {
      globals: {
        ...globals.node,
      },
    },
  },
  {
    files: ['scripts/**/*.mjs'],
    languageOptions: {
      globals: {
        ...globals.node,
        ...globals.browser,
      },
    },
  },
);
