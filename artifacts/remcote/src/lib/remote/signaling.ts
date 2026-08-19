import {
  SIGNALING_WS_PATH,
  type ClientToServer,
  type ServerToClient,
} from "@workspace/remcote-protocol";

export type SignalingListener = (msg: ServerToClient) => void;

/**
 * Thin reconnecting WebSocket wrapper for the ReMCote signaling channel.
 * Carries only JSON control messages — never media.
 */
export class SignalingClient {
  private ws: WebSocket | null = null;
  private listeners = new Set<SignalingListener>();
  private closedByUser = false;
  private reconnectDelayMs = 500;
  private openWaiters: Array<() => void> = [];

  onStatus: ((connected: boolean) => void) | null = null;

  connect(): void {
    this.closedByUser = false;
    const base = import.meta.env.BASE_URL.replace(/\/$/, "");
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const url = `${proto}//${location.host}${base}${SIGNALING_WS_PATH}`;
    const ws = new WebSocket(url);
    this.ws = ws;

    ws.onopen = () => {
      this.reconnectDelayMs = 500;
      this.onStatus?.(true);
      for (const w of this.openWaiters) w();
      this.openWaiters = [];
    };
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data as string) as ServerToClient;
        for (const l of this.listeners) l(msg);
      } catch {
        // ignore malformed frames
      }
    };
    ws.onclose = () => {
      this.onStatus?.(false);
      if (!this.closedByUser) {
        setTimeout(() => this.connect(), this.reconnectDelayMs);
        this.reconnectDelayMs = Math.min(this.reconnectDelayMs * 2, 8000);
      }
    };
    ws.onerror = () => {
      ws.close();
    };
  }

  async waitForOpen(): Promise<void> {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) return;
    if (!this.ws || this.ws.readyState > WebSocket.OPEN) this.connect();
    await new Promise<void>((resolve) => this.openWaiters.push(resolve));
  }

  send(msg: ClientToServer): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(msg));
    }
  }

  subscribe(listener: SignalingListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  close(): void {
    this.closedByUser = true;
    this.ws?.close();
    this.ws = null;
  }
}
