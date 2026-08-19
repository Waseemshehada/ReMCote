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
  private pendingMessages: ClientToServer[] = [];
  private reconnectTimer: number | null = null;

  onStatus: ((connected: boolean) => void) | null = null;

  connect(): void {
    if (
      this.ws?.readyState === WebSocket.CONNECTING ||
      this.ws?.readyState === WebSocket.OPEN
    ) {
      return;
    }
    this.closedByUser = false;
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    // The API artifact owns this route at the workspace root. Do not prepend
    // the web artifact's BASE_URL or a viewer mounted under a path would try
    // to upgrade its own Vite route instead of the signaling service.
    const url = `${proto}//${location.host}${SIGNALING_WS_PATH}`;
    const ws = new WebSocket(url);
    this.ws = ws;

    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.reconnectDelayMs = 500;
      this.onStatus?.(true);
      const pending = this.pendingMessages.splice(0);
      for (const msg of pending) ws.send(JSON.stringify(msg));
      for (const w of this.openWaiters) w();
      this.openWaiters = [];
    };
    ws.onmessage = (ev) => {
      if (this.ws !== ws) return;
      try {
        const msg = JSON.parse(ev.data as string) as ServerToClient;
        for (const l of this.listeners) l(msg);
      } catch {
        // ignore malformed frames
      }
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.onStatus?.(false);
      if (!this.closedByUser) {
        this.reconnectTimer = window.setTimeout(() => {
          this.reconnectTimer = null;
          this.connect();
        }, this.reconnectDelayMs);
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
    } else {
      // Keep SDP/ICE and session control messages across a short signaling
      // interruption. RemoteSession sends its resume message first on reopen.
      if (this.pendingMessages.length >= 512) {
        const candidateIndex = this.pendingMessages.findIndex(
          (queued) =>
            queued.type === "client-signal" &&
            queued.payload.kind === "candidate",
        );
        if (candidateIndex >= 0) {
          this.pendingMessages.splice(candidateIndex, 1);
        } else if (
          msg.type === "client-signal" &&
          msg.payload.kind === "candidate"
        ) {
          return;
        }
      }
      this.pendingMessages.push(msg);
    }
  }

  subscribe(listener: SignalingListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  close(): void {
    this.closedByUser = true;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.pendingMessages = [];
    this.ws?.close();
    this.ws = null;
  }
}
