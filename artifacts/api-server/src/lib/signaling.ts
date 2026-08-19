/**
 * ReMCote signaling server — the control plane.
 *
 * Responsibilities: device presence, Device ID registration, connection
 * requests, WebRTC offer/answer + ICE relay, session authorization.
 * It NEVER touches media. Video/input travel P2P via WebRTC.
 */
import { createHash, randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import type { IncomingMessage } from "node:http";
import type { Server as HttpServer } from "node:http";
import { WebSocketServer, WebSocket } from "ws";
import { eq } from "drizzle-orm";
import { db, devicesTable, sessionsTable } from "@workspace/db";
import type {
  ClientSessionStateMsg,
  ClientToServer,
  HostCapabilities,
  HostToServer,
  ServerToClient,
  ServerToHost,
  SessionState,
  SignalPayload,
} from "@workspace/remcote-protocol";
import { logger } from "./logger";
import { getIceServers } from "./ice";

const SESSION_TTL_MS = 2 * 60 * 1000; // pending approval expires quickly (spec §40)
const NEGOTIATION_TTL_MS = 30 * 1000; // approved-but-not-yet-connected window
const CLIENT_RECONNECT_GRACE_MS = 30 * 1000;
const HOST_RECONNECT_GRACE_MS = 30 * 1000;
const HOST_TIMEOUT_MS = 45 * 1000;
const MAX_PENDING_CLIENT_SIGNALS = 256;

interface HostConn {
  ws: WebSocket;
  deviceRowId: number;
  publicDeviceId: string;
  capabilities: HostCapabilities | null;
  lastHeartbeat: number;
}

interface LiveSession {
  sessionId: string;
  tokenHash: string;
  hostDeviceId: string;
  clientWs: WebSocket | null;
  state: SessionState;
  message: string;
  hostCapabilities: HostCapabilities | null;
  pendingClientSignals: SignalPayload[];
  clientReconnectBy: number | null;
  hostReconnectBy: number | null;
  /** Deadline for the current phase (approval, then negotiation). Once the
   * client reports the P2P link is established this is cleared to Infinity. */
  expiresAt: number;
}

const hostsByDeviceId = new Map<string, HostConn>();
const hostsByWs = new Map<WebSocket, HostConn>();
const sessionsById = new Map<string, LiveSession>();
const sessionsByClientWs = new Map<WebSocket, Set<string>>();

const sha256 = (v: string) => createHash("sha256").update(v).digest("hex");
const safeEqualHex = (a: string, b: string) => {
  const ba = Buffer.from(a, "hex");
  const bb = Buffer.from(b, "hex");
  return ba.length === bb.length && timingSafeEqual(ba, bb);
};

/** Cryptographically random 9-digit device id (never sequential, spec §27). */
function generateDeviceId(): string {
  return String(randomInt(100_000_000, 1_000_000_000));
}

function send(ws: WebSocket, msg: ServerToClient | ServerToHost): boolean {
  if (ws.readyState !== WebSocket.OPEN) return false;
  try {
    ws.send(JSON.stringify(msg));
    return true;
  } catch {
    return false;
  }
}

function bindClientSocket(session: LiveSession, ws: WebSocket) {
  const previous = session.clientWs;
  if (previous && previous !== ws) {
    sessionsByClientWs.get(previous)?.delete(session.sessionId);
  }
  session.clientWs = ws;
  session.clientReconnectBy = null;
  let set = sessionsByClientWs.get(ws);
  if (!set) sessionsByClientWs.set(ws, (set = new Set()));
  set.add(session.sessionId);
}

function sessionStateMessage(session: LiveSession): ClientSessionStateMsg {
  return {
    type: "client-session-state",
    sessionId: session.sessionId,
    state: session.state,
    message: session.message,
    hostCapabilities: session.hostCapabilities ?? undefined,
    iceServers: session.state === "NEGOTIATING" ? getIceServers() : undefined,
  };
}

function sendSessionState(session: LiveSession): boolean {
  return session.clientWs
    ? send(session.clientWs, sessionStateMessage(session))
    : false;
}

function queueClientSignal(session: LiveSession, payload: SignalPayload) {
  if (session.pendingClientSignals.length >= MAX_PENDING_CLIENT_SIGNALS) {
    // SDP offers/answers and end-of-candidates are control-plane boundaries.
    // Only ordinary ICE candidates are safely disposable under pressure.
    const candidateIndex = session.pendingClientSignals.findIndex(
      (queued) => queued.kind === "candidate",
    );
    if (candidateIndex >= 0) {
      session.pendingClientSignals.splice(candidateIndex, 1);
    } else if (payload.kind === "candidate") {
      logger.warn(
        { sessionId: session.sessionId },
        "[signaling] dropped queued ICE candidate without dropping SDP",
      );
      return;
    } else {
      logger.warn(
        { sessionId: session.sessionId },
        "[signaling] control signal queue exceeded its ICE budget",
      );
    }
  }
  session.pendingClientSignals.push(payload);
}

function persistSessionState(
  sessionId: string,
  state: "ACTIVE" | "DECLINED" | "ENDED",
  endedReason?: string,
) {
  void db
    .update(sessionsTable)
    .set({ state, ...(endedReason ? { endedReason } : {}) })
    .where(eq(sessionsTable.sessionId, sessionId))
    .catch((err) =>
      logger.error({ err, sessionId }, "[signaling] session persistence failed"),
    );
}

export function getPresenceStats() {
  let active = 0;
  for (const s of sessionsById.values()) {
    if (
      s.state === "NEGOTIATING" ||
      s.state === "CONNECTED_DIRECT" ||
      s.state === "CONNECTED_RELAY"
    ) {
      active++;
    }
  }
  return { hostsOnline: hostsByDeviceId.size, activeSessions: active };
}

export function isHostOnline(publicDeviceId: string): boolean {
  return hostsByDeviceId.has(publicDeviceId);
}

// ---------------------------------------------------------------------------
// Host message handling
// ---------------------------------------------------------------------------

async function handleHostRegister(
  ws: WebSocket,
  msg: Extract<HostToServer, { type: "host-register" }>,
) {
  let deviceRowId: number;
  let publicDeviceId: string;
  let secretToken: string;

  if (msg.publicDeviceId && msg.secretToken) {
    const [row] = await db
      .select()
      .from(devicesTable)
      .where(eq(devicesTable.publicDeviceId, msg.publicDeviceId))
      .limit(1);
    if (!row || !safeEqualHex(row.secretTokenHash, sha256(msg.secretToken))) {
      send(ws, { type: "error", code: "BAD_CREDENTIALS", message: "Unknown device or bad secret" });
      ws.close();
      return;
    }
    deviceRowId = row.id;
    publicDeviceId = row.publicDeviceId;
    secretToken = msg.secretToken;
    await db
      .update(devicesTable)
      .set({ online: true, lastSeen: new Date(), name: msg.name ?? row.name })
      .where(eq(devicesTable.id, row.id));
  } else {
    // Issue a fresh identity. Retry on the (unlikely) id collision.
    secretToken = randomBytes(32).toString("hex");
    let attempts = 0;
    for (;;) {
      publicDeviceId = generateDeviceId();
      try {
        const [row] = await db
          .insert(devicesTable)
          .values({
            publicDeviceId,
            secretTokenHash: sha256(secretToken),
            name: msg.name ?? null,
            online: true,
            lastSeen: new Date(),
          })
          .returning({ id: devicesTable.id });
        deviceRowId = row!.id;
        break;
      } catch (err) {
        if (++attempts >= 5) throw err;
      }
    }
  }

  const conn: HostConn = {
    ws,
    deviceRowId,
    publicDeviceId,
    capabilities: msg.capabilities ?? null,
    lastHeartbeat: Date.now(),
  };
  // Replace any stale connection for the same device.
  const stale = hostsByDeviceId.get(publicDeviceId);
  if (stale && stale.ws !== ws) stale.ws.close();
  hostsByDeviceId.set(publicDeviceId, conn);
  hostsByWs.set(ws, conn);
  for (const session of sessionsById.values()) {
    if (session.hostDeviceId === publicDeviceId) {
      session.hostReconnectBy = null;
    }
  }

  send(ws, {
    type: "host-registered",
    publicDeviceId,
    secretToken,
    iceServers: getIceServers(),
  });
  logger.info({ publicDeviceId }, "[signaling] host registered");
}

async function handleHostMessage(ws: WebSocket, msg: HostToServer) {
  if (msg.type === "host-register") {
    await handleHostRegister(ws, msg);
    return;
  }
  const host = hostsByWs.get(ws);
  if (!host) {
    send(ws, { type: "error", code: "NOT_REGISTERED", message: "Register first" });
    return;
  }
  switch (msg.type) {
    case "host-heartbeat": {
      host.lastHeartbeat = Date.now();
      await db
        .update(devicesTable)
        .set({ lastSeen: new Date(), online: true })
        .where(eq(devicesTable.id, host.deviceRowId));
      break;
    }
    case "host-connect-response": {
      const session = sessionsById.get(msg.sessionId);
      if (!session || session.hostDeviceId !== host.publicDeviceId) {
        logger.warn(
          { sessionId: msg.sessionId, publicDeviceId: host.publicDeviceId },
          "[signaling] ignored response for unknown session",
        );
        return;
      }
      if (session.state !== "AWAITING_APPROVAL") {
        logger.warn(
          { sessionId: msg.sessionId, state: session.state },
          "[signaling] ignored duplicate host response",
        );
        return;
      }
      logger.info(
        { sessionId: msg.sessionId, accepted: msg.accept },
        "[signaling] host approval received",
      );
      if (msg.accept) {
        session.state = "NEGOTIATING";
        session.message = "Establishing direct connection";
        session.expiresAt = Date.now() + NEGOTIATION_TTL_MS;
        session.hostReconnectBy = null;
        persistSessionState(msg.sessionId, "ACTIVE");
        const delivered = sendSessionState(session);
        logger.info(
          { sessionId: msg.sessionId, delivered },
          delivered
            ? "[signaling] negotiation state delivered to viewer"
            : "[signaling] negotiation state retained for viewer reconnect",
        );
      } else {
        await endSession(msg.sessionId, "FAILED", "The host declined the connection");
      }
      break;
    }
    case "host-signal": {
      const session = sessionsById.get(msg.sessionId);
      if (!session || session.hostDeviceId !== host.publicDeviceId) return;
      // Only relay signaling for an approved session (spec §4 consent boundary).
      if (session.state === "AWAITING_APPROVAL") return;
      const outgoing: ServerToClient = {
        type: "client-peer-signal",
        sessionId: session.sessionId,
        payload: msg.payload,
      };
      const delivered = session.clientWs
        ? send(session.clientWs, outgoing)
        : false;
      if (!delivered) queueClientSignal(session, msg.payload);
      logger.info(
        { sessionId: msg.sessionId, kind: msg.payload.kind, delivered },
        delivered
          ? "[signaling] host peer signal relayed"
          : "[signaling] host peer signal queued for viewer reconnect",
      );
      break;
    }
    case "host-session-closed": {
      const session = sessionsById.get(msg.sessionId);
      if (!session || session.hostDeviceId !== host.publicDeviceId) return;
      await endSession(msg.sessionId, "DISCONNECTED", msg.reason ?? "Host ended the session");
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Client message handling
// ---------------------------------------------------------------------------

async function handleClientConnectRequest(
  ws: WebSocket,
  msg: Extract<ClientToServer, { type: "client-connect-request" }>,
) {
  const publicDeviceId = msg.publicDeviceId.replace(/\D/g, "");
  const host = hostsByDeviceId.get(publicDeviceId);
  const sessionId = randomBytes(16).toString("hex");

  if (!host) {
    // Distinguish unknown vs offline for a useful message; both end the flow.
    const [row] = await db
      .select({ id: devicesTable.id })
      .from(devicesTable)
      .where(eq(devicesTable.publicDeviceId, publicDeviceId))
      .limit(1);
    send(ws, {
      type: "client-session-state",
      sessionId,
      state: "OFFLINE",
      message: row ? "This device is offline right now" : "No device found with this ID",
    });
    return;
  }

  const sessionToken = randomBytes(24).toString("hex");
  const expiresAt = Date.now() + SESSION_TTL_MS;
  await db.insert(sessionsTable).values({
    sessionId,
    hostDeviceId: host.deviceRowId,
    sessionTokenHash: sha256(sessionToken),
    state: "PENDING",
    expiresAt: new Date(expiresAt),
  });

  const live: LiveSession = {
    sessionId,
    tokenHash: sha256(sessionToken),
    hostDeviceId: publicDeviceId,
    clientWs: ws,
    state: "AWAITING_APPROVAL",
    message: "Waiting for the host to allow this connection",
    hostCapabilities: host.capabilities,
    pendingClientSignals: [],
    clientReconnectBy: null,
    hostReconnectBy: null,
    expiresAt,
  };
  sessionsById.set(sessionId, live);
  bindClientSocket(live, ws);

  send(ws, {
    type: "client-session-state",
    sessionId,
    sessionToken,
    state: "AWAITING_APPROVAL",
    message: "Waiting for the host to allow this connection",
    hostCapabilities: host.capabilities ?? undefined,
  });
  send(host.ws, {
    type: "host-connect-request",
    sessionId,
    clientDescription: "A computer wants to connect",
  });
  logger.info(
    { publicDeviceId, sessionId },
    "[signaling] connect request delivered to host",
  );
}

function authorizeClient(msg: { sessionId: string; sessionToken: string }, ws: WebSocket) {
  const session = sessionsById.get(msg.sessionId);
  if (!session) return null;
  if (session.clientWs !== ws) return null;
  if (!safeEqualHex(session.tokenHash, sha256(msg.sessionToken))) return null;
  return session;
}

async function handleClientMessage(ws: WebSocket, msg: ClientToServer) {
  switch (msg.type) {
    case "client-connect-request":
      await handleClientConnectRequest(ws, msg);
      break;
    case "client-resume-session": {
      const session = sessionsById.get(msg.sessionId);
      if (
        !session ||
        !safeEqualHex(session.tokenHash, sha256(msg.sessionToken))
      ) {
        send(ws, {
          type: "error",
          code: "SESSION_RESUME_FAILED",
          message: "The remote session can no longer be resumed",
        });
        logger.warn(
          { sessionId: msg.sessionId },
          "[signaling] viewer session resume rejected",
        );
        return;
      }
      bindClientSocket(session, ws);
      sendSessionState(session);
      const queuedSignals = session.pendingClientSignals.splice(0);
      for (const payload of queuedSignals) {
        send(ws, {
          type: "client-peer-signal",
          sessionId: session.sessionId,
          payload,
        });
      }
      logger.info(
        {
          sessionId: session.sessionId,
          state: session.state,
          queuedSignals: queuedSignals.length,
        },
        "[signaling] viewer session resumed",
      );
      break;
    }
    case "client-signal": {
      const session = authorizeClient(msg, ws);
      if (!session) {
        logger.warn(
          { sessionId: msg.sessionId, kind: msg.payload.kind },
          "[signaling] rejected unauthorized viewer signal",
        );
        return;
      }
      // Consent boundary: no client signaling reaches the host until approved.
      if (session.state === "AWAITING_APPROVAL") return;
      const host = hostsByDeviceId.get(session.hostDeviceId);
      if (!host) {
        logger.warn(
          { sessionId: msg.sessionId, kind: msg.payload.kind },
          "[signaling] viewer signal could not reach offline host",
        );
        return;
      }
      // Never infer "connected" from an SDP/ICE frame — the client reports it.
      const delivered = send(host.ws, {
        type: "host-peer-signal",
        sessionId: session.sessionId,
        payload: msg.payload,
      });
      logger.info(
        { sessionId: msg.sessionId, kind: msg.payload.kind, delivered },
        "[signaling] viewer peer signal relayed",
      );
      break;
    }
    case "client-session-established": {
      const session = authorizeClient(msg, ws);
      if (!session) return;
      if (session.state !== "NEGOTIATING") return;
      session.state = msg.connectionType === "relay" ? "CONNECTED_RELAY" : "CONNECTED_DIRECT";
      session.message =
        msg.connectionType === "relay"
          ? "Connected through relay"
          : "Connected directly";
      session.expiresAt = Number.POSITIVE_INFINITY; // established; stop the timer
      session.hostReconnectBy = null;
      persistSessionState(session.sessionId, "ACTIVE");
      logger.info(
        { sessionId: session.sessionId, connectionType: msg.connectionType },
        "[signaling] peer connection established",
      );
      break;
    }
    case "client-session-closed": {
      const session = authorizeClient(msg, ws);
      if (!session) return;
      await endSession(session.sessionId, "DISCONNECTED", msg.reason ?? "Client ended the session");
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

async function endSession(sessionId: string, state: SessionState, reason: string) {
  const session = sessionsById.get(sessionId);
  if (!session) return;
  sessionsById.delete(sessionId);
  if (session.clientWs) {
    sessionsByClientWs.get(session.clientWs)?.delete(sessionId);
    send(session.clientWs, {
      type: "client-session-state",
      sessionId,
      state,
      message: reason,
    });
  }
  persistSessionState(
    sessionId,
    state === "FAILED" ? "DECLINED" : "ENDED",
    reason,
  );
  const host = hostsByDeviceId.get(session.hostDeviceId);
  if (host) send(host.ws, { type: "host-session-ended", sessionId, reason });
  logger.info({ sessionId, state, reason }, "[signaling] session ended");
}

async function handleDisconnect(ws: WebSocket) {
  const host = hostsByWs.get(ws);
  if (host) {
    hostsByWs.delete(ws);
    const isCurrentConnection =
      hostsByDeviceId.get(host.publicDeviceId)?.ws === ws;
    if (isCurrentConnection) {
      hostsByDeviceId.delete(host.publicDeviceId);
      await db
        .update(devicesTable)
        .set({ online: false, lastSeen: new Date() })
        .where(eq(devicesTable.id, host.deviceRowId));
      // Keep pending sessions alive briefly while the Windows host reconnects.
      // A superseded socket must not impose a deadline on its replacement.
      const reconnectBy = Date.now() + HOST_RECONNECT_GRACE_MS;
      for (const session of sessionsById.values()) {
        if (
          session.hostDeviceId === host.publicDeviceId &&
          (session.state === "AWAITING_APPROVAL" || session.state === "NEGOTIATING")
        ) {
          session.hostReconnectBy = reconnectBy;
        }
      }
    }
    logger.info(
      {
        publicDeviceId: host.publicDeviceId,
        stale: !isCurrentConnection,
        reconnectGraceMs: HOST_RECONNECT_GRACE_MS,
      },
      "[signaling] host disconnected",
    );
    return;
  }
  const owned = sessionsByClientWs.get(ws);
  if (owned) {
    sessionsByClientWs.delete(ws);
    for (const id of owned) {
      const session = sessionsById.get(id);
      if (!session || session.clientWs !== ws) continue;
      session.clientWs = null;
      session.clientReconnectBy = Date.now() + CLIENT_RECONNECT_GRACE_MS;
      logger.info(
        { sessionId: id, graceMs: CLIENT_RECONNECT_GRACE_MS },
        "[signaling] viewer disconnected; awaiting resume",
      );
    }
  }
}

/** Periodic sweep: expire stale pending sessions and dead hosts. */
function startSweeper() {
  setInterval(() => {
    const now = Date.now();
    for (const [id, s] of sessionsById) {
      if (
        !s.clientWs &&
        s.clientReconnectBy !== null &&
        now > s.clientReconnectBy
      ) {
        void endSession(id, "DISCONNECTED", "Viewer signaling connection was lost");
      } else if (
        s.hostReconnectBy !== null &&
        now > s.hostReconnectBy &&
        (s.state === "AWAITING_APPROVAL" || s.state === "NEGOTIATING")
      ) {
        void endSession(id, "FAILED", "Host signaling connection was lost");
      } else if (s.state === "AWAITING_APPROVAL" && now > s.expiresAt) {
        void endSession(id, "FAILED", "Connection request timed out");
      } else if (s.state === "NEGOTIATING" && now > s.expiresAt) {
        void endSession(id, "FAILED", "Could not establish a direct connection");
      }
    }
    for (const [deviceId, host] of hostsByDeviceId) {
      if (now - host.lastHeartbeat > HOST_TIMEOUT_MS) {
        logger.warn({ deviceId }, "[signaling] host heartbeat timeout");
        host.ws.close();
      }
    }
  }, 10_000).unref();
}

export function attachSignaling(server: HttpServer) {
  const wss = new WebSocketServer({ noServer: true });

  server.on("upgrade", (req: IncomingMessage, socket, head) => {
    const url = new URL(req.url ?? "/", "http://localhost");
    if (url.pathname === "/api/ws" || url.pathname.endsWith("/api/ws")) {
      wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws, req));
    } else {
      socket.destroy();
    }
  });

  wss.on("connection", (ws: WebSocket) => {
    ws.on("message", (data) => {
      let parsed: unknown;
      try {
        parsed = JSON.parse(String(data));
      } catch {
        return;
      }
      // Reject anything that is not a well-formed, typed message before dispatch.
      if (
        typeof parsed !== "object" ||
        parsed === null ||
        typeof (parsed as { type?: unknown }).type !== "string"
      ) {
        return;
      }
      const msg = parsed as HostToServer | ClientToServer;
      const handler = msg.type.startsWith("host-")
        ? handleHostMessage(ws, msg as HostToServer)
        : handleClientMessage(ws, msg as ClientToServer);
      handler.catch((err) => logger.error({ err }, "[signaling] handler error"));
    });
    ws.on("close", () => {
      void handleDisconnect(ws).catch((err) =>
        logger.error({ err }, "[signaling] disconnect error"),
      );
    });
  });

  startSweeper();
  logger.info("[signaling] WebSocket signaling attached at /api/ws");
}
