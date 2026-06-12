/// <reference types="vite/client" />

declare global {
  type PluginState = Record<string, number>;

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
