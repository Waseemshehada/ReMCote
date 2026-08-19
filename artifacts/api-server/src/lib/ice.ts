import type { IceServerConfig } from "@workspace/remcote-protocol";

/**
 * STUN/TURN configuration comes from the environment (spec §13).
 * TURN is optional fallback only; direct P2P is always preferred.
 */
export function getIceServers(): IceServerConfig[] {
  const servers: IceServerConfig[] = [];
  const stunUrl = process.env["STUN_URL"] ?? "stun:stun.l.google.com:19302";
  servers.push({ urls: [stunUrl] });

  const turnUrl = process.env["TURN_URL"];
  if (turnUrl) {
    const turn: IceServerConfig = { urls: [turnUrl] };
    const username = process.env["TURN_USERNAME"];
    const credential = process.env["TURN_PASSWORD"];
    if (username) turn.username = username;
    if (credential) turn.credential = credential;
    servers.push(turn);
  }
  return servers;
}
