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
      requestParamValueUpdate={requestParamValueUpdate}
      resetErrorState={() => errorStore.setState({ error: null })} />
  );
}
