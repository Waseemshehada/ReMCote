/**
 * Performance HUD sampler. Polls RTCPeerConnection.getStats once per second
 * and derives real, measured values — never invented (spec rule 9).
 */
export interface HudStats {
  fps: number | null;
  bitrateMbps: number | null;
  rttMs: number | null;
  packetLossPct: number | null;
  framesDropped: number | null;
  frameWidth: number | null;
  frameHeight: number | null;
  codec: string | null;
  jitterBufferMs: number | null;
  decodeMs: number | null;
  connectionType: "direct" | "relay" | null;
}

export const EMPTY_STATS: HudStats = {
  fps: null,
  bitrateMbps: null,
  rttMs: null,
  packetLossPct: null,
  framesDropped: null,
  frameWidth: null,
  frameHeight: null,
  codec: null,
  jitterBufferMs: null,
  decodeMs: null,
  connectionType: null,
};

export class StatsSampler {
  private timer: number | null = null;
  private lastBytes = 0;
  private lastTs = 0;
  private lastFramesDecoded = 0;
  private lastJitterEmitted = 0;
  private lastJitterDelay = 0;
  private lastDecodeSum = 0;
  private lastPacketsReceived = 0;
  private lastPacketsLost = 0;

  constructor(
    private getPc: () => RTCPeerConnection | null,
    private onSample: (stats: HudStats) => void,
  ) {}

  start() {
    this.stop();
    this.timer = window.setInterval(() => void this.sample(), 1000);
  }

  stop() {
    if (this.timer !== null) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  private async sample() {
    const pc = this.getPc();
    if (!pc || pc.connectionState !== "connected") return;
    let report: RTCStatsReport;
    try {
      report = await pc.getStats();
    } catch {
      return;
    }

    const out: HudStats = { ...EMPTY_STATS };
    const codecs = new Map<string, string>();
    report.forEach((s) => {
      if (s.type === "codec") codecs.set(s.id, (s.mimeType as string) ?? "");
    });

    report.forEach((s) => {
      if (s.type === "inbound-rtp" && s.kind === "video") {
        const now = s.timestamp as number;
        const bytes = (s.bytesReceived as number) ?? 0;
        if (this.lastTs > 0 && now > this.lastTs) {
          out.bitrateMbps = ((bytes - this.lastBytes) * 8) / ((now - this.lastTs) / 1000) / 1e6;
          const framesDecoded = (s.framesDecoded as number) ?? 0;
          out.fps = (framesDecoded - this.lastFramesDecoded) / ((now - this.lastTs) / 1000);
          this.lastFramesDecoded = framesDecoded;

          const emitted = (s.framesDecoded as number) ?? 0;
          const jbDelay = (s.jitterBufferDelay as number) ?? 0;
          const dEmitted = emitted - this.lastJitterEmitted;
          if (dEmitted > 0) {
            out.jitterBufferMs = ((jbDelay - this.lastJitterDelay) / dEmitted) * 1000;
            out.decodeMs =
              ((((s.totalDecodeTime as number) ?? 0) - this.lastDecodeSum) / dEmitted) * 1000;
          }
          this.lastJitterEmitted = emitted;
          this.lastJitterDelay = jbDelay;
          this.lastDecodeSum = (s.totalDecodeTime as number) ?? 0;

          const packets = (s.packetsReceived as number) ?? 0;
          const lost = (s.packetsLost as number) ?? 0;
          const dPackets = packets - this.lastPacketsReceived;
          const dLost = lost - this.lastPacketsLost;
          if (dPackets + dLost > 0) out.packetLossPct = (dLost / (dPackets + dLost)) * 100;
          this.lastPacketsReceived = packets;
          this.lastPacketsLost = lost;
        }
        this.lastBytes = bytes;
        this.lastTs = now;
        out.framesDropped = (s.framesDropped as number) ?? null;
        out.frameWidth = (s.frameWidth as number) ?? null;
        out.frameHeight = (s.frameHeight as number) ?? null;
        const mime = codecs.get(s.codecId as string);
        if (mime) out.codec = mime.replace("video/", "");
      }
      if (s.type === "candidate-pair" && s.nominated && s.state === "succeeded") {
        if (typeof s.currentRoundTripTime === "number") {
          out.rttMs = s.currentRoundTripTime * 1000;
        }
        const local = report.get(s.localCandidateId as string);
        const remote = report.get(s.remoteCandidateId as string);
        out.connectionType =
          local?.candidateType === "relay" || remote?.candidateType === "relay"
            ? "relay"
            : "direct";
      }
    });

    this.onSample(out);
  }
}
