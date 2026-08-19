import { encodePointerMove } from "@workspace/remcote-protocol";
import type { RemoteSession } from "./session";

/** Map KeyboardEvent.code to Windows PS/2 Set 1 scan codes (subset; host falls back to VK mapping by code string when 0). */
const SCAN_CODES: Record<string, number> = {
  Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05,
  Digit5: 0x06, Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0a,
  Digit0: 0x0b, Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15,
  KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19, BracketLeft: 0x1a,
  BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d, KeyA: 0x1e, KeyS: 0x1f,
  KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25,
  KeyL: 0x26, Semicolon: 0x27, Quote: 0x28, Backquote: 0x29, ShiftLeft: 0x2a,
  Backslash: 0x2b, KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e, KeyV: 0x2f, KeyB: 0x30,
  KeyN: 0x31, KeyM: 0x32, Comma: 0x33, Period: 0x34, Slash: 0x35,
  ShiftRight: 0x36, NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39,
  CapsLock: 0x3a, F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e, F5: 0x3f, F6: 0x40,
  F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44, NumLock: 0x45, ScrollLock: 0x46,
  Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a,
  Numpad4: 0x4b, Numpad5: 0x4c, Numpad6: 0x4d, NumpadAdd: 0x4e, Numpad1: 0x4f,
  Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53,
  F11: 0x57, F12: 0x58,
  // Extended keys (0xE0xx)
  NumpadEnter: 0xe01c, ControlRight: 0xe01d, NumpadDivide: 0xe035,
  AltRight: 0xe038, Home: 0xe047, ArrowUp: 0xe048, PageUp: 0xe049,
  ArrowLeft: 0xe04b, ArrowRight: 0xe04d, End: 0xe04f, ArrowDown: 0xe050,
  PageDown: 0xe051, Insert: 0xe052, Delete: 0xe053, MetaLeft: 0xe05b,
  MetaRight: 0xe05c, ContextMenu: 0xe05d,
};

export interface InputAttachOptions {
  /** Called on every local pointer move with normalized coords, for the client-side cursor overlay (spec §16). */
  onLocalPointer?: (x: number, y: number) => void;
}

/**
 * Attach remote-desktop input capture to the element that displays the video.
 * Coordinates are normalized against the element's content box, respecting
 * the video's letterboxed aspect ratio via the supplied videoRect getter.
 *
 * Returns a cleanup function.
 */
export function attachRemoteInput(
  el: HTMLElement,
  session: RemoteSession,
  getVideoRect: () => DOMRect,
  opts: InputAttachOptions = {},
): () => void {
  const normalize = (ev: PointerEvent | WheelEvent | MouseEvent): { x: number; y: number } | null => {
    const rect = getVideoRect();
    if (rect.width <= 0 || rect.height <= 0) return null;
    const x = (ev.clientX - rect.left) / rect.width;
    const y = (ev.clientY - rect.top) / rect.height;
    if (x < 0 || x > 1 || y < 0 || y > 1) return null;
    return { x, y };
  };

  const onPointerMove = (ev: PointerEvent) => {
    const p = normalize(ev);
    if (!p) return;
    opts.onLocalPointer?.(p.x, p.y);
    // Use coalesced events for full motion fidelity, but never queue: send latest.
    session.sendPointerMove(encodePointerMove(p.x, p.y));
  };

  const onPointerDown = (ev: PointerEvent) => {
    el.focus();
    const p = normalize(ev);
    if (!p) return;
    ev.preventDefault();
    session.sendReliable({ t: "mb", b: ev.button, d: true, x: p.x, y: p.y });
  };

  const onPointerUp = (ev: PointerEvent) => {
    const p = normalize(ev);
    if (!p) return;
    ev.preventDefault();
    session.sendReliable({ t: "mb", b: ev.button, d: false, x: p.x, y: p.y });
  };

  const onWheel = (ev: WheelEvent) => {
    const p = normalize(ev);
    if (!p) return;
    ev.preventDefault();
    session.sendReliable({ t: "wheel", dx: ev.deltaX, dy: ev.deltaY, x: p.x, y: p.y });
  };

  const onContextMenu = (ev: Event) => ev.preventDefault();

  const onKeyDown = (ev: KeyboardEvent) => {
    ev.preventDefault();
    if (ev.repeat) return; // host OS applies its own key repeat
    session.sendReliable({ t: "kb", code: ev.code, sc: SCAN_CODES[ev.code] ?? 0, d: true });
  };

  const onKeyUp = (ev: KeyboardEvent) => {
    ev.preventDefault();
    session.sendReliable({ t: "kb", code: ev.code, sc: SCAN_CODES[ev.code] ?? 0, d: false });
  };

  el.tabIndex = 0;
  el.style.outline = "none";
  el.addEventListener("pointermove", onPointerMove);
  el.addEventListener("pointerdown", onPointerDown);
  el.addEventListener("pointerup", onPointerUp);
  el.addEventListener("wheel", onWheel, { passive: false });
  el.addEventListener("contextmenu", onContextMenu);
  el.addEventListener("keydown", onKeyDown);
  el.addEventListener("keyup", onKeyUp);

  return () => {
    el.removeEventListener("pointermove", onPointerMove);
    el.removeEventListener("pointerdown", onPointerDown);
    el.removeEventListener("pointerup", onPointerUp);
    el.removeEventListener("wheel", onWheel);
    el.removeEventListener("contextmenu", onContextMenu);
    el.removeEventListener("keydown", onKeyDown);
    el.removeEventListener("keyup", onKeyUp);
  };
}
