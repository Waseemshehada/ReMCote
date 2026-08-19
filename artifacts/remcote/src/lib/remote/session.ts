import {
  POINTER_CHANNEL_LABEL,
  RELIABLE_CHANNEL_LABEL,
  type HostCapabilities,
  type IceServerConfig,
  type ReliableInputMsg,
  type ServerToClient,
  type SessionState,
  type SignalPayload,
} from "@workspace/remcote-protocol";
import { SignalingClient } from "./signaling";

export interface SessionEvents {
  /** State machine transitions (spec §31). */
  onState: (state: SessionState, message?: string) => void;
  /** Remote video track arrived — attach to <video>. Called outside React state. */
  onTrack: (stream: MediaStream) => void;
  /** Host cursor metadata (optional). */
  onHostCursor?: (x: number, y: number, visible: boolean) => void;
  /** Measured input RTT in ms (ping/pong over input-reliable). */
  onInputRtt?: (rttMs: number) => void;
  onCapabilities?: (caps: HostCapabilities) => void;
}

/**
 * RemoteSession drives the full client lifecycle:
 * signaling connect -> approval -> WebRTC negotiation -> live P2P stream.
 *
 * The browser is the OFFERER: it creates both input data channels and a
 * recvonly video transceiver, so the host (answerer) only needs to attach
 * its encoded track. Media never touches the ReMCote server.
 */
export class RemoteSession {
  private signaling = new SignalingClient();
  private pc: RTCPeerConnection | null = null;
  private pointerCh: RTCDataChannel | null = null;
  private reliableCh: RTCDataChannel | null = null;
  private sessionId: string | null = null;
  private sessionToken: string | null = null;
  private unsubscribe: (() => void) | null = null;
  private pingTimer: number | null = null;
  private closed = false;
  private iceRestarting = false;
  private remoteDescriptionSet = false;
  private pendingCandidates: SignalPayload[] = [];

  state: SessionState = "OFFLINE";
  capabilities: HostCapabilities | null = null;

  constructor(
    private deviceId: string,
    private events: SessionEvents,
  ) {}

  async start(): Promise<void> {
    this.setState("CONNECTING", "Finding device");
    this.unsubscribe = this.signaling.subscribe((m) => this.onSignalingMessage(m));
    this.signaling.connect();
    await this.signaling.waitForOpen();
    this.signaling.send({ type: "client-connect-request", publicDeviceId: this.deviceId });
  }

  private setState(state: SessionState, message?: string) {
    this.state = state;
    this.events.onState(state, message);
  }

  private onSignalingMessage(msg: ServerToClient) {
    if (msg.type === "error") {
      this.setState("FAILED", msg.message);
      return;
    }
    if (msg.type === "client-session-state") {
      if (this.sessionId && msg.sessionId !== this.sessionId) return;
      this.sessionId = msg.sessionId;
      if (msg.sessionToken) this.sessionToken = msg.sessionToken;
      if (msg.hostCapabilities) {
        this.capabilities = msg.hostCapabilities;
        this.events.onCapabilities?.(msg.hostCapabilities);
      }
      if (msg.state === "NEGOTIATING" && !this.pc) {
        this.setState("NEGOTIATING", "Establishing direct connection");
        void this.startWebRtc(msg.iceServers ?? []);
      } else if (msg.state === "FAILED" || msg.state === "DISCONNECTED" || msg.state === "OFFLINE") {
        this.setState(msg.state, msg.message);
        this.teardownPeer();
      } else {
        this.setState(msg.state, msg.message);
      }
      return;
    }
    if (msg.type === "client-peer-signal" && msg.sessionId === this.sessionId) {
      void this.handlePeerSignal(msg.payload);
    }
  }

