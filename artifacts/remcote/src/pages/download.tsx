import { Link } from "wouter";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { ArrowLeft, Terminal, Cpu, ShieldCheck, Download, AlertTriangle, Code2 } from "lucide-react";

// Direct link to the stable "latest" GitHub Release asset — updated automatically
// after every successful CI build. Never links to Actions artifacts.
const RELEASE_ZIP_URL = "https://github.com/Waseemshehada/ReMCote/releases/download/latest/ReMCoteHost-Windows.zip";
const SOURCE_ZIP_URL = `${import.meta.env.BASE_URL}downloads/remcote-windows-host.zip`;
// The exact signaling endpoint for THIS deployment of ReMCote.
const SIGNALING_URL = `wss://${window.location.host}/api/ws`;

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
          <span className="font-bold text-lg text-foreground">ReMCote Host Setup</span>
        </div>
      </header>

      <main className="flex-1 overflow-y-auto px-4 py-12 z-10">
        <div className="max-w-4xl mx-auto space-y-12">

          {/* Hero */}
          <div className="space-y-4">
            <h1 className="text-4xl font-bold tracking-tight text-foreground">
              Get and Run the Host
            </h1>
            <p className="text-lg text-muted-foreground max-w-2xl leading-relaxed">
              Download the prebuilt Windows Host, point it at this server, and start sharing your desktop in minutes.
            </p>
          </div>

          {/* Primary download card */}
          <div className="bg-primary/10 border border-primary/30 rounded-2xl p-8 flex flex-col sm:flex-row gap-6 items-start sm:items-center">
            <div className="flex-1 space-y-3">
              <div className="flex items-center gap-2">
                <Download size={20} className="text-primary" />
                <span className="text-sm font-semibold text-primary uppercase tracking-wider">Prebuilt · No compiler required</span>
              </div>
              <h2 className="text-2xl font-bold text-foreground">ReMCote Host for Windows</h2>
              <div className="flex flex-wrap gap-2 text-xs">
                {["Windows 10/11", "x64", "Prebuilt Host", "No compiler required"].map(tag => (
                  <span key={tag} className="px-2 py-0.5 rounded-full bg-white/10 text-muted-foreground border border-white/10">{tag}</span>
                ))}
              </div>
            </div>
            <Button asChild size="lg" className="font-bold text-base shrink-0 px-8 py-6">
              <a href={RELEASE_ZIP_URL}>
                <Download size={18} className="mr-2" />
                Download ReMCote Host for Windows
              </a>
            </Button>
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

          {/* Setup steps */}
          <div className="space-y-6">
            <h2 className="text-2xl font-semibold text-foreground border-b border-white/10 pb-2">
              Setup Instructions
            </h2>

            <div className="space-y-8 pl-4 border-l-2 border-white/10">

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">1</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Download &amp; extract</h3>
                <p className="text-muted-foreground text-sm">
                  Click the button above to download <code className="text-primary/80">ReMCoteHost-Windows.zip</code>, then extract it anywhere on your Windows PC.
                </p>
              </div>

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">2</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Point the Host at this server</h3>
                <p className="text-muted-foreground text-sm mb-3">
                  Open PowerShell inside the extracted folder and set the signaling URL for this ReMCote server:
                </p>
                <div className="bg-black/60 border border-white/5 rounded-md p-4 font-mono text-sm text-primary-foreground/80 flex items-center gap-3 overflow-x-auto">
                  <Terminal size={16} className="text-muted-foreground shrink-0" />
                  <code>{`$env:REMCOTE_SIGNALING_URL = "${SIGNALING_URL}"`}</code>
                </div>
              </div>

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">3</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Run &amp; Connect</h3>
                <p className="text-muted-foreground text-sm mb-3">
                  Start the host from the same PowerShell window. It runs a preflight check, registers with the server, and shows a 9-digit Device ID. Enter that ID on this website from another computer — the host will show an ALLOW prompt.
                </p>
                <div className="bg-black/60 border border-white/5 rounded-md p-4 font-mono text-sm text-primary-foreground/80 flex items-center gap-3">
                  <Terminal size={16} className="text-muted-foreground" />
                  <code>.\ReMCoteHost.exe</code>
                </div>
              </div>

            </div>
          </div>

          {/* Advanced: build from source */}
          <details className="group border border-white/10 rounded-xl overflow-hidden">
            <summary className="flex items-center gap-3 px-6 py-4 cursor-pointer select-none bg-black/20 hover:bg-black/40 transition-colors">
              <Code2 size={18} className="text-muted-foreground" />
              <span className="font-medium text-muted-foreground group-open:text-foreground transition-colors">Build from Source</span>
              <span className="ml-auto text-xs text-muted-foreground">Advanced</span>
            </summary>
            <div className="px-6 py-5 space-y-4 text-sm text-muted-foreground bg-black/10">
              <p>
                Download the C++ source, CMake files, and automated build script. Requires Visual Studio 2022 with the C++ Desktop Development workload, git, and an internet connection for vcpkg.
              </p>
              <Button asChild variant="outline" size="sm" className="font-semibold">
                <a href={SOURCE_ZIP_URL} download>
                  <Download size={14} className="mr-2" />
                  Download Source ZIP
                </a>
              </Button>
              <div className="bg-black/40 border border-white/5 rounded-md p-4 font-mono text-xs">
                <div className="text-muted-foreground mb-1"># Extract the ZIP, then:</div>
                <div>cd remcote-windows-host\windows-host; .\build-windows.ps1</div>
              </div>
              <p className="text-xs">
                Full build docs in <code className="text-primary/80">windows-host/docs/</code>. On startup the host logs every preflight check and session transition — run from PowerShell to see them, or find <code className="text-primary/80">remcote-host.log</code> next to the exe.
              </p>
            </div>
          </details>

          {/* Diagnostics note */}
          <div className="bg-primary/10 border border-primary/20 rounded-xl p-6 flex gap-4 items-start">
            <AlertTriangle className="text-primary shrink-0 mt-1" />
            <div>
              <h4 className="font-semibold text-primary mb-1">Diagnostics</h4>
              <p className="text-sm text-muted-foreground">
                On startup the host runs a preflight check (NVENC, Direct3D, DXGI capture, signaling URL) and logs every session state transition. Run it from PowerShell to see logs in the terminal, or find them in <code className="text-primary/80">remcote-host.log</code> next to the exe.
              </p>
            </div>
          </div>

        </div>
      </main>
    </div>
  );
}
