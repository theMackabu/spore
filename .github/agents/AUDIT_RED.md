# Spore — GOAL: red-team the sandbox + the concurrency/dynamic-linking surface it must survive

## Goal

Spore's entire thesis is _confined_ agent execution. Every prior goal proved the
box works on the happy and structured-deny paths. This goal does the opposite:
**seriously try to escape the box**, and harden whatever breaks. It also expands
the attack surface deliberately by adding the three things most likely to open new
holes — **SMP, kernel preemption, and dynamic linking** — and requires the
confinement guarantees to survive all three. The deliverable is an adversarial
test suite that _attempts_ real escapes, a hardened kernel that defeats them, and
the concurrency/dynlink features added _without_ introducing a bypass.

This is the inverse of a feature goal: success is measured by attacks **failing**.
A passing gate means a determined escape attempt returns `-EPERM`/SIGKILL/clean
denial with the boundary intact — proven, not assumed.

## Grounding in the actual tree (verified)

- **The enforcement points to attack are concrete** (cell.c): `cell_syscall_allowed(nr)`
  (syscall allowlist), `cell_egress_allowed(proto,ip,port)` (packet egress, called
  at the UDP/TCP send seams), `caps_subset(requested,parent)` (monotonic
  restriction, gated by `CAP_ENFORCE`), `memory_page_cap` (RSS cap), `cpu_budget`,
  and path-policy (`path_policy_denied`, fs-view containment in path resolution).
  These are the surfaces the suite must probe.
- **The kernel is explicitly lock-free today**: cell.c notes _"these tables
  intentionally have no locks until a later SMP/preemptive goal."_ So adding SMP or
  preemption **breaks the invariant every prior goal relied on** — every shared
  structure (cell table, PMM refcounts, VMA lists, fd tables, socket/TCB state,
  futex queues, capability sets) becomes a potential race, and **a race in an
  enforcement check is a security hole** (TOCTOU on a cap check, a refcount race
  freeing a live frame, etc.). This is precisely why it belongs in the red-team
  goal: the new concurrency must not open a confinement bypass.
- **Dynamic linking is half-seeded**: `elf_find_interp_aarch64` already _parses_
  `PT_INTERP` but nothing acts on it (static musl only today). Adding an ld.so path
  is a classic escape surface — a confined domain loading attacker-controlled
  shared objects, or escaping its fs view via the loader's search path.
- **Entropy/RNG, the TLS path, AF_UNIX, pipes, the C compiler (tinycc), curl** all
  now exist — a much bigger surface than when confinement was first built. A
  sandbox with a _C compiler in it_ is a very interesting thing to try to escape.

## Threat model (what "escape" means)

A confined domain (spawned under a restrictive `Capability=` manifest — limited
syscall set, fs subtree, memory/CPU budget, egress policy) must NOT be able to:

1. Execute a syscall outside its allowlist (incl. via indirect paths).
2. Read/write/traverse outside its fs view (incl. via `..`, symlinks, `/proc`,
   `dirfd` tricks, rename races, FIFO/socket nodes it can see).
3. Send packets outside its egress policy (incl. connect-then-sendto, raw tuple
   spoofing, DNS-as-side-channel, TLS SNI to a forbidden host).
4. Exceed its memory or CPU budget (incl. via fork-bomb, mmap tricks, shared pages,
   refcount races).
5. Acquire capabilities its parent lacks (escalation via spawn, exec, clone, or a
   subset-check race).
6. Corrupt or read another domain's state (cross-domain memory via CoW bug, shared
   buffer, AF_UNIX/pipe fd confusion, TLB staleness).
7. Use a _new_ feature (SMP, preemption, dynlink, compiler, TLS) to do any of 1–6.

## Part 1 — the adversarial suite (build the attacks first)

A `/demos/escape/` (or `/libexec/spore-test/escape/`) family of binaries, each a
focused escape attempt, run under tight manifests by the harness. Build these
_before_ hardening, so they currently document what holds and what doesn't:

- **syscall-allowlist probes**: call every denied syscall directly; attempt via
  unusual numbers, via `clone`/thread to a different path, via the compiler
  (compile+run a program that issues a forbidden syscall).
- **fs-view escapes**: `../../..` traversal, absolute paths outside the view,
  `openat` with a forbidden `dirfd`, symlink-out (if symlinks exist), `/proc` self
  inspection, rename/rename-race to move a target into view, FIFO/AF_UNIX node
  outside the subtree.
- **egress escapes**: connect-allowed-then-sendto-forbidden, sendto with a forged
  sockaddr after connect, DNS query to exfiltrate to a forbidden resolver, TLS to a
  forbidden host via SNI, raw port/CIDR-boundary probes, TX-counter audited.
- **budget escapes**: fork-bomb against the domain cap, mmap-past-cap variants,
  CoW-shared-page accounting tricks, CPU-budget evasion via tight loops across
  yield points.
- **escalation**: spawn a child requesting broader caps; exec a setuid-ish path;
  clone into a less-confined sibling; race the `caps_subset` check.