  private async startWebRtc(iceServers: IceServerConfig[]) {
    const pc = new RTCPeerConnection({
      iceServers: iceServers.map((s) => ({
        urls: s.urls,
        username: s.username,
        credential: s.credential,
      })),
    });
    this.pc = pc;

    // Freshness over retransmission for pointer motion (spec §15).
    this.pointerCh = pc.createDataChannel(POINTER_CHANNEL_LABEL, {
      ordered: false,
      maxRetransmits: 0,
    });
    this.pointerCh.binaryType = "arraybuffer";

    this.reliableCh = pc.createDataChannel(RELIABLE_CHANNEL_LABEL, { ordered: true });
    this.reliableCh.onmessage = (ev) => this.onReliableMessage(ev.data as string);
    this.reliableCh.onopen = () => this.startInputPing();

    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });

    pc.ontrack = (ev) => {
      if (ev.track.kind === "video") {
        const stream = ev.streams[0] ?? new MediaStream([ev.track]);
        this.events.onTrack(stream);
      }
    };

    pc.onicecandidate = (ev) => {
      if (ev.candidate) {
        this.sendSignal({
          kind: "candidate",
          candidate: ev.candidate.candidate,
          sdpMid: ev.candidate.sdpMid,
          sdpMLineIndex: ev.candidate.sdpMLineIndex,
        });
      } else {
        this.sendSignal({ kind: "candidate-end" });
      }
    };

    pc.onconnectionstatechange = () => {
      switch (pc.connectionState) {
        case "connected":
          void this.classifyConnection();
          this.iceRestarting = false;
          break;
        case "connecting":
          break;
        case "disconnected":
          void this.tryIceRestart();
          break;
        case "failed":
          if (!this.iceRestarting) void this.tryIceRestart();
          else this.setState("FAILED", "Direct connection failed");
          break;
        case "closed":
          if (!this.closed) this.setState("DISCONNECTED");
          break;
      }
    };

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    this.sendSignal({ kind: "offer", sdp: offer.sdp ?? "" });
  }

  /** Determine direct vs relay from the selected candidate pair. */
  private async classifyConnection() {
    if (!this.pc) return;
    let relay = false;
    try {
      const stats = await this.pc.getStats();
      stats.forEach((report) => {
        if (report.type === "candidate-pair" && report.nominated && report.state === "succeeded") {
          const local = stats.get(report.localCandidateId);
          const remote = stats.get(report.remoteCandidateId);
          if (local?.candidateType === "relay" || remote?.candidateType === "relay") relay = true;
        }
      });
    } catch {
      // stats unavailable — assume direct
    }
    // Tell the server the P2P link is live so it stops the negotiation timer.
    if (this.sessionId && this.sessionToken) {
      this.signaling.send({
        type: "client-session-established",
        sessionId: this.sessionId,
        sessionToken: this.sessionToken,
        connectionType: relay ? "relay" : "direct",
      });
    }
    this.setState(relay ? "CONNECTED_RELAY" : "CONNECTED_DIRECT");
  }

  private async tryIceRestart() {
    if (!this.pc || this.closed || this.iceRestarting) return;
    this.iceRestarting = true;
    this.setState("NEGOTIATING", "Reconnecting");
    // A fresh answer is coming — buffer candidates until it arrives again.
    this.remoteDescriptionSet = false;
    this.pendingCandidates = [];
    try {
      const offer = await this.pc.createOffer({ iceRestart: true });
      await this.pc.setLocalDescription(offer);
      this.sendSignal({ kind: "offer", sdp: offer.sdp ?? "" });
    } catch {
      this.setState("FAILED", "Reconnect failed");
    }
  }

  private async handlePeerSignal(payload: SignalPayload) {
    const pc = this.pc;
    if (!pc) return;
    try {
      if (payload.kind === "answer") {
        await pc.setRemoteDescription({ type: "answer", sdp: payload.sdp });
        this.remoteDescriptionSet = true;
        // Drain any candidates that arrived before the answer.
        const queued = this.pendingCandidates;
        this.pendingCandidates = [];
        for (const c of queued) await this.applyCandidate(c);
      } else if (payload.kind === "candidate" || payload.kind === "candidate-end") {
        // Trickle ICE can deliver candidates before the remote description —
        // buffer until setRemoteDescription succeeds, then apply in order.
        if (!this.remoteDescriptionSet) {
          this.pendingCandidates.push(payload);
        } else {
          await this.applyCandidate(payload);
        }
      }
    } catch (err) {
      console.error("[remcote] signal handling error", err);
    }
  }

  private async applyCandidate(payload: SignalPayload) {
    const pc = this.pc;
    if (!pc) return;
    try {
      if (payload.kind === "candidate-end") {
        await pc.addIceCandidate(null as unknown as RTCIceCandidateInit);
      } else if (payload.kind === "candidate") {
        await pc.addIceCandidate({
          candidate: payload.candidate,
          sdpMid: payload.sdpMid ?? undefined,
          sdpMLineIndex: payload.sdpMLineIndex ?? undefined,
        });
      }
    } catch (err) {
      console.error("[remcote] addIceCandidate error", err);
    }
  }

  private sendSignal(payload: SignalPayload) {
    if (!this.sessionId || !this.sessionToken) return;
    this.signaling.send({
      type: "client-signal",
      sessionId: this.sessionId,
      sessionToken: this.sessionToken,
      payload,
    });
  }

  // --- input -------------------------------------------------------------

  /** Raw binary pointer motion — unreliable channel, newest frame wins. */
  sendPointerMove(buf: ArrayBuffer) {
    if (this.pointerCh?.readyState === "open") this.pointerCh.send(buf);
  }

  sendReliable(msg: ReliableInputMsg) {
    if (this.reliableCh?.readyState === "open") this.reliableCh.send(JSON.stringify(msg));
  }

  private onReliableMessage(data: string) {
    try {
      const msg = JSON.parse(data) as { t: string } & Record<string, unknown>;
      if (msg.t === "pong") {
        const ts = msg["ts"] as number;
        this.events.onInputRtt?.(performance.now() - ts);
      } else if (msg.t === "cursor") {
        this.events.onHostCursor?.(
          msg["x"] as number,
          msg["y"] as number,
          msg["visible"] as boolean,
        );
      }
    } catch {
      // ignore
    }
  }

  private startInputPing() {
    this.stopInputPing();
    this.pingTimer = window.setInterval(() => {
      this.sendReliable({ t: "ping", ts: performance.now() });
    }, 2000);
  }

  private stopInputPing() {
    if (this.pingTimer !== null) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  getPeerConnection(): RTCPeerConnection | null {
    return this.pc;
  }

  private teardownPeer() {
    this.stopInputPing();
    this.pointerCh?.close();
    this.reliableCh?.close();
    this.pc?.close();
    this.pointerCh = null;
    this.reliableCh = null;
    this.pc = null;
  }

  close(reason?: string) {
    if (this.closed) return;
    this.closed = true;
    if (this.sessionId && this.sessionToken) {
      this.signaling.send({
        type: "client-session-closed",
        sessionId: this.sessionId,
        sessionToken: this.sessionToken,
        reason,
      });
    }
    this.teardownPeer();
    this.unsubscribe?.();
    this.signaling.close();
    this.setState("DISCONNECTED");
  }
}
