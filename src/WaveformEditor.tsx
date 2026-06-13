import { useMemo, useState, type PointerEvent } from 'react';

type Marker = 'start' | 'position' | 'end';

type WaveformEditorProps = {
  peaks: number[];
  regionStart: number;
  regionEnd: number;
  position: number;
  onChange: (paramId: 'regionStart' | 'regionEnd' | 'position', value: number) => void;
};

const viewWidth = 1000;
const viewHeight = 160;
const centerY = viewHeight / 2;

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}

function WaveformEditor(props: WaveformEditorProps) {
  const [activeMarker, setActiveMarker] = useState<Marker | null>(null);
  const waveformPath = useMemo(() => {
    if (props.peaks.length === 0)
      return '';

    const step = viewWidth / props.peaks.length;
    return props.peaks.map((peak, index) => {
      const x = (index + 0.5) * step;
      const amplitude = clamp(peak, 0, 1) * (centerY - 12);
      return `M ${x.toFixed(2)} ${(centerY - amplitude).toFixed(2)} V ${(centerY + amplitude).toFixed(2)}`;
    }).join(' ');
  }, [props.peaks]);

  function updateMarker(marker: Marker, clientX: number, svg: SVGSVGElement) {
    const bounds = svg.getBoundingClientRect();
    const value = clamp((clientX - bounds.left) / bounds.width, 0, 1);

    if (marker === 'start') {
      const nextStart = Math.min(value, props.regionEnd);
      props.onChange('regionStart', nextStart);

      if (props.position < nextStart)
        props.onChange('position', nextStart);
    } else if (marker === 'end') {
      const nextEnd = Math.max(value, props.regionStart);
      props.onChange('regionEnd', nextEnd);

      if (props.position > nextEnd)
        props.onChange('position', nextEnd);
    }
    else
      props.onChange('position', clamp(value, props.regionStart, props.regionEnd));
  }

  function startDrag(marker: Marker, event: PointerEvent<SVGGElement>) {
    const svg = event.currentTarget.ownerSVGElement;

    if (!svg)
      return;

    svg.setPointerCapture(event.pointerId);
    setActiveMarker(marker);
    updateMarker(marker, event.clientX, svg);
  }

  function continueDrag(event: PointerEvent<SVGSVGElement>) {
    if (activeMarker)
      updateMarker(activeMarker, event.clientX, event.currentTarget);
  }

  function stopDrag(event: PointerEvent<SVGSVGElement>) {
    if (event.currentTarget.hasPointerCapture(event.pointerId))
      event.currentTarget.releasePointerCapture(event.pointerId);

    setActiveMarker(null);
  }

  const startX = props.regionStart * viewWidth;
  const endX = props.regionEnd * viewWidth;
  const positionX = props.position * viewWidth;

  return (
    <div className="rounded border border-slate-700 bg-slate-950 p-2">
      <div className="mb-1 flex justify-between text-[10px] leading-none text-slate-400">
        <span>Waveform</span>
        <span>Drag Start, Position, or End</span>
      </div>
      <svg
        aria-label="Sample waveform editor"
        className="h-20 w-full touch-none select-none rounded bg-slate-900"
        role="img"
        viewBox={`0 0 ${viewWidth} ${viewHeight}`}
        onPointerMove={continueDrag}
        onPointerUp={stopDrag}
        onPointerCancel={stopDrag}
      >
        <rect width={viewWidth} height={viewHeight} fill="#0f172a" />
        <rect
          x={startX}
          width={Math.max(0, endX - startX)}
          height={viewHeight}
          fill="#ec4899"
          opacity="0.09"
        />
        <line x1="0" x2={viewWidth} y1={centerY} y2={centerY} stroke="#334155" />
        {waveformPath
          ? <path d={waveformPath} fill="none" stroke="#94a3b8" strokeWidth="2" opacity="0.9" />
          : <text x={viewWidth / 2} y={centerY + 5} fill="#64748b" textAnchor="middle">No waveform loaded</text>}

        <g className="cursor-ew-resize" onPointerDown={(event) => startDrag('start', event)}>
          <line x1={startX} x2={startX} y1="0" y2={viewHeight} stroke="#f8fafc" strokeWidth="3" />
          <rect data-testid="waveform-start-handle" x={startX - 14} width="28" height={viewHeight} fill="transparent" />
          <text x={clamp(startX + 8, 8, viewWidth - 44)} y="17" fill="#f8fafc" fontSize="18">Start</text>
        </g>

        <g className="cursor-ew-resize" onPointerDown={(event) => startDrag('position', event)}>
          <line x1={positionX} x2={positionX} y1="0" y2={viewHeight} stroke="#f472b6" strokeWidth="4" />
          <circle cx={positionX} cy={centerY} r="8" fill="#f472b6" />
          <rect data-testid="waveform-position-handle" x={positionX - 14} width="28" height={viewHeight} fill="transparent" />
        </g>

        <g className="cursor-ew-resize" onPointerDown={(event) => startDrag('end', event)}>
          <line x1={endX} x2={endX} y1="0" y2={viewHeight} stroke="#f8fafc" strokeWidth="3" />
          <rect data-testid="waveform-end-handle" x={endX - 14} width="28" height={viewHeight} fill="transparent" />
          <text x={clamp(endX - 42, 8, viewWidth - 38)} y="17" fill="#f8fafc" fontSize="18">End</text>
        </g>
      </svg>
      <div className="mt-1 grid grid-cols-3 text-[10px] leading-none text-slate-300">
        <span>Start {props.regionStart.toFixed(3)}</span>
        <span className="text-center text-pink-400">Position {props.position.toFixed(3)}</span>
        <span className="text-right">End {props.regionEnd.toFixed(3)}</span>
      </div>
    </div>
  );
}

export default WaveformEditor;
