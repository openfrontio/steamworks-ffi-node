/**
 * Entry point for the Linux overlay host process. Not imported by the library:
 * it is exec'd by LinuxOverlayHost with LD_PRELOAD set, which is the entire
 * point of it existing as a separate process. See LinuxOverlayHost for why.
 *
 * Contract:
 *   argv[2]  path to write the handshake JSON to, once the window exists
 *   argv[3]  path to steam-overlay.node, resolved by the parent (may be empty)
 *   stdin    newline-delimited JSON commands: create, show, hide, frame, destroy
 *   stdout   inherited from the parent, so the addon's own logging is not lost
 *
 * The host presents its own empty frames on a timer. That call is the one Steam's
 * interposer sees, and keeping it here means no per-frame traffic crosses the
 * process boundary — in transparent mode there are no pixels to send at all.
 */
import * as fs from "fs";
import * as path from "path";

interface CreateCommand {
  width: number;
  height: number;
  title: string;
  fps: number;
  vsync: boolean;
  electronXid?: number;
}

function loadNativeModule(explicitPath?: string): any {
  // The parent resolves the addon and passes it in, because it knows the package
  // layout and this script may have been extracted out of an asar, leaving its
  // own __dirname pointing at a temp directory. The relative candidates below are
  // the fallback for running straight from a source checkout.
  const candidates = [
    ...(explicitPath ? [explicitPath] : []),
    path.join(__dirname, "..", "..", "native", "build", "Release", "steam-overlay.node"),
    path.join(
      __dirname,
      "..",
      "..",
      "prebuilds",
      `${process.platform}-${process.arch}`,
      "steam-overlay.node",
    ),
  ];
  let lastError: unknown = null;
  for (const candidate of candidates) {
    try {
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      return require(candidate);
    } catch (e) {
      lastError = e;
    }
  }
  throw lastError ?? new Error("steam-overlay.node not found");
}

function main(): void {
  const stampPath = process.argv[2];
  const writeStamp = (payload: Record<string, unknown>) => {
    if (!stampPath) return;
    try {
      // Write-then-rename so the parent, which polls, never reads a partial file.
      const tmp = `${stampPath}.partial`;
      fs.writeFileSync(tmp, JSON.stringify(payload), "utf8");
      fs.renameSync(tmp, stampPath);
    } catch {
      /* the parent will time out and clean up */
    }
  };

  const addonPath = process.argv[3] || undefined;

  let addon: any;
  try {
    addon = loadNativeModule(addonPath);
  } catch (e) {
    writeStamp({ ok: false, error: `native module: ${e}` });
    process.exit(1);
  }

  let window: any = null;
  let presentTimer: NodeJS.Timeout | null = null;

  const destroy = () => {
    if (presentTimer) {
      clearInterval(presentTimer);
      presentTimer = null;
    }
    if (window) {
      try {
        addon.destroyOverlayWindow(window);
      } catch {
        /* shutting down anyway */
      }
      window = null;
    }
  };

  const create = (cmd: CreateCommand) => {
    window = addon.createOverlayWindow({
      width: cmd.width,
      height: cmd.height,
      title: cmd.title,
      fps: cmd.fps,
      vsync: cmd.vsync,
      transparent: true,
    });

    if (!window) {
      writeStamp({ ok: false, error: "createOverlayWindow returned null" });
      process.exit(1);
    }

    if (cmd.electronXid && typeof addon.setElectronWindow === "function") {
      // Input forwarding is X11-to-X11 and needs no channel back to the parent:
      // the addon XSendEvents straight to Electron's window.
      addon.setElectronWindow(window, cmd.electronXid);
    }

    const transparent =
      typeof addon.isOverlayTransparent === "function"
        ? addon.isOverlayTransparent(window) === true
        : false;
    const hookState =
      typeof addon.getOverlayHookState === "function"
        ? addon.getOverlayHookState()
        : "unknown";

    addon.showOverlayWindow(window);

    const interval = Math.max(1, Math.round(1000 / (cmd.fps || 60)));
    presentTimer = setInterval(() => {
      if (!window) return;
      try {
        addon.presentFrame(window);
      } catch {
        /* a failed present must never take the host down */
      }
    }, interval);

    writeStamp({ ok: true, window: String(window), transparent, hookState });
  };

  const handle = (line: string) => {
    let cmd: any;
    try {
      cmd = JSON.parse(line);
    } catch {
      return;
    }
    switch (cmd.op) {
      case "create":
        create(cmd as CreateCommand);
        break;
      case "show":
        if (window) addon.showOverlayWindow(window);
        break;
      case "hide":
        if (window) addon.hideOverlayWindow(window);
        break;
      case "frame":
        if (window) addon.setOverlayFrame(window, cmd.x, cmd.y, cmd.width, cmd.height);
        break;
      case "debug":
        if (typeof addon.setDebugMode === "function") addon.setDebugMode(cmd.enabled === true);
        break;
      case "destroy":
        destroy();
        process.exit(0);
        break;
    }
  };

  let buffer = "";
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (chunk: Buffer | string) => {
    buffer += String(chunk);
    let newline: number;
    while ((newline = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, newline).trim();
      buffer = buffer.slice(newline + 1);
      if (line) {
        try {
          handle(line);
        } catch (e) {
          console.error(`[Overlay Host] command failed: ${e}`);
        }
      }
    }
  });

  // The parent going away must not leave an always-on-top window stranded.
  process.stdin.on("end", () => {
    destroy();
    process.exit(0);
  });
  process.on("SIGTERM", () => {
    destroy();
    process.exit(0);
  });
  process.on("SIGINT", () => {
    destroy();
    process.exit(0);
  });
}

main();
