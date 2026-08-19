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
  ClientToServer,
  HostCapabilities,
  HostToServer,
  ServerToClient,
  ServerToHost,
  SessionState,
} from "@workspace/remcote-protocol";
import { logger } from "./logger";
import { getIceServers } from "./ice";

const SESSION_TTL_MS = 2 * 60 * 1000; // pending approval expires quickly (spec §40)
const NEGOTIATION_TTL_MS = 30 * 1000; // approved-but-not-yet-connected window
const HOST_TIMEOUT_MS = 45 * 1000;

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
  clientWs: WebSocket;
  state: SessionState;
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

function send(ws: WebSocket, msg: ServerToClient | ServerToHost) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

export function getPresenceStats() {
  let active = 0;
  for (const s of sessionsById.values()) {
    if (s.state === "NEGOTIATING" || s.state === "CONNECTED_DIRECT") active++;
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
      if (!session || session.hostDeviceId !== host.publicDeviceId) return;
      if (session.state !== "AWAITING_APPROVAL") return; // approve only once
      if (msg.accept) {
        session.state = "NEGOTIATING";
        session.expiresAt = Date.now() + NEGOTIATION_TTL_MS;
        await db
          .update(sessionsTable)
          .set({ state: "ACTIVE" })
          .where(eq(sessionsTable.sessionId, msg.sessionId));
        send(session.clientWs, {
          type: "client-session-state",
          sessionId: session.sessionId,
          state: "NEGOTIATING",
          message: "Establishing direct connection",
          hostCapabilities: host.capabilities ?? undefined,
          iceServers: getIceServers(),
        });
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
      send(session.clientWs, {
        type: "client-peer-signal",
        sessionId: session.sessionId,
        payload: msg.payload,
      });
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
    expiresAt,
  };
  sessionsById.set(sessionId, live);
  let set = sessionsByClientWs.get(ws);
  if (!set) sessionsByClientWs.set(ws, (set = new Set()));
  set.add(sessionId);

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
  logger.info({ publicDeviceId, sessionId }, "[signaling] connect request");
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
    case "client-signal": {
      const session = authorizeClient(msg, ws);
      if (!session) return;
      // Consent boundary: no client signaling reaches the host until approved.
      if (session.state === "AWAITING_APPROVAL") return;
      const host = hostsByDeviceId.get(session.hostDeviceId);
      if (!host) return;
      // Never infer "connected" from an SDP/ICE frame — the client reports it.
      send(host.ws, {
        type: "host-peer-signal",
        sessionId: session.sessionId,
        payload: msg.payload,
      });
      break;
    }
    case "client-session-established": {
      const session = authorizeClient(msg, ws);
      if (!session) return;
      if (session.state !== "NEGOTIATING") return;
      session.state = msg.connectionType === "relay" ? "CONNECTED_RELAY" : "CONNECTED_DIRECT";
      session.expiresAt = Number.POSITIVE_INFINITY; // established; stop the timer
      await db
        .update(sessionsTable)
        .set({ state: "ACTIVE" })
        .where(eq(sessionsTable.sessionId, session.sessionId));
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
  sessionsByClientWs.get(session.clientWs)?.delete(sessionId);

  await db
    .update(sessionsTable)
    .set({ state: state === "FAILED" ? "DECLINED" : "ENDED", endedReason: reason })
    .where(eq(sessionsTable.sessionId, sessionId));

  send(session.clientWs, {
    type: "client-session-state",
    sessionId,
    state,
    message: reason,
  });
  const host = hostsByDeviceId.get(session.hostDeviceId);
  if (host) send(host.ws, { type: "host-session-ended", sessionId, reason });
}

async function handleDisconnect(ws: WebSocket) {
  const host = hostsByWs.get(ws);
  if (host) {
    hostsByWs.delete(ws);
    if (hostsByDeviceId.get(host.publicDeviceId)?.ws === ws) {
      hostsByDeviceId.delete(host.publicDeviceId);
    }
    await db
      .update(devicesTable)
      .set({ online: false, lastSeen: new Date() })
      .where(eq(devicesTable.id, host.deviceRowId));
    // Note: an established WebRTC session keeps running P2P even if the host's
    // signaling socket drops (spec §32) — we only end *pending* negotiations.
    for (const [id, s] of sessionsById) {
      // Established P2P sessions survive a host signaling drop (spec §32);
      // sessions still negotiating cannot proceed, so end them.
      if (
        s.hostDeviceId === host.publicDeviceId &&
        (s.state === "AWAITING_APPROVAL" || s.state === "NEGOTIATING")
      ) {
        await endSession(id, "FAILED", "Host went offline before the session was established");
      }
    }
    logger.info({ publicDeviceId: host.publicDeviceId }, "[signaling] host disconnected");
    return;
  }
  const owned = sessionsByClientWs.get(ws);
  if (owned) {
    sessionsByClientWs.delete(ws);
    for (const id of owned) {
      await endSession(id, "DISCONNECTED", "Client disconnected");
    }
  }
}

/** Periodic sweep: expire stale pending sessions and dead hosts. */
function startSweeper() {
  setInterval(() => {
    const now = Date.now();
    for (const [id, s] of sessionsById) {
      if (s.state === "AWAITING_APPROVAL" && now > s.expiresAt) {
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
