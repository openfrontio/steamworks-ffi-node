import { spawn, ChildProcess } from "child_process";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { SteamLogger } from "./SteamLogger";

/**
 * Runs the Linux overlay window in a separate, preloaded process.
 *
 * Steam's Linux overlay is an LD_PRELOAD interposer: `gameoverlayrenderer.so`
 * is placed at the *front* of the global symbol scope at exec, so the game's
 * `glXSwapBuffers` resolves to Steam's copy rather than the driver's, and Steam
 * draws from inside the process on the next buffer swap.
 *
 * That mechanism cannot be reproduced after startup. A library loaded later
 * with `dlopen` is appended to the global scope, not inserted at the front, so
 * the symbol still binds to the driver. Nor can the interposer be called
 * directly: Steam's `glXSwapBuffers` forwards to the real one via
 * `dlsym(RTLD_NEXT, "glXSwapBuffers")`, which resolves from its own fixed
 * position in the link map. Loaded last, that lookup finds nothing, and the
 * code returns without presenting — silently, so the window simply stops
 * updating. Load *order*, not load presence, and nothing in-process can fix it.
 *
 * Preloading the Electron process itself is not an option either: `LD_PRELOAD`
 * is inherited by Chromium's zygotes, which are forked during startup before
 * any JavaScript runs, and the GPU process does not survive it. Deleting the
 * variable from `process.env` cannot help — by then the zygote has already been
 * forked carrying the library as mapped memory.
 *
 * So the overlay window moves to a process we *can* control from before exec: a
 * plain Node process (Electron re-invoked with `ELECTRON_RUN_AS_NODE`) that
 * loads nothing but this addon, with `LD_PRELOAD` set properly. The loader does
 * the interposition as designed, Chromium never sees the library, and the
 * overlay window is an ordinary X11 window that floats over Electron's.
 *
 * The environment needed is smaller than it looks: `LD_PRELOAD` alone is
 * necessary and sufficient for Steam to attach and respond to Shift+Tab. The
 * host does not need to be a Steam-launched child, and does not need
 * `SteamAppId`. `SteamGameId` is forwarded when present only because it
 * silences a "BasePanel does not know about the game ID" warning in Steam's own
 * log; it is not load-bearing.
 */

/** Known Steam installation roots, relative to $HOME. Steam's path is not fixed. */
const STEAM_ROOTS = [
  ".local/share/Steam",
  ".steam/steam",
  ".steam/root",
  ".steam/debian-installation",
  ".var/app/com.valvesoftware.Steam/data/Steam",
];

export interface LinuxOverlayHostOptions {
  width: number;
  height: number;
  title: string;
  fps: number;
  vsync: boolean;
  /** XID of the Electron window, for input forwarding. */
  electronXid?: number;
}

interface Handshake {
  ok: boolean;
  /** Whether the child's window got a depth-32 ARGB visual and a compositor. */
  transparent?: boolean;
  /** "active" | "mapped-not-interposing" | "not-present" */
  hookState?: string;
  error?: string;
}

/**
 * Steam's overlay library, for this process's architecture only.
 *
 * Steam itself sets LD_PRELOAD to *both* architectures and lets the loader
 * reject the wrong one, which is harmless but prints
 *
 *   ERROR: ld.so: object '.../ubuntu12_32/gameoverlayrenderer.so' from
 *   LD_PRELOAD cannot be preloaded (wrong ELF class: ELFCLASS32): ignored.
 *
 * three times on a 64-bit binary. Passing that through to the host would put
 * the same noise in the consuming application's own logs, so pick the single
 * matching entry instead.
 *
 * Prefer Steam's own paths when they were inherited, since that is where Steam
 * is actually installed; fall back to searching, because the install location
 * is not fixed. Note the "64" is in the directory name, not the filename.
 */
