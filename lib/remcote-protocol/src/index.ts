/**
 * ReMCote shared signaling + input protocol.
 *
 * Signaling runs as JSON messages over WebSocket at /api/ws.
 * Media and input NEVER touch the signaling server — they travel over
 * WebRTC P2P (video track + two data channels).
 *
 * The Windows host implements the exact same wire protocol in C++
 * (see windows-host/ and docs/protocol.md).
 */

// ---------------------------------------------------------------------------
// Connection / session states (spec §31)
// ---------------------------------------------------------------------------

export type SessionState =
  | "OFFLINE"
  | "CONNECTING"
  | "AWAITING_APPROVAL"
  | "NEGOTIATING"
  | "CONNECTED_DIRECT"
  | "CONNECTED_RELAY"
  | "FAILED"
  | "DISCONNECTED";

// ---------------------------------------------------------------------------
// WebSocket signaling messages
// ---------------------------------------------------------------------------

/** SDP or ICE payload relayed verbatim between peers. */
export type SignalPayload =
  | { kind: "offer"; sdp: string }
  | { kind: "answer"; sdp: string }
  | { kind: "candidate"; candidate: string; sdpMid?: string | null; sdpMLineIndex?: number | null }
  | { kind: "candidate-end" };

/** Host capabilities advertised at registration (spec §44). */
export interface HostCapabilities {
  h264: boolean;
  hevc?: boolean;
  av1?: boolean;
  maxWidth?: number;
  maxHeight?: number;
  maxFps?: number;
  gpuName?: string;
  encoderName?: string;
  desktopWidth?: number;
  desktopHeight?: number;
  desktopHz?: number;
}

// --- Host -> Server ---
export interface HostRegisterMsg {
  type: "host-register";
  /** Present on re-registration; omit to be issued a new identity. */
  publicDeviceId?: string;
  /** Long-lived device secret proving ownership of publicDeviceId. */
  secretToken?: string;
  name?: string;
  capabilities?: HostCapabilities;
}
export interface HostHeartbeatMsg {
  type: "host-heartbeat";
}
export interface HostConnectResponseMsg {
  type: "host-connect-response";
  sessionId: string;
  accept: boolean;
}
export interface HostSignalMsg {
  type: "host-signal";
  sessionId: string;
  payload: SignalPayload;
}
export interface HostSessionClosedMsg {
  type: "host-session-closed";
  sessionId: string;
  reason?: string;
}
export type HostToServer =
  | HostRegisterMsg
  | HostHeartbeatMsg
  | HostConnectResponseMsg
  | HostSignalMsg
  | HostSessionClosedMsg;

// --- Server -> Host ---
export interface HostRegisteredMsg {
  type: "host-registered";
  publicDeviceId: string;
  secretToken: string;
  iceServers: IceServerConfig[];
}
export interface HostConnectRequestMsg {
  type: "host-connect-request";
  sessionId: string;
  /** Coarse, non-identifying description of the requester. */
  clientDescription: string;
}
export interface HostPeerSignalMsg {
  type: "host-peer-signal";
  sessionId: string;
  payload: SignalPayload;
}
export interface HostSessionEndedMsg {
  type: "host-session-ended";
  sessionId: string;
  reason?: string;
}
export interface ErrorMsg {
  type: "error";
  code: string;
  message: string;
}
export type ServerToHost =
  | HostRegisteredMsg
  | HostConnectRequestMsg
  | HostPeerSignalMsg
  | HostSessionEndedMsg
  | ErrorMsg;

// --- Client (browser viewer) -> Server ---
export interface ClientConnectRequestMsg {
  type: "client-connect-request";
  /** Normalized 9-digit device id (no spaces). */
  publicDeviceId: string;
}
/** Rebind an existing session to a replacement signaling WebSocket after a
 * transient network interruption. The short-lived token proves ownership. */
