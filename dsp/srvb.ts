import {el, type ElemNode} from '@elemaudio/core';

type EightChannels = [
  ElemNode,
  ElemNode,
  ElemNode,
  ElemNode,
  ElemNode,
  ElemNode,
  ElemNode,
  ElemNode,
];

type HadamardRow = [number, number, number, number, number, number, number, number];
type Hadamard8 = [
  HadamardRow,
  HadamardRow,
  HadamardRow,
  HadamardRow,
  HadamardRow,
  HadamardRow,
  HadamardRow,
  HadamardRow,
];

// A size 8 Hadamard matrix constructed using Numpy and Scipy.
//
// The Hadamard matrix satisfies the property H*H^T = nI, where n is the size
// of the matrix, I the identity, and H^T the transpose of H. Therefore, we have
// orthogonality and stability in the feedback path if we scale according to (1 / n)
// along the diagonal, which we do internally by multiplying each matrix element
// by Math.sqrt(1 / n), which yields the identity as above.
//
// @see https://docs.scipy.org/doc/scipy-0.14.0/reference/generated/scipy.linalg.hadamard.html
// @see https://nhigham.com/2020/04/10/what-is-a-hadamard-matrix/
const H8: Hadamard8 = [
  [ 1,  1,  1,  1,  1,  1,  1,  1],
  [ 1, -1,  1, -1,  1, -1,  1, -1],
  [ 1,  1, -1, -1,  1,  1, -1, -1],
  [ 1, -1, -1,  1,  1, -1, -1,  1],
  [ 1,  1,  1,  1, -1, -1, -1, -1],
  [ 1, -1,  1, -1, -1,  1, -1,  1],
  [ 1,  1, -1, -1, -1, -1,  1,  1],
  [ 1, -1, -1,  1, -1,  1,  1, -1],
];

function mixHadamard(inputs: EightChannels): EightChannels {
  const scale = Math.sqrt(1 / inputs.length);
  const mixRow = (row: HadamardRow) => el.add(
    el.mul(row[0] * scale, inputs[0]),
    el.mul(row[1] * scale, inputs[1]),
    el.mul(row[2] * scale, inputs[2]),
    el.mul(row[3] * scale, inputs[3]),
    el.mul(row[4] * scale, inputs[4]),
    el.mul(row[5] * scale, inputs[5]),
    el.mul(row[6] * scale, inputs[6]),
    el.mul(row[7] * scale, inputs[7]),
  );

  return [
    mixRow(H8[0]),
    mixRow(H8[1]),
    mixRow(H8[2]),
    mixRow(H8[3]),
    mixRow(H8[4]),
    mixRow(H8[5]),
    mixRow(H8[6]),
    mixRow(H8[7]),
  ];
}

// A diffusion step expecting exactly 8 input channels with
// a maximum diffusion time of 500ms
function diffuse(size: number, ...ins: EightChannels): EightChannels {
  const len = ins.length;
  const dels: EightChannels = [
    el.sdelay({size: size * (1 / len)}, ins[0]),
    el.sdelay({size: size * (2 / len)}, ins[1]),
    el.sdelay({size: size * (3 / len)}, ins[2]),
    el.sdelay({size: size * (4 / len)}, ins[3]),
    el.sdelay({size: size * (5 / len)}, ins[4]),
    el.sdelay({size: size * (6 / len)}, ins[5]),
    el.sdelay({size: size * (7 / len)}, ins[6]),
    el.sdelay({size: size * (8 / len)}, ins[7]),
  ];

  return mixHadamard(dels);
}

