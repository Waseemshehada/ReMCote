import { Link } from "wouter";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { ArrowLeft, Terminal, Cpu, ShieldCheck, Download, AlertTriangle } from "lucide-react";

const ZIP_URL = `${import.meta.env.BASE_URL}downloads/remcote-windows-host.zip`;
// Prebuilt Windows binaries produced by CI from the same source.
const CI_BUILDS_URL = "https://github.com/Waseemshehada/ReMCote/actions/workflows/build-windows-host.yml";
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
          
          <div className="space-y-4">
            <h1 className="text-4xl font-bold tracking-tight text-foreground">
              Get and Run the Host
            </h1>
            <p className="text-lg text-muted-foreground max-w-2xl leading-relaxed">
              ReMCote is open-source. Download the prebuilt Windows Host — compiled automatically from the public source by GitHub Actions — or build it from source yourself and verify every line.
            </p>
          </div>

          <div className="grid md:grid-cols-2 gap-6">
            <Card className="bg-black/40 border-white/10 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2 text-primary">
                  <Cpu size={20} /> System Requirements
                </CardTitle>
              </CardHeader>
              <CardContent className="space-y-4 text-sm text-muted-foreground">
                <ul className="space-y-3">
                  <li className="flex items-start gap-2">
                    <div className="w-1.5 h-1.5 rounded-full bg-primary mt-1.5 shrink-0" />
                    <span><strong>OS:</strong> Windows 10 or Windows 11 (64-bit)</span>
                  </li>
                  <li className="flex items-start gap-2">
                    <div className="w-1.5 h-1.5 rounded-full bg-primary mt-1.5 shrink-0" />
                    <span><strong>GPU:</strong> NVIDIA RTX series highly recommended (for NVENC ultra-low latency hardware encoding)</span>
                  </li>
                  <li className="flex items-start gap-2">
                    <div className="w-1.5 h-1.5 rounded-full bg-primary mt-1.5 shrink-0" />
                    <span><strong>Network:</strong> Wired ethernet connection strongly advised for host machine</span>
                  </li>
                  <li className="flex items-start gap-2">
                    <div className="w-1.5 h-1.5 rounded-full bg-primary mt-1.5 shrink-0" />
                    <span><strong>Build Tools (source builds only):</strong> Visual Studio 2022 Community (C++ Desktop Development)</span>
                  </li>
                </ul>
              </CardContent>
            </Card>

            <Card className="bg-black/40 border-white/10 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2 text-foreground">
                  <ShieldCheck size={20} className="text-success" /> Open source, verifiable builds
                </CardTitle>
              </CardHeader>
              <CardContent className="space-y-4 text-sm text-muted-foreground leading-relaxed">
                <p>
                  Remote desktop software requires deep system-level access to capture screens and inject input. The prebuilt package is compiled by GitHub Actions directly from the public source — or build it yourself and verify every line.
                </p>
                <p>
                  The media stream never touches our servers. Your video and input are transmitted directly peer-to-peer using WebRTC, secured with DTLS/SRTP end-to-end encryption.
                </p>
              </CardContent>
            </Card>
          </div>

          <div className="space-y-6">
            <h2 className="text-2xl font-semibold text-foreground border-b border-white/10 pb-2">
              Setup Instructions
            </h2>
            
            <div className="space-y-8 pl-4 border-l-2 border-white/10">
              
              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">1</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Get the Host</h3>
                <p className="text-muted-foreground text-sm mb-3">
                  <strong className="text-foreground">Recommended:</strong> download the prebuilt <code className="text-primary/80">ReMCoteHost-Windows</code> package — open the latest green build and grab it from the <em>Artifacts</em> section. It contains <code className="text-primary/80">ReMCoteHost.exe</code> and all required runtime DLLs. No compiler needed.
                </p>
                <div className="flex flex-wrap gap-3 mb-3">
                  <Button asChild className="font-semibold">
                    <a href={CI_BUILDS_URL} target="_blank" rel="noreferrer">
                      <Download size={16} className="mr-2" />
                      Prebuilt Windows Host (GitHub Actions)
                    </a>
                  </Button>
                  <Button asChild variant="outline" className="font-semibold">
                    <a href={ZIP_URL} download>
                      <Download size={16} className="mr-2" />
                      Source ZIP (build it yourself)
                    </a>
                  </Button>
                </div>
              </div>

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">2</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Build the Host <span className="text-muted-foreground font-normal text-sm">(source ZIP only — skip if you downloaded the prebuilt package)</span></h3>
                <p className="text-muted-foreground text-sm mb-3">Extract the ZIP, then run the build script from PowerShell. It checks prerequisites, downloads dependencies via vcpkg, and compiles Release x64.</p>
                <div className="bg-black/60 border border-white/5 rounded-md p-4 font-mono text-sm text-primary-foreground/80 flex items-center gap-3">
                  <Terminal size={16} className="text-muted-foreground" />
                  <code>cd remcote-windows-host\windows-host; .\build-windows.ps1</code>
                </div>
              </div>

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">3</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Point the Host at this server</h3>
                <p className="text-muted-foreground text-sm mb-3">The Host has no built-in server URL. Set the signaling URL for this ReMCote server before starting it:</p>
                <div className="bg-black/60 border border-white/5 rounded-md p-4 font-mono text-sm text-primary-foreground/80 flex items-center gap-3 overflow-x-auto">
                  <Terminal size={16} className="text-muted-foreground shrink-0" />
                  <code>{`$env:REMCOTE_SIGNALING_URL = "${SIGNALING_URL}"`}</code>
                </div>
              </div>

              <div className="relative">
                <div className="absolute -left-[25px] top-0 w-6 h-6 rounded-full bg-primary/20 border border-primary flex items-center justify-center text-xs font-bold text-primary">4</div>
                <h3 className="text-lg font-medium text-foreground mb-2">Run & Connect</h3>
                <p className="text-muted-foreground text-sm mb-3">Start the host from the same PowerShell window. It runs a preflight check, registers, and shows a 9-digit Device ID. Enter that ID on this website from another computer — the host will show an ALLOW prompt.</p>
                <div className="bg-black/60 border border-white/5 rounded-md p-4 font-mono text-sm text-primary-foreground/80 flex items-center gap-3">
                  <Terminal size={16} className="text-muted-foreground" />
                  <code>.\ReMCoteHost.exe</code>
                </div>
                <p className="text-muted-foreground text-xs mt-2">(Prebuilt package: the exe is at the top of the extracted folder. Source build: it's at <code className="text-primary/80">.\dist\ReMCoteHost.exe</code>.)</p>
              </div>

            </div>
          </div>
          
          <div className="bg-primary/10 border border-primary/20 rounded-xl p-6 flex gap-4 items-start">
            <AlertTriangle className="text-primary shrink-0 mt-1" />
            <div>
              <h4 className="font-semibold text-primary mb-1">Documentation & diagnostics</h4>
              <p className="text-sm text-muted-foreground">
                The ZIP includes full documentation in <code className="text-primary/80">windows-host/docs/</code> — build details, architecture, wire protocol, and testing. On startup the host runs a preflight check (NVENC, Direct3D, DXGI capture, signaling URL) and logs every session state transition. Run it from PowerShell to see logs in the terminal, or find them in <code className="text-primary/80">remcote-host.log</code> next to the exe.
              </p>
            </div>
          </div>

        </div>
      </main>
    </div>
  );
}
