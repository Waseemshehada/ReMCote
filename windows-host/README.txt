ReMCote Desktop for Windows
===========================

Install and run the same program on both computers.

1. On the host PC, copy the 9-digit Device ID.
2. On the viewer PC, click CONNECT TO ANOTHER DEVICE.
3. Enter the host's Device ID and click CONNECT.
4. Click ALLOW on the host PC.

No browser is required. The host PC needs an NVIDIA GPU for NVENC.
A viewer-only PC needs Windows 10/11 x64.

Logs appear in the terminal if you run from PowerShell,
or in remcote-host.log next to the exe when double-clicked.

Advanced (developers only):
  Override server: set REMCOTE_SIGNALING_URL=wss://your-server/api/ws
  Or create remcote-server.txt next to the exe containing the URL.
