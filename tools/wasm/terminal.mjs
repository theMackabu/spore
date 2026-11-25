import { Terminal } from "./xterm/xterm.mjs";
import { FitAddon } from "./fit/addon-fit.mjs";

export function start(site, release) {
  const screen = document.getElementById("terminal");
  const terminal = new Terminal({ cursorBlink: true, theme: { background: "#101311" } });
  const fit = new FitAddon();
  terminal.loadAddon(fit);
  terminal.open(screen);
  fit.fit();
  terminal.focus();

  let worker,
    active,
    nextBoot,
    stats,
    updating = false,
    checking = false;
  async function checkRelease() {
    if (!release || checking || updating) return;
    checking = true;
    try {
      const res = await fetch(new URL("release.json", site), { cache: "no-store" });
      if (!res.ok) return;
      const next = await res.json();
      if (next.format !== 1 || !/^[a-f0-9]{64}$/.test(next.id) || next.id === release.id) return;
      if (!worker) {
        location.reload();
        return;
      }
      updating = true;
      terminal.writeln("\r\nServer update available. Saving disk and restarting…");
      worker.postMessage({ stop: true });
    } catch (error) {
      console.debug("[spore:update]", error);
    } finally {
      checking = false;
    }
  }
  if (release) {
    setInterval(checkRelease, 5000);
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) checkRelease();
    });
  }
  const options = new URLSearchParams(location.search);
  function confirmReset(disk) {
    return (
      disk &&
      !options.has("snapshot") &&
      confirm("Erase all browser-saved changes to this guest disk and boot the base image?")
    );
  }
  window.spore = {
    stats: () => stats && { ...stats },
    resetDisk() {
      if (!updating && active && confirmReset(active.disk)) boot(active.app, active.disk, true);
    },
  };
  terminal.onData((input) => worker?.postMessage({ input }));
  terminal.onResize(({ rows, cols }) => worker?.postMessage({ size: { rows, cols } }));
  new ResizeObserver(() => fit.fit()).observe(screen);
  function finish(message) {
    worker?.terminate();
    worker = null;
    if (updating) {
      if (message === "Stopped" || message.startsWith("Exited with ")) {
        location.reload();
        return;
      }
      terminal.writeln("\r\nUpdate interrupted: " + message);
      return;
    }
    if (nextBoot) {
      const [app, disk, resetDisk] = nextBoot;
      nextBoot = null;
      boot(app, disk, resetDisk);
      return;
    }
    terminal.writeln("\r\n" + message);
  }
  function boot(app, disk, resetDisk = false) {
    if (worker) {
      nextBoot = [app, disk, resetDisk];
      worker.postMessage({ stop: true });
      return;
    }
    terminal.reset();
    active = { app, disk };
    terminal.focus();
    stats = undefined;
    worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
    worker.onerror = (event) => finish(event.message || "Browser worker failed to start");
    worker.onmessage = ({ data }) => {
      if (data.reload) {
        location.reload();
        return;
      }
      if (data.bytes) terminal.write(data.bytes);
      else if (data.stats) stats = data.stats;
      else if (data.notice) terminal.writeln(data.notice);
      else {
        if (stats) console.info("[spore]", data.status, stats);
        else console.info("[spore]", data.status);
        finish(data.status);
      }
    };
    worker.postMessage({
      app: typeof app === "string" ? new URL(app, site).href : app,
      disk: typeof disk === "string" ? new URL(disk, site).href : disk,
      release: disk === "./root.ext2" && !app ? release : null,
      site,
      args: options.getAll("arg"),
      persist: !options.has("snapshot"),
      resetDisk,
      online: options.has("net"),
      initialSize: { rows: terminal.rows, cols: terminal.cols },
      cpuCount: Number(options.get("cpus") ?? 1),
      memoryMiB: Number(options.get("memory") ?? 1024),
    });
  }
  const app = options.get("app");
  const disk = options.get("disk") ?? (app ? null : "./root.ext2");
  let resetDisk = false;
  if (options.has("reset")) {
    const url = new URL(location.href);
    url.searchParams.delete("reset");
    history.replaceState(null, "", url);
    resetDisk = confirmReset(disk);
  }
  boot(app, disk, resetDisk);
  screen.addEventListener("dragover", (event) => event.preventDefault());
  screen.addEventListener("drop", async (event) => {
    event.preventDefault();
    let app, disk;
    for (const file of [...event.dataTransfer.files]) {
      const magic = await file.slice(0, 8).text();
      if (magic.startsWith("\x7fELF") || magic === "SPOREAP2") app = file;
      else disk = file;
    }
    if (app || disk) boot(app, disk);
  });
}
