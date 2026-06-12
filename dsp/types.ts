export type JsonPrimitive = string | number | boolean | null;
export type JsonValue = JsonPrimitive | JsonValue[] | JsonObject;
export type JsonObject = {
  [key: string]: JsonValue;
};

export type DspState = {
  sampleRate: number;
  size: number;
  decay: number;
  mod: number;
  mix: number;
};

export type HydratedNode = {
  symbol: '__ELEM_NODE__';
  kind: '__HYDRATED__';
  hash: number;
  props: JsonValue;
  generation: {
    current: number;
  };
};

export function isJsonObject(value: JsonValue): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function parseJsonObject(serialized: string, label: string): JsonObject {
  const value: JsonValue = JSON.parse(serialized);

  if (!isJsonObject(value)) {
    throw new Error(`Invalid ${label}: expected a JSON object`);
  }

  return value;
}

function readNumberField(source: JsonObject, key: string): number {
  const value = source[key];

  if (typeof value !== 'number') {
    throw new Error(`Invalid DSP state: expected numeric field "${key}"`);
  }

  return value;
}

export function parseDspState(serialized: string): DspState {
  const source = parseJsonObject(serialized, 'DSP state');

  return {
    sampleRate: readNumberField(source, 'sampleRate'),
    size: readNumberField(source, 'size'),
    decay: readNumberField(source, 'decay'),
    mod: readNumberField(source, 'mod'),
    mix: readNumberField(source, 'mix'),
  };
}
