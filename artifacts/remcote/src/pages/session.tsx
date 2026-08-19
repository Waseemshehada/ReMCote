import { useEffect, useRef, useState, useCallback } from "react";
import { useRoute, useLocation } from "wouter";
import { RemoteSession, type SessionEvents } from "@/lib/remote/session";
import { attachRemoteInput } from "@/lib/remote/input";
import { StatsSampler, type HudStats, EMPTY_STATS } from "@/lib/remote/stats";
import { formatDeviceId, type SessionState, type HostCapabilities } from "@workspace/remcote-protocol";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { Maximize, Minimize, Settings2, MonitorPlay, X, Wifi, AlertCircle, CheckCircle2, ChevronRight, Activity, MousePointer2 } from "lucide-react";
import { cn } from "@/lib/utils";

export default function SessionPage() {
  const [, params] = useRoute("/session/:deviceId");
  const [, setLocation] = useLocation();
  const deviceId = params?.deviceId || "";

  const containerRef = useRef<HTMLDivElement>(null);
  const videoRef = useRef<HTMLVideoElement>(null);
  const cursorRef = useRef<HTMLDivElement>(null);
  
  const [sessionState, setSessionState] = useState<SessionState>("OFFLINE");
  const [stateMessage, setStateMessage] = useState<string>("");
  const [stats, setStats] = useState<HudStats>(EMPTY_STATS);
  const [caps, setCaps] = useState<HostCapabilities | null>(null);
  
  const [objectFit, setObjectFit] = useState<"contain" | "cover">("contain");
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [showStats, setShowStats] = useState(false);
  
  const sessionRef = useRef<RemoteSession | null>(null);
  const cleanupInputRef = useRef<(() => void) | null>(null);
  const statsSamplerRef = useRef<StatsSampler | null>(null);

  // Initialize session
  useEffect(() => {
    if (!deviceId) {
      setLocation("/");
      return;
    }

    const events: SessionEvents = {
      onState: (state, msg) => {
        setSessionState(state);
        if (msg) setStateMessage(msg);
      },
      onTrack: (stream) => {
        if (videoRef.current) {
          videoRef.current.srcObject = stream;
        }
      },
      onCapabilities: (capabilities) => {
        setCaps(capabilities);
      }
    };

    const session = new RemoteSession(deviceId, events);
    sessionRef.current = session;
    
    session.start().catch(err => {
      setSessionState("FAILED");
      setStateMessage(err.message || "Failed to start session");
    });

    return () => {
      session.close();
      sessionRef.current = null;
    };
  }, [deviceId, setLocation]);

  // Handle video element availability and input attachment
  useEffect(() => {
    if (sessionState !== "CONNECTED_DIRECT" && sessionState !== "CONNECTED_RELAY") {
      if (cleanupInputRef.current) {
        cleanupInputRef.current();
        cleanupInputRef.current = null;
      }
      if (statsSamplerRef.current) {
        statsSamplerRef.current.stop();
        statsSamplerRef.current = null;
      }
      return;
    }

    // Give the DOM a tiny tick to render the video element if it just transitioned
    const attachTimer = setTimeout(() => {
      if (!videoRef.current || !sessionRef.current) return;
      
      const getVideoRect = () => {
        const video = videoRef.current;
        if (!video) return new DOMRect();
        
        // Calculate the actual rendered video rectangle inside the container (letterboxing)
        const rect = video.getBoundingClientRect();
        const vw = video.videoWidth || 1920;
        const vh = video.videoHeight || 1080;
        
        const containerAspect = rect.width / rect.height;
        const videoAspect = vw / vh;
        
        let renderWidth = rect.width;
        let renderHeight = rect.height;
        let renderLeft = rect.left;
        let renderTop = rect.top;
        
        if (objectFit === "contain") {
          if (containerAspect > videoAspect) {
            // Pillarboxed (bars on sides)
            renderWidth = rect.height * videoAspect;
            renderLeft = rect.left + (rect.width - renderWidth) / 2;
          } else {
            // Letterboxed (bars on top/bottom)
            renderHeight = rect.width / videoAspect;
            renderTop = rect.top + (rect.height - renderHeight) / 2;
          }
        }
        
        return new DOMRect(renderLeft, renderTop, renderWidth, renderHeight);
      };

      cleanupInputRef.current = attachRemoteInput(videoRef.current, sessionRef.current, getVideoRect, {
        onLocalPointer: (x, y) => {
          if (cursorRef.current) {
            // x, y are 0..1 relative to the video rect
            // The cursor is positioned absolute relative to the video wrapper, so we map percentages directly
            cursorRef.current.style.left = `${x * 100}%`;
            cursorRef.current.style.top = `${y * 100}%`;
          }
        }
      });

      statsSamplerRef.current = new StatsSampler(
        () => sessionRef.current?.getPeerConnection() || null,
        (newStats) => setStats(newStats)
      );
      statsSamplerRef.current.start();
      
    }, 100);

    return () => {
      clearTimeout(attachTimer);
    };
  }, [sessionState, objectFit]);

  // Fullscreen listener
  useEffect(() => {
    const handleFsChange = () => {
      setIsFullscreen(!!document.fullscreenElement);
    };
    document.addEventListener("fullscreenchange", handleFsChange);
    return () => document.removeEventListener("fullscreenchange", handleFsChange);
  }, []);

  const toggleFullscreen = () => {
    if (!document.fullscreenElement && containerRef.current) {
      containerRef.current.requestFullscreen().catch(() => {});
    } else if (document.exitFullscreen) {
      document.exitFullscreen().catch(() => {});
    }
  };

  const isConnected = sessionState === "CONNECTED_DIRECT" || sessionState === "CONNECTED_RELAY";
  const isFailed = sessionState === "FAILED" || sessionState === "DISCONNECTED";

  // Formatted state for the checklist
  const getStepStatus = (step: string) => {
    const order = ["OFFLINE", "CONNECTING", "AWAITING_APPROVAL", "NEGOTIATING", "CONNECTED_DIRECT", "CONNECTED_RELAY"];
    
    // Treat direct and relay as equivalent connected states
    const effectiveCurrent = sessionState === "CONNECTED_RELAY" ? "CONNECTED_DIRECT" : sessionState;
    
    const currentIndex = order.indexOf(effectiveCurrent);
    const stepIndex = order.indexOf(step);
    
    if (isFailed) return "failed";
    if (currentIndex > stepIndex) return "done";
    if (currentIndex === stepIndex) return "active";
    return "pending";
  };

  return (
    <div ref={containerRef} className="w-screen h-[100dvh] bg-black text-foreground overflow-hidden flex flex-col relative font-sans">
      
      {/* ------------------------------------------------------------- */}
      {/* STATE: CONNECTING / FAILED (Opaque Overlay)                     */}
      {/* ------------------------------------------------------------- */}
      {!isConnected && (
        <div className="absolute inset-0 z-50 flex items-center justify-center bg-background/95 backdrop-blur-md">
          <Card className="w-full max-w-md bg-card/80 border-white/10 shadow-2xl p-8">
            <div className="flex flex-col items-center text-center space-y-6">
              
              <div className="w-16 h-16 rounded-full bg-black/50 border border-white/5 flex items-center justify-center shadow-inner">
                {isFailed ? (
                  <AlertCircle className="w-8 h-8 text-destructive" />
                ) : (
                  <Activity className="w-8 h-8 text-primary animate-pulse" />
                )}
              </div>
              
              <div className="space-y-2">
                <h2 className="text-2xl font-bold tracking-tight">
                  {isFailed ? "Connection Failed" : "Connecting to Host"}
                </h2>
                <p className="text-muted-foreground font-mono text-sm tracking-widest bg-black/30 py-1 px-3 rounded-md inline-block">
                  {formatDeviceId(deviceId)}
                </p>
              </div>

              {!isFailed && (
                <div className="w-full space-y-4 text-left bg-black/30 p-5 rounded-lg border border-white/5">
                  <ChecklistItem 
                    label="Finding device" 
                    status={getStepStatus("CONNECTING")} 
                  />
                  <ChecklistItem 
                    label="Waiting for approval on the host…" 
                    status={getStepStatus("AWAITING_APPROVAL")} 
                  />
                  <ChecklistItem 
                    label="Securing session & negotiating WebRTC" 
                    status={getStepStatus("NEGOTIATING")} 
                  />
                </div>
              )}

              {isFailed && (
                <div className="bg-destructive/10 border border-destructive/20 text-destructive-foreground p-4 rounded-lg text-sm text-left w-full">
                  <strong>Error:</strong> {stateMessage || "The remote host closed the connection or is unreachable."}
                </div>
              )}

              <div className="w-full pt-4 flex gap-3">
                <Button 
                  variant="outline" 
                  className="flex-1 border-white/10" 
                  onClick={() => setLocation("/")}
                >
                  Cancel
                </Button>
                {isFailed && (
                  <Button 
                    className="flex-1"
                    onClick={() => {
                      setSessionState("CONNECTING");
                      sessionRef.current?.start();
                    }}
                  >
                    Try Again
                  </Button>
                )}
              </div>

            </div>
          </Card>
        </div>
      )}

      {/* ------------------------------------------------------------- */}
      {/* STATE: CONNECTED (Live Video)                                   */}
      {/* ------------------------------------------------------------- */}
      {isConnected && (
        <>
          <div className="flex-1 relative w-full h-full bg-black overflow-hidden flex items-center justify-center">
            
            {/* The actual video element wrapper to constrain the cursor */}
            <div className="relative flex items-center justify-center w-full h-full">
              <video
                ref={videoRef}
                autoPlay
                playsInline
                muted
                className={cn(
                  "w-full h-full pointer-events-auto", // enable pointer events on video
                  objectFit === "contain" ? "object-contain" : "object-cover",
                  "cursor-none" // Hide native cursor over video
                )}
                style={{
                  // Prevent any CSS filters or animations that might slow down composite
                  filter: 'none',
                  transform: 'translateZ(0)', 
                }}
              />
              
              {/* Custom Cursor Overlay */}
              <div 
                ref={cursorRef} 
                className="absolute w-4 h-4 pointer-events-none z-40 origin-top-left"
                style={{ 
                  left: '50%', top: '50%', // default center, updated directly via DOM
                  transform: 'translate(-50%, -50%)' // center the dot on the exact coordinate
                }}
              >
                {/* A precise dot with a soft ring for visibility on light/dark backgrounds */}
                <div className="w-2.5 h-2.5 rounded-full bg-white border border-black/50 shadow-[0_0_4px_rgba(0,0,0,0.8)]" />
              </div>
            </div>

            {/* Performance HUD (Top Left) */}
            {showStats && (
              <div className="absolute top-4 left-4 z-50 bg-black/70 backdrop-blur-md border border-white/10 rounded-lg p-4 w-64 text-xs font-mono shadow-2xl pointer-events-none">
                <div className="flex items-center justify-between mb-3 border-b border-white/10 pb-2">
                  <span className="text-white/70 font-semibold tracking-wider">HUD</span>
                  <span className={cn(
                    "px-1.5 py-0.5 rounded text-[10px] uppercase font-bold",
                    sessionState === "CONNECTED_DIRECT" ? "bg-success/20 text-success" : "bg-warning/20 text-warning"
                  )}>
                    {sessionState === "CONNECTED_DIRECT" ? "DIRECT" : "RELAY"}
                  </span>
                </div>
                <div className="grid grid-cols-2 gap-y-2 gap-x-4">
                  <StatRow label="FPS" value={stats.fps?.toFixed(0)} />
                  <StatRow label="Bitrate" value={stats.bitrateMbps?.toFixed(2)} unit="Mbps" />
                  <StatRow label="RTT" value={stats.rttMs?.toFixed(0)} unit="ms" />
                  <StatRow label="Decode" value={stats.decodeMs?.toFixed(1)} unit="ms" />
                  <StatRow label="Loss" value={stats.packetLossPct?.toFixed(2)} unit="%" />
                  <StatRow label="Jitter" value={stats.jitterBufferMs?.toFixed(1)} unit="ms" />
                  <div className="col-span-2 mt-2 pt-2 border-t border-white/5 flex justify-between text-white/50">
                    <span>{stats.codec || "—"}</span>
                    <span>{stats.frameWidth || "—"} × {stats.frameHeight || "—"}</span>
                  </div>
                </div>
              </div>
            )}

            {/* Floating Toolbar (Bottom Center) */}
            <div className="absolute bottom-6 left-1/2 -translate-x-1/2 z-50 flex items-center gap-1.5 p-1.5 bg-black/60 backdrop-blur-xl border border-white/10 rounded-2xl shadow-2xl transition-opacity opacity-20 hover:opacity-100">
              
              <div className="px-3 flex items-center gap-2 border-r border-white/10 mr-1">
                <div className="w-2 h-2 rounded-full bg-success shadow-[0_0_8px_var(--success)]" />
                <span className="text-sm font-mono tracking-widest text-white/90">
                  {formatDeviceId(deviceId)}
                </span>
              </div>

              <Button 
                variant="ghost" 
                size="icon" 
                className="text-white/70 hover:text-white hover:bg-white/10 rounded-xl"
                onClick={() => setShowStats(!showStats)}
                title="Performance HUD"
              >
                <Activity size={18} />
              </Button>

              <Button 
                variant="ghost" 
                size="icon" 
                className="text-white/70 hover:text-white hover:bg-white/10 rounded-xl"
                onClick={() => setObjectFit(prev => prev === "contain" ? "cover" : "contain")}
                title={objectFit === "contain" ? "Fill Screen" : "Fit to Screen"}
              >
                {objectFit === "contain" ? <Maximize size={18} /> : <Minimize size={18} />}
              </Button>

              <Button 
                variant="ghost" 
                size="icon" 
                className="text-white/70 hover:text-white hover:bg-white/10 rounded-xl"
                onClick={toggleFullscreen}
                title="Fullscreen"
              >
                <MonitorPlay size={18} />
              </Button>

              <div className="w-[1px] h-6 bg-white/10 mx-1" />

              <Button 
                variant="ghost" 
                size="icon" 
                className="text-destructive/80 hover:text-destructive hover:bg-destructive/10 rounded-xl"
                onClick={() => setLocation("/")}
                title="Disconnect"
              >
                <X size={18} />
              </Button>

            </div>

          </div>
        </>
      )}

    </div>
  );
}

function ChecklistItem({ label, status }: { label: string, status: "pending" | "active" | "done" | "failed" }) {
  return (
    <div className="flex items-center gap-3 py-1">
      <div className="w-5 h-5 flex items-center justify-center shrink-0">
        {status === "done" && <CheckCircle2 className="w-5 h-5 text-success" />}
        {status === "active" && <div className="w-2 h-2 rounded-full bg-primary animate-ping" />}
        {status === "pending" && <div className="w-1.5 h-1.5 rounded-full bg-white/20" />}
      </div>
      <span className={cn(
        "text-sm font-medium transition-colors",
        status === "done" ? "text-white/70" : 
        status === "active" ? "text-primary" : 
        "text-white/30"
      )}>
        {label}
      </span>
    </div>
  );
}

function StatRow({ label, value, unit }: { label: string, value?: string | null, unit?: string }) {
  return (
    <div className="flex justify-between items-baseline">
      <span className="text-white/40">{label}</span>
      <span className="text-white/90 text-[11px]">
        {value === null || value === undefined ? "—" : value}
        {unit && value !== null && value !== undefined && <span className="text-white/40 ml-0.5">{unit}</span>}
      </span>
    </div>
  );
}
