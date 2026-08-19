import { boolean, pgTable, serial, text, timestamp, varchar } from "drizzle-orm/pg-core";
import { createInsertSchema } from "drizzle-zod";
import { z } from "zod/v4";

export const devicesTable = pgTable("devices", {
  id: serial("id").primaryKey(),
  /** 9-digit public device id, e.g. "583491276" (cryptographically random). */
  publicDeviceId: varchar("public_device_id", { length: 16 }).notNull().unique(),
  /** sha256 hash of the long-lived device secret. */
  secretTokenHash: text("secret_token_hash").notNull(),
  name: text("name"),
  online: boolean("online").notNull().default(false),
  lastSeen: timestamp("last_seen"),
  createdAt: timestamp("created_at").notNull().defaultNow(),
});

export const insertDeviceSchema = createInsertSchema(devicesTable).omit({ id: true });
export type InsertDevice = z.infer<typeof insertDeviceSchema>;
export type Device = typeof devicesTable.$inferSelect;
