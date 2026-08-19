import { Router, type IRouter } from "express";
import { count, eq } from "drizzle-orm";
import { db, devicesTable, sessionsTable } from "@workspace/db";
import {
  GetControlPlaneStatsResponse,
  GetDeviceStatusResponse,
  GetIceConfigResponse,
} from "@workspace/api-zod";
import { getIceServers } from "../lib/ice";
import { getPresenceStats, isHostOnline } from "../lib/signaling";

const router: IRouter = Router();

router.get("/devices/:publicDeviceId", async (req, res) => {
  const publicDeviceId = String(req.params["publicDeviceId"]).replace(/\D/g, "");
  const [row] = await db
    .select()
    .from(devicesTable)
    .where(eq(devicesTable.publicDeviceId, publicDeviceId))
    .limit(1);
  if (!row) {
    res.status(404).json({ error: "No device found with this ID" });
    return;
  }
  res.json(
    GetDeviceStatusResponse.parse({
      publicDeviceId: row.publicDeviceId,
      name: row.name,
      // Live presence wins over the persisted flag.
      online: isHostOnline(row.publicDeviceId),
      lastSeen: row.lastSeen ? row.lastSeen.toISOString() : null,
    }),
  );
});

router.get("/ice-config", (_req, res) => {
  res.json(GetIceConfigResponse.parse({ iceServers: getIceServers() }));
});

router.get("/stats", async (_req, res) => {
  const [devicesTotal] = await db.select({ n: count() }).from(devicesTable);
  const [sessionsTotal] = await db.select({ n: count() }).from(sessionsTable);
  const live = getPresenceStats();
  res.json(
    GetControlPlaneStatsResponse.parse({
      devicesOnline: live.hostsOnline,
      devicesTotal: devicesTotal?.n ?? 0,
      activeSessions: live.activeSessions,
      sessionsTotal: sessionsTotal?.n ?? 0,
    }),
  );
});

export default router;