export function findOverlayLibrary(): string | undefined {
  const dir = process.arch === "ia32" ? "ubuntu12_32" : "ubuntu12_64";

  const inherited = process.env["LD_PRELOAD"];
  if (inherited) {
    // Steam's value has a leading empty entry; the filters below drop it.
    // Readability is checked rather than assumed: a stale inherited value would
    // otherwise go straight into the child's LD_PRELOAD, where the loader
    // silently ignores a bad path and the overlay simply never appears.
    const match = inherited
      .split(":")
      .find(
        (entry) =>
          entry.includes("gameoverlayrenderer.so") &&
          entry.includes(dir) &&
          isReadable(entry),
      );
    if (match) return match;
  }

  const home = os.homedir();
  if (!home) return undefined;

  for (const root of STEAM_ROOTS) {
    const candidate = path.join(home, root, dir, "gameoverlayrenderer.so");
    if (isReadable(candidate)) return candidate;
  }
  return undefined;
}

function isReadable(p: string): boolean {
  try {
    fs.accessSync(p, fs.constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

/** Sleep without yielding to the event loop, so a handshake can be awaited synchronously. */
function sleepSync(ms: number): void {
  try {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
  } catch {
    const until = Date.now() + ms;
    while (Date.now() < until) {
      /* SharedArrayBuffer unavailable; spin */
    }
  }
}

/**
 * Rewrite a path that points inside an asar archive to its unpacked twin.
 *
 * Native modules are always unpacked -- they cannot be loaded from inside an
 * archive -- so a `.node` resolved relative to this file needs this mapping in
 * a packaged app.
 */
function unpacked(p: string): string {
  return p.replace(`.asar${path.sep}`, `.asar.unpacked${path.sep}`);
}

/**
 * The host entry point, as a path a plain Node process can execute.
 *
 * In a packaged Electron app this file lives inside app.asar. The Electron main
 * process can read that, but a child spawned with ELECTRON_RUN_AS_NODE cannot be
 * relied upon to require from it. Copying the script out to a temporary file is
 * cheaper and far more predictable than requiring every consumer to add an
 * asarUnpack rule and remember why.
 */
function resolveHostEntry(): string | undefined {
  const entry = path.join(__dirname, "linuxOverlayHostMain.js");
  if (!entry.includes(`.asar${path.sep}`)) {
    return fs.existsSync(entry) ? entry : undefined;
  }
  try {
    const source = fs.readFileSync(entry, "utf8");
    const extracted = path.join(os.tmpdir(), `steam-overlay-host-${process.pid}.js`);
    fs.writeFileSync(extracted, source, "utf8");
    return extracted;
  } catch {
    return undefined;
  }
}

/**
 * The native addon, resolved by the parent and handed to the host.
 *
 * The host cannot always work this out for itself: once its script has been
 * extracted out of an asar, its own __dirname no longer points anywhere near
 * the package. Passing it explicitly keeps that knowledge here, where the
 * package layout is actually known.
 */
function resolveAddonPath(): string | undefined {
  const candidates = [
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
  for (const candidate of candidates) {
    const real = unpacked(candidate);
    if (fs.existsSync(real)) return real;
  }
  return undefined;
}

export class LinuxOverlayHost {
  private child: ChildProcess | null = null;
  private handshake: Handshake | null = null;
  private stampPath: string | null = null;

  /** Whether the host process is up and owns a window. */
  get running(): boolean {
    return this.child !== null && this.child.exitCode === null;
  }

  /**
   * Whether the child is still alive, asked of the kernel rather than of Node.
   *
   * Everything Node offers here depends on the event loop, which the handshake
   * deliberately blocks, so none of it works:
   *
   *   - `child.exitCode` is set from the 'exit' event, delivered on the loop.
   *   - `process.kill(pid, 0)` succeeds on a **zombie**, and a dead child stays
   *     a zombie precisely because reaping it needs SIGCHLD handled on the loop.
   *     Measured: it reported ALIVE for a full 3s deadline against a child that
   *     had already exited, with /proc showing state Z throughout.
   *
   * /proc is the only source that answers synchronously and truthfully. Linux
   * only, but so is this entire file.
   */
  private childAlive(): boolean {
    const pid = this.child?.pid;
    if (!pid) return false;
    try {
      const stat = fs.readFileSync(`/proc/${pid}/stat`, "utf8");
      // Field 3 is the state character. Skip past the last ')' rather than
      // splitting from the start: field 2 is the executable name, which may
      // itself contain spaces and parentheses.
      const state = stat.slice(stat.lastIndexOf(")") + 2).split(" ")[0];
      return state !== "Z" && state !== "X";
    } catch {
      // No /proc entry: already reaped, or never started.
      return false;
    }
  }

  /** Whether the host's window is genuinely transparent. False until the handshake lands. */
  get transparent(): boolean {
    return this.handshake?.transparent === true;
  }

  /** What the host reported about Steam's interposer: "active" means the overlay will draw. */
  get hookState(): string {
    return this.handshake?.hookState ?? "unknown";
  }

  /**
   * Spawn the host and block until it reports its window, or fail.
   *
   * Blocking is deliberate: `addElectronSteamOverlay()` is synchronous and
   * returns whether the overlay attached, and callers rely on `isTransparent()`
   * being meaningful immediately afterwards. The wait is bounded and happens
   * once, at attach time.
   */
  start(options: LinuxOverlayHostOptions, timeoutMs = 3000): boolean {
    const library = findOverlayLibrary();
    if (!library) {
      SteamLogger.debug(
        "[Steam Overlay] gameoverlayrenderer.so not found — no Steam installation " +
          "in any known location. The overlay cannot be hosted.",
      );
      return false;
    }

    const entry = resolveHostEntry();
    if (!entry) {
      SteamLogger.debug("[Steam Overlay] Host entry point could not be resolved");
      return false;
    }

    const stampPath = path.join(
      os.tmpdir(),
      `steam-overlay-host-${process.pid}-${Date.now()}.json`,
    );
    this.stampPath = stampPath;

    const env: NodeJS.ProcessEnv = {
      ...process.env,
      ELECTRON_RUN_AS_NODE: "1",
      LD_PRELOAD: library,
    };
    // Ozone/Wayland hints mean nothing to a plain Node process and only invite
    // confusion if the host is ever inspected.
    delete env["ELECTRON_OZONE_PLATFORM_HINT"];

    try {
      this.child = spawn(process.execPath, [entry, stampPath, resolveAddonPath() ?? ""], {
        env,
        stdio: ["pipe", "inherit", "inherit"],
        detached: false,
      });
    } catch (e) {
      SteamLogger.debug(`[Steam Overlay] Could not spawn overlay host: ${e}`);
      return false;
    }

    this.child.on("exit", (code, signal) => {
      SteamLogger.debug(
        `[Steam Overlay] Overlay host exited (code=${code} signal=${signal})`,
      );
      this.child = null;
    });

    this.send("create", options);

    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      // Give up the moment the child dies rather than waiting out the deadline:
      // the failure path is what users hit, and a five second frozen window
      // followed by a silent fallback is the worst of both.
      if (!this.childAlive()) break;
      const result = this.readStamp();
      if (result) {
        this.handshake = result;
        break;
      }
      sleepSync(10);
    }

    if (!this.handshake?.ok) {
      SteamLogger.debug(
        `[Steam Overlay] Overlay host did not come up: ${this.handshake?.error ?? "timed out"}`,
      );
      this.stop();
      return false;
    }

    SteamLogger.debug(
      `[Steam Overlay] Overlay host ready (transparent=${this.handshake.transparent}, ` +
        `hook=${this.handshake.hookState})`,
    );
    if (this.handshake.hookState !== "active") {
      SteamLogger.debug(
        "[Steam Overlay] WARNING: the host started but Steam's interposer is not in " +
          "its call path, so the overlay will not draw. This should not happen when " +
          "LD_PRELOAD is set at exec; please report it.",
      );
    }
    return true;
  }

  private readStamp(): Handshake | null {
    if (!this.stampPath) return null;
    try {
      const raw = fs.readFileSync(this.stampPath, "utf8");
      if (!raw.trim()) return null;
      return JSON.parse(raw) as Handshake;
    } catch {
      return null; // not written yet, or a partial write
    }
  }

  /** Fire-and-forget command. Nothing after `create` needs a reply. */
  send(op: string, payload?: object): void {
    if (!this.child?.stdin || this.child.stdin.destroyed) return;
    try {
      this.child.stdin.write(JSON.stringify({ op, ...payload }) + "\n");
    } catch {
      /* the host is going away; a dropped command must not take the app down */
    }
  }

  stop(): void {
    if (this.child) {
      this.send("destroy");
      // Give it a moment to unmap cleanly, then make sure it is gone: a stranded
      // always-on-top window would be far worse than a missing overlay.
      const child = this.child;
      setTimeout(() => {
        try {
          if (child.exitCode === null) child.kill("SIGKILL");
        } catch {
          /* already gone */
        }
      }, 500);
      this.child = null;
    }
    if (this.stampPath) {
      try {
        fs.unlinkSync(this.stampPath);
      } catch {
        /* best effort */
      }
      this.stampPath = null;
    }
    this.handshake = null;
  }
}

/** Sentinel window handle. The real window lives in the host process. */
const HOSTED_WINDOW = "linux-overlay-host";

/**
 * An object shaped like the native module, backed by a host process.
 *
 * SteamOverlay drives the overlay entirely through `this.nativeModule`, so
 * substituting this keeps the hosting concern out of the platform-agnostic
 * code: every call site stays as it is.
 *
 * `presentFrame` is deliberately absent. SteamOverlay treats a missing
 * `presentFrame` as "the backend presents on its own", which is exactly true
 * here -- the host runs its own present loop, so no per-frame traffic crosses
 * the process boundary.
 *
 * `renderFrame` is a no-op: mirroring would mean shipping a full frame buffer
 * per frame to another process, and transparent mode makes it unnecessary. The
 * host path is only used when transparency was requested and granted.
 */
export function createHostedNativeModule(fallback: any): any {
  const host = new LinuxOverlayHost();

  return {
    createOverlayWindow(options: any): any {
      const started = host.start({
        width: options.width,
        height: options.height,
        title: options.title,
        fps: options.fps,
        vsync: options.vsync,
        electronXid: options.electronXid,
      });
      return started ? HOSTED_WINDOW : null;
    },
    showOverlayWindow(): void {
      host.send("show");
    },
    hideOverlayWindow(): void {
      host.send("hide");
    },
    setOverlayFrame(_window: any, x: number, y: number, width: number, height: number): void {
      host.send("frame", { x, y, width, height });
    },
    renderFrame(): void {
      /* mirroring is not supported in the host; see the note above */
    },
    isOverlayTransparent(): boolean {
      return host.transparent;
    },
    getOverlayHookState(): string {
      return host.hookState;
    },
    destroyOverlayWindow(): void {
      host.stop();
    },
    setDebugMode(enabled: boolean): void {
      host.send("debug", { enabled });
      try {
        fallback?.setDebugMode?.(enabled);
      } catch {
        /* debug logging must never be fatal */
      }
    },
    setSteamGameAtomOnWindow(xid: number, appId: number): void {
      // X11 only, no GL: this works from the parent and needs no host round trip.
      try {
        fallback?.setSteamGameAtomOnWindow?.(xid, appId);
      } catch {
        /* best effort */
      }
    },
    setElectronWindow(): void {
      /* the host received the Electron XID with its create command */
    },
    shouldSuppressNextBlur(): boolean {
      // The state this reads lives in the host process. Nothing in this library
      // calls it; returning false keeps the shape without inventing an answer.
      return false;
    },
  };
}
