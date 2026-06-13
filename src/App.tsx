import { useStore as useZustandStore } from 'zustand'
import { createStore } from 'zustand/vanilla'

import Interface from './Interface'

const store = createStore<PluginState>(() => ({}));
const useStore = () => useZustandStore(store);

const errorStore = createStore<{ error: PluginError | null }>(() => ({ error: null }));
const useErrorStore = () => useZustandStore(errorStore);

function requestParamValueUpdate(paramId: string, value: number) {
  if (typeof globalThis.__postNativeMessage__ === 'function') {
    globalThis.__postNativeMessage__("setParameterValue", {
      paramId,
      value,
    });
  }
}

function openSample() {
  globalThis.__postNativeMessage__?.('openSample');
}

function postPresetMessage(
  message: 'savePreset' | 'savePresetAs' | 'loadPreset' | 'renamePreset' | 'deletePreset',
  payload: Record<string, unknown>,
) {
  globalThis.__postNativeMessage__?.(message, payload);
}

if (import.meta.env.DEV && import.meta.hot) {
  import.meta.hot.on('reload-dsp', () => {
    console.log('Sending reload dsp message');

    if (typeof globalThis.__postNativeMessage__ === 'function') {
      globalThis.__postNativeMessage__('reload');
    }
  });
}

globalThis.__receiveStateChange__ = function(state: string) {
  store.setState(JSON.parse(state) as PluginState);
};

globalThis.__receiveError__ = (err: PluginError) => {
  errorStore.setState({ error: err });
};

export default function App() {
  const state = useStore();
  const {error} = useErrorStore();

  return (
    <Interface
      state={state}
      error={error}
      openSample={openSample}
      savePreset={(name) => postPresetMessage('savePreset', { name })}
      savePresetAs={(name) => postPresetMessage('savePresetAs', { name })}
      loadPreset={(presetId) => postPresetMessage('loadPreset', { presetId })}
      renamePreset={(presetId, name) => postPresetMessage('renamePreset', { presetId, name })}
      deletePreset={(presetId) => postPresetMessage('deletePreset', { presetId })}
      requestParamValueUpdate={requestParamValueUpdate}
      resetErrorState={() => errorStore.setState({ error: null })} />
  );
}
