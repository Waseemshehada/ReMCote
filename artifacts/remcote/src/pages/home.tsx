import { useLocation } from "wouter";
import { useState, FormEvent } from "react";
import { useGetControlPlaneStats, useGetDeviceStatus, getGetDeviceStatusQueryKey, getGetControlPlaneStatsQueryKey } from "@workspace/api-client-react";
import { formatDeviceId, normalizeDeviceId } from "@workspace/remcote-protocol";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { Button } from "@/components/ui/button";
import { MonitorPlay, DownloadCloud, Activity, ArrowRight, Server, ChevronRight } from "lucide-react";
import { useQueryClient } from "@tanstack/react-query";

export default function Home() {
  const [, setLocation] = useLocation();
  const [deviceId, setDeviceId] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [isChecking, setIsChecking] = useState(false);
  const queryClient = useQueryClient();

  const { data: stats } = useGetControlPlaneStats({ 
    query: { refetchInterval: 5000, queryKey: getGetControlPlaneStatsQueryKey() } 
  });

  const handleDeviceChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const normalized = normalizeDeviceId(e.target.value);
    // Max 9 digits
    if (normalized.length <= 9) {
      setDeviceId(formatDeviceId(normalized));
      setError(null);
    }
  };

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault();
    const normalized = normalizeDeviceId(deviceId);
    
    if (normalized.length !== 9) {
      setError("Device ID must be 9 digits");
      return;
    }

    setIsChecking(true);
    setError(null);

    try {
      // Optional pre-check
      const status = await queryClient.fetchQuery({
        queryKey: getGetDeviceStatusQueryKey(normalized),
        queryFn: () => fetch(`/api/devices/${normalized}`).then(res => {
          if (!res.ok) throw new Error("Network error");
          return res.json();
        })
      }).catch(() => null);

      if (status && !status.online) {
        setError("Device is offline");
        setIsChecking(false);
        return;
      }

      setLocation(`/session/${normalized}`);
    } catch (err) {
      // If the API fails, we just try to connect anyway
      setLocation(`/session/${normalized}`);
    } finally {
      setIsChecking(false);
    }
  };

  return (
    <div className="min-h-[100dvh] flex flex-col bg-background relative overflow-hidden">
      {/* Subtle background glow */}
      <div className="absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 w-[800px] h-[800px] bg-primary/5 rounded-full blur-[120px] pointer-events-none" />

      {/* Header */}
      <header className="px-8 py-6 flex items-center justify-between z-10">
        <div className="flex items-center gap-3 text-foreground">
          <div className="w-8 h-8 rounded bg-primary flex items-center justify-center text-primary-foreground shadow-[0_0_15px_rgba(var(--primary),0.3)]">
            <MonitorPlay size={18} />
          </div>
          <span className="font-bold text-xl tracking-tight">ReMCote</span>
        </div>
        
        <div className="flex items-center gap-4">
          <Button variant="ghost" onClick={() => setLocation('/download')} className="text-muted-foreground hover:text-foreground">
            Host Setup
          </Button>
          <Button variant="glass" onClick={() => setLocation('/download')} className="hidden md:flex gap-2">
            <DownloadCloud size={16} />
            Download for Windows
          </Button>
        </div>
      </header>

      {/* Hero Section */}
      <main className="flex-1 flex flex-col items-center justify-center px-4 sm:px-6 z-10 w-full max-w-5xl mx-auto -mt-16">
        <div className="text-center space-y-6 mb-16 max-w-3xl">
          <h1 className="text-5xl sm:text-7xl font-bold tracking-tight text-foreground leading-[1.1]">
            Remote. <span className="text-transparent bg-clip-text bg-gradient-to-r from-primary to-blue-400">Reimagined.</span>
          </h1>
          <p className="text-lg sm:text-xl text-muted-foreground max-w-2xl mx-auto leading-relaxed">
            Ultra-low-latency remote desktop built for professionals. Scrub timelines, grade color, and feel the immediacy of a local workstation from anywhere.
          </p>
        </div>

        {/* Connect Card */}
        <Card className="w-full max-w-md mx-auto border-white/10 bg-black/40 backdrop-blur-xl shadow-2xl">
          <CardHeader className="pb-4">
            <CardTitle className="text-xl font-semibold flex items-center gap-2">
              <MonitorPlay className="w-5 h-5 text-primary" />
              Connect to Workstation
            </CardTitle>
            <CardDescription className="text-muted-foreground">
              Enter the 9-digit Device ID of the remote host.
            </CardDescription>
          </CardHeader>
          <CardContent>
            <form onSubmit={handleSubmit} className="space-y-4">
              <div className="space-y-2">
                <Input 
                  type="text" 
                  inputMode="numeric"
                  placeholder="000 000 000"
                  value={deviceId}
                  onChange={handleDeviceChange}
                  className="text-center text-3xl h-16 tracking-widest font-mono bg-white/5 border-white/10 focus-visible:border-primary/50 transition-all rounded-lg"
                  autoFocus
                />
                {error && (
                  <p className="text-sm text-destructive text-center font-medium">{error}</p>
                )}
              </div>
              <Button 
                type="submit" 
                size="xl" 
                className="w-full text-lg shadow-[0_0_20px_rgba(var(--primary),0.2)] group"
                disabled={deviceId.length < 11 || isChecking} // 11 because of spaces
              >
                {isChecking ? "Verifying..." : "Connect"}
                {!isChecking && <ArrowRight className="w-5 h-5 ml-2 group-hover:translate-x-1 transition-transform" />}
              </Button>
            </form>
          </CardContent>
          <div className="px-6 pb-6 text-center">
            <button 
              onClick={() => setLocation('/download')}
              className="text-sm text-muted-foreground hover:text-primary transition-colors inline-flex items-center gap-1"
            >
              Need to set up a host computer? <ChevronRight size={14} />
            </button>
          </div>
        </Card>
      </main>

      {/* Live Stats Strip */}
      <footer className="w-full border-t border-white/5 bg-black/20 backdrop-blur-md px-6 py-4 flex items-center justify-between text-xs text-muted-foreground z-10 absolute bottom-0">
        <div className="flex items-center gap-6">
          <div className="flex items-center gap-2">
            <div className="w-2 h-2 rounded-full bg-success shadow-[0_0_8px_var(--success)]" />
            Control Plane Online
          </div>
          {stats && (
            <>
              <div className="hidden sm:flex items-center gap-2 border-l border-white/10 pl-6">
                <Server size={14} className="text-primary/70" />
                <span><strong className="text-foreground">{stats.devicesOnline}</strong> Hosts Online</span>
              </div>
              <div className="hidden sm:flex items-center gap-2 border-l border-white/10 pl-6">
                <Activity size={14} className="text-primary/70" />
                <span><strong className="text-foreground">{stats.activeSessions}</strong> Active Sessions</span>
              </div>
            </>
          )}
        </div>
        <div className="font-mono opacity-50">v0.1.0-alpha</div>
      </footer>
    </div>
  );
}