export interface ClientResumeSessionMsg {
  type: "client-resume-session";
  sessionId: string;
  sessionToken: string;
}
export interface ClientSignalMsg {
  type: "client-signal";
  sessionId: string;
  sessionToken: string;
  payload: SignalPayload;
}
export interface ClientSessionClosedMsg {
  type: "client-session-closed";
  sessionId: string;
  sessionToken: string;
  reason?: string;
}
/** Client tells the server the P2P connection is live so the server can stop
 * the negotiation timeout. Server never infers "connected" from SDP/ICE. */
export interface ClientSessionEstablishedMsg {
  type: "client-session-established";
  sessionId: string;
  sessionToken: string;
  connectionType: "direct" | "relay";
}
export type ClientToServer =
  | ClientConnectRequestMsg
  | ClientResumeSessionMsg
  | ClientSignalMsg
  | ClientSessionEstablishedMsg
  | ClientSessionClosedMsg;

// --- Server -> Client ---
export interface ClientSessionStateMsg {
  type: "client-session-state";
  sessionId: string;
  /** Short-lived token authorizing further signaling for this session. */
  sessionToken?: string;
  state: SessionState;
  message?: string;
  hostCapabilities?: HostCapabilities;
  iceServers?: IceServerConfig[];
}
export interface ClientPeerSignalMsg {
  type: "client-peer-signal";
  sessionId: string;
  payload: SignalPayload;
}
export type ServerToClient = ClientSessionStateMsg | ClientPeerSignalMsg | ErrorMsg;

export interface IceServerConfig {
  urls: string[];
  username?: string;
  credential?: string;
}

// ---------------------------------------------------------------------------
// WebRTC data channels
// ---------------------------------------------------------------------------

/** Unreliable, unordered — high-frequency pointer motion only (spec §15). */
export const POINTER_CHANNEL_LABEL = "input-pointer";
/** Reliable, ordered — buttons, keys, wheel, control, ping (spec §17). */
export const RELIABLE_CHANNEL_LABEL = "input-reliable";

/**
 * input-pointer binary format (little-endian), 9 bytes:
 *   u8  type = 1 (POINTER_MOVE)
 *   f32 x    normalized 0..1 relative to remote desktop
 *   f32 y    normalized 0..1
 */
export const POINTER_MOVE_TYPE = 1;
export const POINTER_MOVE_BYTES = 9;

export function encodePointerMove(x: number, y: number): ArrayBuffer {
  const buf = new ArrayBuffer(POINTER_MOVE_BYTES);
  const view = new DataView(buf);
  view.setUint8(0, POINTER_MOVE_TYPE);
  view.setFloat32(1, x, true);
  view.setFloat32(5, y, true);
  return buf;
}

/** JSON messages on input-reliable channel. */
export type ReliableInputMsg =
  /** Mouse button: b 0=left 1=middle 2=right 3=x1 4=x2, d=down, x/y normalized. */
  | { t: "mb"; b: number; d: boolean; x: number; y: number }
  /** Wheel deltas in browser pixels (host scales). */
  | { t: "wheel"; dx: number; dy: number; x: number; y: number }
  /** Keyboard: code = KeyboardEvent.code, sc = Windows scan code (0 if unknown), d=down. */
  | { t: "kb"; code: string; sc: number; d: boolean }
  /** Input latency probe — host echoes back as pong immediately. */
  | { t: "ping"; ts: number }
  | { t: "pong"; ts: number };

/** Host -> client cursor metadata channel messages (sent on input-reliable). */
export type HostCursorMsg = {
  t: "cursor";
  x: number;
  y: number;
  visible: boolean;
  shape?: string;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Normalize a user-typed device id: strip everything but digits. */
export function normalizeDeviceId(raw: string): string {
  return raw.replace(/\D/g, "");
}

/** Format 583491276 -> "583 491 276" for display. */
export function formatDeviceId(id: string): string {
  return id.replace(/(\d{3})(?=\d)/g, "$1 ").trim();
}

/** Path of the signaling WebSocket relative to the shared proxy. */
export const SIGNALING_WS_PATH = "/api/ws";
