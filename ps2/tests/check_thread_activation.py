"""Exercise the real libultra start/stop functions with immediate preemption.

Windows fibers stand in for the EE's higher-priority thread switching. This
checks bookkeeping across a wake that runs the target until it parks again;
it is a host regression test, not PS2 hardware validation.

Run from any directory on Windows with GCC on PATH. Optional arguments are
the known-bad revision followed by one or more revisions expected to pass.
"""
import pathlib
import shutil
import subprocess
import sys


def function(source, name):
    start = source.index("void " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
enum { OS_STATE_STOPPED, OS_STATE_RUNNABLE, OS_STATE_RUNNING };
typedef struct { int started, suspended, ee_id, run_sema; } PS2ThreadExt;
typedef struct { int state, id; PS2ThreadExt ext; } OSThread;
#define PS2_THREAD_EXT(t) (&(t)->ext)
static OSThread target;
static void *caller_fiber, *target_fiber;
static int activations, failures;
static OSThread *ps2_ultra_self(void) { return &target; }
void ps2_panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    exit(3);
}
/* Each wake immediately runs the higher-priority target until it blocks. */
static int StartThread(int id, void *arg) {
    (void)id; (void)arg; SwitchToFiber(target_fiber); return 0;
}
static int ResumeThreadMock(int id) {
    (void)id; SwitchToFiber(target_fiber); return 0;
}
static int SignalSema(int id) {
    (void)id; SwitchToFiber(target_fiber); return 0;
}
static int WaitSema(int id) {
    (void)id; SwitchToFiber(caller_fiber); return 0;
}
static int SuspendThreadMock(int id) { (void)id; return 0; }
#define ResumeThread ResumeThreadMock
#define SuspendThread SuspendThreadMock
__FUNCTIONS__
static VOID WINAPI coroutine(void *arg) {
    (void)arg;
    for (;;) {
        activations++;
        osStopThread(NULL);
    }
}
static void scenario(const char *name, int started, int suspended) {
    memset(&target, 0, sizeof(target));
    target.ext.started = started;
    target.ext.suspended = suspended;
    target.state = OS_STATE_STOPPED;
    activations = 0;
    target_fiber = CreateFiber(65536, coroutine, NULL);
    if (!target_fiber) exit(4);
    for (int wake = 1; wake <= 3; wake++) {
        osStartThread(&target);
        if (activations != wake || target.state != OS_STATE_STOPPED ||
            !target.ext.started || target.ext.suspended) {
            printf("FAIL %s wake=%d: activations=%d state=%d started=%d suspended=%d\n",
                   name, wake, activations, target.state,
                   target.ext.started, target.ext.suspended);
            failures++;
            break;
        }
    }
    if (activations == 3 && target.state == OS_STATE_STOPPED)
        printf("PASS %s: three activations each park and remain resumable\n", name);
    DeleteFiber(target_fiber);
}
int main(void) {
    caller_fiber = ConvertThreadToFiber(NULL);
    if (!caller_fiber) return 4;
    scenario("first start", 0, 0);
    scenario("resume suspended", 1, 1);
    scenario("signal stopped", 1, 0);
    return failures ? 1 : 0;
}
'''


def main():
    repo = pathlib.Path(__file__).resolve().parents[2]
    refs = sys.argv[1:] or ["fd7b74730", "HEAD"]
    if len(refs) < 2:
        raise SystemExit("Supply a known-bad revision and at least one good revision")
    compiler = shutil.which("gcc")
    if not compiler:
        raise SystemExit("A Windows GCC host compiler is required")
    out = repo / "ps2" / "build" / "thread-activation-check"
    out.mkdir(parents=True, exist_ok=True)
    for index, ref in enumerate(refs):
        source = subprocess.check_output(
            ["git", "show", f"{ref}:ps2/src/ultra/thread.c"],
            cwd=repo, text=True, encoding="utf-8")
        functions = "\n\n".join(function(source, f) for f in
                                  ["osStartThread", "osStopThread"])
        c_file = out / f"thread-activation-{index}.c"
        exe = out / f"thread-activation-{index}.exe"
        c_file.write_text(HARNESS.replace("__FUNCTIONS__", functions), encoding="utf-8")
        subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(exe)], check=True)
        print(f"\n{ref}", flush=True)
        result = subprocess.run([str(exe)], timeout=10)
        print(f"exit={result.returncode}", flush=True)
        expected = 1 if index == 0 else 0
        if result.returncode != expected:
            raise SystemExit(f"Unexpected result for {ref}: wanted exit {expected}")
    print("\nExpected regression reproduced; restored baseline passes.")


if __name__ == "__main__":
    main()
