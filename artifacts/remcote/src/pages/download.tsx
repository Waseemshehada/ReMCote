import { Link } from "wouter";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { ArrowLeft, Cpu, ShieldCheck, Download, Code2 } from "lucide-react";

// Direct link to the stable "latest" GitHub Release installer — updated
// automatically after every successful CI build.
const SETUP_EXE_URL = "https://github.com/Waseemshehada/ReMCote/releases/download/latest/ReMCoteSetup.exe";
const DEV_ZIP_URL = "https://github.com/Waseemshehada/ReMCote/releases/download/latest/ReMCoteHost-Windows.zip";
const SOURCE_ZIP_URL = `${import.meta.env.BASE_URL}downloads/remcote-windows-host.zip`;

const STEPS: Array<[string, string]> = [
  ["Download ReMCote", "Click the button above to download ReMCoteSetup.exe."],
  ["Install", "Run the installer. It sets everything up and launches ReMCote when done."],
  ["Copy your Device ID", "ReMCote opens and shows a 9-digit Device ID — no configuration needed."],
  ["Connect from another computer", "Open remcote.replit.app on the other computer and enter the ID."],
  ["Click ALLOW", "Approve the connection on this PC and you're in."],
];

export default function DownloadPage() {
  return (
    <div className="min-h-[100dvh] flex flex-col bg-background relative overflow-hidden">
      {/* Subtle background glow */}
      <div className="absolute top-0 right-0 w-[600px] h-[600px] bg-primary/5 rounded-full blur-[100px] pointer-events-none" />

      <header className="px-8 py-6 flex items-center justify-between z-10 border-b border-white/5 bg-black/20 backdrop-blur-md">
        <div className="flex items-center gap-4">
          <Link href="/">
            <Button variant="ghost" size="icon" className="text-muted-foreground hover:text-foreground">
              <ArrowLeft size={18} />
            </Button>
          </Link>
          <span className="font-bold text-lg text-foreground">Get ReMCote</span>
        </div>
      </header>

      <main className="flex-1 overflow-y-auto px-4 py-12 z-10">
        <div className="max-w-4xl mx-auto space-y-12">

          {/* Hero */}
          <div className="space-y-4">
            <h1 className="text-4xl font-bold tracking-tight text-foreground">
              ReMCote for Windows
            </h1>
            <p className="text-lg text-muted-foreground max-w-2xl leading-relaxed">
              Install ReMCote on the PC you want to control. It connects automatically — no setup, no configuration.
            </p>
          </div>

          {/* Primary download card */}
          <div className="bg-primary/10 border border-primary/30 rounded-2xl p-8 flex flex-col sm:flex-row gap-6 items-start sm:items-center" data-testid="card-download">
            <div className="flex-1 space-y-3">
              <div className="flex items-center gap-2">
                <Download size={20} className="text-primary" />
                <span className="text-sm font-semibold text-primary uppercase tracking-wider">Installer · Zero configuration</span>
              </div>
              <h2 className="text-2xl font-bold text-foreground">ReMCoteSetup.exe</h2>
              <div className="flex flex-wrap gap-2 text-xs">
                {["Windows 10/11", "x64", "One-click install", "Connects automatically"].map(tag => (
                  <span key={tag} className="px-2 py-0.5 rounded-full bg-white/10 text-muted-foreground border border-white/10">{tag}</span>
                ))}
              </div>
            </div>
            <Button asChild size="lg" className="font-bold text-base shrink-0 px-8 py-6" data-testid="button-download-installer">
              <a href={SETUP_EXE_URL}>
                <Download size={18} className="mr-2" />
                Download ReMCote for Windows
              </a>
            </Button>
          </div>

          {/* Setup steps */}
          <div className="space-y-6">
            <h2 className="text-2xl font-semibold text-foreground border-b border-white/10 pb-2">
              How it works
            </h2>
            <div className="space-y-8 pl-4 border-l-2 border-white/10">
              {STEPS.map(([title, desc], i) => (
                <div className="relative" key={title}>
                  <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">{i + 1}</div>
                  <h3 className="text-lg font-medium text-foreground mb-2">{title}</h3>
                  <p className="text-muted-foreground text-sm">{desc}</p>
                </div>
              ))}
            </div>
          </div>

          {/* Info cards */}
          <div className="grid md:grid-cols-2 gap-6">
            <Card className="bg-black/40 border-white/10 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2 text-primary">
                  <Cpu size={20} /> System Requirements
                </CardTitle>
              </CardHeader>
              <CardContent className="space-y-3 text-sm text-muted-foreground">
                <ul className="space-y-3">
                  {[
                    ["OS", "Windows 10 or Windows 11 (64-bit)"],
                    ["GPU", "NVIDIA RTX series recommended (NVENC hardware encoding)"],
                    ["Network", "Wired ethernet strongly advised for the host machine"],
                  ].map(([label, desc]) => (
                    <li key={label} className="flex items-start gap-2">
                      <div className="w-1.5 h-1.5 rounded-full bg-primary mt-1.5 shrink-0" />
                      <span><strong>{label}:</strong> {desc}</span>
                    </li>
                  ))}
                </ul>
              </CardContent>
            </Card>

            <Card className="bg-black/40 border-white/10 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2 text-foreground">
                  <ShieldCheck size={20} className="text-green-400" /> Open source, verifiable builds
                </CardTitle>
              </CardHeader>
              <CardContent className="space-y-3 text-sm text-muted-foreground leading-relaxed">
                <p>
                  Every release is compiled automatically from the public source by GitHub Actions — you can inspect the build log for any release.
                </p>
                <p>
                  Video and input travel directly peer-to-peer over WebRTC (DTLS/SRTP). The media stream never touches the server.
                </p>
              </CardContent>
            </Card>
          </div>

          {/* Advanced: developers */}
          <details className="group border border-white/10 rounded-xl overflow-hidden">
            <summary className="flex items-center gap-3 px-6 py-4 cursor-pointer select-none bg-black/20 hover:bg-black/40 transition-colors">
              <Code2 size={18} className="text-muted-foreground" />
              <span className="font-medium text-muted-foreground group-open:text-foreground transition-colors">For Developers</span>
              <span className="ml-auto text-xs text-muted-foreground">Advanced</span>
            </summary>
            <div className="px-6 py-5 space-y-4 text-sm text-muted-foreground bg-black/10">
              <p>
                A plain ZIP of the compiled host (no installer) is published alongside every release for diagnostics, and the full C++ source is available for auditing or custom builds. Developers can override the signaling server with the <code className="text-primary/80">REMCOTE_SIGNALING_URL</code> environment variable or a <code className="text-primary/80">remcote-server.txt</code> file next to the exe.
              </p>
              <div className="flex flex-wrap gap-3">
                <Button asChild variant="outline" size="sm" className="font-semibold">
                  <a href={DEV_ZIP_URL}>
                    <Download size={14} className="mr-2" />
                    Host ZIP (no installer)
                  </a>
                </Button>
                <Button asChild variant="outline" size="sm" className="font-semibold">
                  <a href={SOURCE_ZIP_URL} download>
                    <Download size={14} className="mr-2" />
                    Source ZIP
                  </a>
                </Button>
              </div>
            </div>
          </details>

        </div>
      </main>
    </div>
  );
}
