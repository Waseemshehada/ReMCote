import { integer, pgTable, serial, text, timestamp, varchar } from "drizzle-orm/pg-core";
import { createInsertSchema } from "drizzle-zod";
import { z } from "zod/v4";
import { devicesTable } from "./devices";

export const sessionsTable = pgTable("sessions", {
  id: serial("id").primaryKey(),
  /** Random public session id used in signaling messages. */
  sessionId: varchar("session_id", { length: 64 }).notNull().unique(),
  hostDeviceId: integer("host_device_id")
    .notNull()
    .references(() => devicesTable.id),
  /** sha256 hash of the short-lived session token issued to the client. */
  sessionTokenHash: text("session_token_hash").notNull(),
  state: varchar("state", { length: 32 }).notNull().default("PENDING"),
  createdAt: timestamp("created_at").notNull().defaultNow(),
  expiresAt: timestamp("expires_at").notNull(),
  endedReason: text("ended_reason"),
});

export const insertSessionSchema = createInsertSchema(sessionsTable).omit({ id: true });
export type InsertSession = z.infer<typeof insertSessionSchema>;
export type Session = typeof sessionsTable.$inferSelect;