- **cross-domain**: try to read another domain's pages (CoW bug probe), confuse fd
  tables across fork, abuse a shared pipe/AF_UNIX endpoint, exploit stale TLB after
  an address-space switch.
  Gate `rt-1-suite`: the suite builds and runs under the harness; each attempt is
  logged as HELD or BROKEN with the exact mechanism; a written `ESCAPE_REPORT.md`
  enumerates every probe and its result. (BROKEN results here are expected and feed
  Part 2 — this gate is "the attacks exist and are honestly scored," not "all pass.")

## Part 2 — harden every break

For each BROKEN probe from Part 1, fix the enforcement so it becomes HELD, at the
right layer (the existing `cell_*` checks), with the fix proven by the probe now
failing. No probe is "fixed" by being removed or weakened.
Gate `rt-2-harden`: every Part-1 probe is HELD; `ESCAPE_REPORT.md` shows all
attempts denied with mechanism + enforcement point; the suite is wired into
`run-tests` as a permanent regression so future goals can't silently reopen a hole.

## Part 3 — SMP (and the races it threatens)

Bring up secondary cores via PSCI; schedule threads across cores. **The hard part
is that lock-free-by-run-to-completion no longer holds** — introduce the minimal
correct locking for every shared structure that an enforcement decision touches,
and prove no TOCTOU/race opens a hole. Re-run the _entire_ escape suite under SMP
with attacks running concurrently on multiple cores (the configuration most likely
to surface an enforcement race).
Gate `rt-3-smp`: N cores bring up and schedule; shared structures are correctly
synchronized (document the locking discipline); the full escape suite passes with
attackers running concurrently on all cores; no cross-domain leak, no cap-check
race, no refcount UAF; all prior regressions green.

## Part 4 — kernel preemption (and the reentrancy it threatens)

Make the kernel preemptible inside syscalls (timer can reschedule in EL1), removing
the run-to-completion shield. Every enforcement check must be safe against being
interrupted mid-decision. Re-run the escape suite with preemption forcing maximal
interleaving (e.g. preempt between a cap check and the action it guards) to hunt
TOCTOU.
Gate `rt-4-preempt`: kernel is preemptible with correct synchronization; the escape
suite passes under aggressive preemption (incl. deliberately preempting between
check and use); no enforcement TOCTOU; all prior regressions green.

## Part 5 — dynamic linking (and the loader holes it threatens)

Act on the already-parsed `PT_INTERP`: support dynamically-linked musl binaries via
an in-image `ld-musl`. **The loader is an escape surface**: a confined domain must
not load shared objects outside its fs view, must not escape via `LD_LIBRARY_PATH`/
`LD_PRELOAD`/rpath, and the loader's own file access must be subject to the domain's
fs-view and syscall policy. Add escape probes specifically for the loader and
harden.
Gate `rt-5-dynlink`: dynamic musl binaries run; a confined domain's loader cannot
read/load objects outside its fs view; `LD_*`-based escapes are denied; the
loader-specific escape probes are HELD; all prior regressions green.

## Final gate

Final tag: `redteam`. The full escape suite (Parts 1–5, incl. loader and
concurrency probes) passes — every attempt denied with proof — under SMP +
preemption + dynamic linking simultaneously enabled. `ESCAPE_REPORT.md` is the
artifact: every probe, its mechanism, its enforcement point, HELD. The suite is a
permanent `run-tests` regression.

## Validation session (illustrative)

```text
/ $ myc start escape-suite.service
escape-suite: running 47 probes under tight manifests...
  syscall-allowlist:  18/18 denied
  fs-view:            11/11 denied
  egress:              9/9 denied (TX counters clean)
  budget:              4/4 enforced
  escalation:          3/3 rejected
  cross-domain:        2/2 denied
escape-suite: 47/47 HELD (SMP=4 cores, preempt=on, dynlink=on)
/ $ cat /var/log/escape-report.txt | tail -3
[HELD] ld-preload-escape: LD_PRELOAD outside fs-view -> open denied (fs-view, loader)
[HELD] captoctou-smp: cap check raced on core 3 -> serialized, denied (caps lock)
[HELD] cow-crossdomain: read sibling page -> fault, killed (CoW refcount)
/ $ echo shell alive
shell alive
```

Required regression set: everything prior (userland, ext2/vfs, threads/futex,
ICMP/UDP/TCP/DNS/TLS, mycelium, the structured egress suite) PLUS the full escape
suite, all green — now under SMP + preemption + dynamic linking.

## Hard rules

- **A probe is fixed only by being denied, never by removal or weakening.**
- **Every new feature (SMP, preempt, dynlink) re-runs the full escape suite** — a
  feature that opens a hole isn't done until the hole is closed.
- **Enforcement races are security bugs**: TOCTOU on any `cell_*` check, refcount
  UAF, or cross-domain leak is a gate failure, not a perf footnote.
- **The escape suite becomes a permanent `run-tests` regression** so no future goal
  silently reopens a hole.
- **Document the locking discipline** introduced for SMP/preemption — which lock
  guards which structure, and why no enforcement decision can be raced.
- Honesty bar carried forward: denials proven at the real seam (TX counter for
  egress, actual `-EPERM`/SIGKILL for the rest), not assumed.

## Out of scope

- Formal verification / a proof of non-escape (this is empirical red-teaming).
- Side-channel / timing / Spectre-class microarchitectural attacks (note as a
  future hardening goal; out of scope for functional confinement).
- Hardware-level attacks, fault injection.
- Performance tuning of the new locking (correctness first; a perf goal can follow).
