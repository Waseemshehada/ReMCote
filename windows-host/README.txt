ReMCote Windows Host
====================

1. Run ReMCoteHost.exe
2. Copy the 9-digit Device ID shown in the window
3. Open https://remcote.replit.app on the other PC
4. Enter the Device ID
5. Click ALLOW on this PC

Requirements: Windows 10/11 x64, NVIDIA GPU (RTX recommended)

Logs appear in the terminal if you run from PowerShell,
or in remcote-host.log next to the exe when double-clicked.

Advanced (developers only):
  Override server: set REMCOTE_SIGNALING_URL=wss://your-server/api/ws
  Or create remcote-server.txt next to the exe containing the URL.
