import type { HTMLAttributes, ReactNode } from 'react';
import { useRef } from 'react';
import { useDrag } from '@use-gesture/react'


function clampUnitInterval(value: number) {
  return Math.max(0, Math.min(1, value));
}

function hasValue(value: number | undefined): value is number {
  return typeof value === 'number';
}

type DragBehaviorProps = Omit<HTMLAttributes<HTMLDivElement>, 'onChange'> & {
  children?: ReactNode;
  snapToMouseLinearHorizontal?: boolean;
  value?: number;
  onChange?: (value: number) => void;
};

export default function DragBehavior(props: DragBehaviorProps) {
  const nodeRef = useRef<HTMLDivElement>(null);
  const valueAtDragStartRef = useRef(props.value || 0);

  const {snapToMouseLinearHorizontal, value, onChange, ...other} = props;
  const emitChange = (nextValue: number) => onChange?.(clampUnitInterval(nextValue));

  const bindDragHandlers = useDrag((state) => {
    if (hasValue(value)) {
      if (state.first) {
        valueAtDragStartRef.current = value;

        if (snapToMouseLinearHorizontal) {
          const [x] = state.xy;
          if (!nodeRef.current) {
            return;
          }

          const posInScreen = nodeRef.current.getBoundingClientRect();

          const dx = x - posInScreen.left;
          const dv = dx / posInScreen.width;

          valueAtDragStartRef.current = clampUnitInterval(dv);
          emitChange(dv);
        }

        return;
      }
    }

    const [dx, dy] = state.movement;
    const dv = (dx - dy) / 200;

    emitChange(valueAtDragStartRef.current + dv);
  });

  return (
    <div ref={nodeRef} className="touch-none" {...bindDragHandlers()} {...other}>
      {props.children}
    </div>
  );
}
