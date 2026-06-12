import invariant from 'invariant';
import type { ElemNode, Renderer } from '@elemaudio/core';
import type { JsonObject } from './types';

type RefSetter = (props: JsonObject) => Promise<void>;
type RefEntry = [ElemNode, RefSetter];

export class RefMap {
  private _map: Map<string, RefEntry>;
  private _core: Renderer;

  constructor(core: Renderer) {
    this._map = new Map();
    this._core = core;
  }

  getOrCreate(
    name: string,
    type: string,
    props: JsonObject,
    children: ElemNode[],
  ): ElemNode {
    if (!this._map.has(name)) {
      this._map.set(name, this.createRefEntry(type, props, children));
    }

    return this._map.get(name)![0];
  }

  update(name: string, props: JsonObject) {
    invariant(this._map.has(name), "Oops, trying to update a ref that doesn't exist");

    const [, setter] = this._map.get(name)!;
    setter(props);
  }

  private createRefEntry(type: string, props: JsonObject, children: ElemNode[]): RefEntry {
    const [node, setRefProps] = this._core.createRef(type, props, children);

    if (typeof node === 'function' || typeof setRefProps !== 'function') {
      throw new Error('Unexpected ref shape returned from Elementary Renderer');
    }

    return [
      node,
      async (nextProps) => {
        await setRefProps(nextProps);
      },
    ];
  }
}