// An eight channel feedback delay network with a one-pole lowpass filter in
// the feedback loop for damping the high frequencies faster than the low.
//
// @param {string} name for the tap structures
// @param {el.const} size in the range [0, 1]
// @param {el.const} decay in the range [0, 1]
// @param {el.const} modDepth in the range [0, 1]
// @param {...core.Node} ...ins eight input channels
function dampFDN(
  name: string,
  sampleRate: number,
  size: ElemNode,
  decay: ElemNode,
  modDepth: ElemNode,
  ...ins: EightChannels
): EightChannels {
  const md = el.mul(modDepth, 0.02);

  // The unity-gain one pole lowpass here is tuned to taste along
  // the range [0.001, 0.5]. Towards the top of the range, we get into the region
  // of killing the decay time too quickly. Towards the bottom, not much damping.
  const makeFeedbackInput = (input: ElemNode, i: number) => {
    return el.add(
      input,
      el.mul(
        decay,
        el.smooth(
          0.105,
          el.tapIn({name: `${name}:fdn${i}`}),
        ),
      ),
    );
  };

  const dels: EightChannels = [
    makeFeedbackInput(ins[0], 0),
    makeFeedbackInput(ins[1], 1),
    makeFeedbackInput(ins[2], 2),
    makeFeedbackInput(ins[3], 3),
    makeFeedbackInput(ins[4], 4),
    makeFeedbackInput(ins[5], 5),
    makeFeedbackInput(ins[6], 6),
    makeFeedbackInput(ins[7], 7),
  ];

  const mix = mixHadamard(dels);

  const makeDelayLine = (mm: ElemNode, i: number) => {
    const modulate = (x: ElemNode, rate: ElemNode, amt: ElemNode) => el.add(x, el.mul(amt, el.cycle(rate)));
    const ms2samps = (ms: number) => sampleRate * (ms / 1000.0);

    // Each delay line here will be ((i + 1) * 17)ms long, multiplied by [1, 4]
    // depending on the size parameter. So at size = 0, delay lines are 17, 34, 51, ...,
    // and at size = 1 we have 68, 136, ..., all in ms here.
    const delaySize = el.mul(el.add(1.00, el.mul(3, size)), ms2samps((i + 1) * 17));

    // Then we modulate the read position for each tap to add some chorus in the
    // delay network.
    const readPos = modulate(delaySize, el.add(0.1, el.mul(i, md)), ms2samps(2.5));

    return el.tapOut(
      {name: `${name}:fdn${i}`},
      el.delay(
        {size: ms2samps(750)},
        readPos,
        0,
        mm
      ),
    );
  };

  return [
    makeDelayLine(mix[0], 0),
    makeDelayLine(mix[1], 1),
    makeDelayLine(mix[2], 2),
    makeDelayLine(mix[3], 3),
    makeDelayLine(mix[4], 4),
    makeDelayLine(mix[5], 5),
    makeDelayLine(mix[6], 6),
    makeDelayLine(mix[7], 7),
  ];
}

// Our main stereo reverb.
//
// Upmixes the stereo input into an 8-channel diffusion network and
// feedback delay network. Must supply a `key` prop to uniquely identify the
// feedback taps in here.
//
// @param {object} props
// @param {number} props.size in [0, 1]
// @param {number} props.decay in [0, 1]
// @param {number} props.mod in [0, 1]
// @param {number} props.mix in [0, 1]
// @param {core.Node} xl input
// @param {core.Node} xr input
type SrvbProps = {
  key: string;
  sampleRate: number;
  size: ElemNode;
  decay: ElemNode;
  mod: ElemNode;
  mix: ElemNode;
};

export default function srvb(props: SrvbProps, xl: ElemNode, xr: ElemNode): ElemNode[] {
  const key = props.key;
  const sampleRate = props.sampleRate;
  const size = el.sm(props.size);
  const decay = el.sm(props.decay);
  const modDepth = el.sm(props.mod);
  const mix = el.sm(props.mix);

  // Upmix to eight channels
  const mid = el.mul(0.5, el.add(xl, xr));
  const side = el.mul(0.5, el.sub(xl, xr));
  const eight: EightChannels = [
    xl,
    xr,
    mid,
    side,
    el.mul(-1, xl),
    el.mul(-1, xr),
    el.mul(-1, mid),
    el.mul(-1, side),
  ];

  // Diffusion
  const ms2samps = (ms: number) => sampleRate * (ms / 1000.0);

  const d1 = diffuse(ms2samps(43), ...eight);
  const d2 = diffuse(ms2samps(97), ...d1);
  const d3 = diffuse(ms2samps(117), ...d2);

  // Reverb network
  const d4 = dampFDN(`${key}:d4`, sampleRate, size, 0.004, modDepth, ...d3)
  const r0 = dampFDN(`${key}:r0`, sampleRate, size, decay, modDepth, ...d4);

  // Downmix
  //
  // It's important here to interleave the output channels because the way that
  // the multi-channel delay lines are written above tends to correlate the delay
  // length with the current index in the 8-channel array. That means the smaller
  // the index, the shorter the delay line. The mix matrix will mostly address this,
  // but if you sum index 0-3 into the left and 4-7 into the right you can definitely
  // hear the energy in the left channel build before the energy in the right.
  const yl = el.mul(0.25, el.add(r0[0], r0[2], r0[4], r0[6]));
  const yr = el.mul(0.25, el.add(r0[1], r0[3], r0[5], r0[7]));

  // Wet dry mixing
  return [
    el.select(mix, yl, xl),
    el.select(mix, yr, xr),
  ];
}
