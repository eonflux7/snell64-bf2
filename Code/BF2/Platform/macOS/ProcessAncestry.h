// [CHOSEN, not recovered] macOS port: whether an application started this process, directly or
// through the shells and scripts between them. The Cocoa host asks this about the application
// that is in front when the game window first requests activation (CocoaHost.mm,
// beginActivation): only such a launcher may lose the front to the repeated requests. Public API
// only: the kernel's process table, read with sysctl(kern.proc.pid).
#ifndef PLATFORM_MACOS_PROCESSANCESTRY_H
#define PLATFORM_MACOS_PROCESSANCESTRY_H

#include <cstring>
#include <sys/types.h>
#include <sys/sysctl.h>

namespace dice { namespace hfe { namespace macos {

// The parent of process `pid`, or -1 when the kernel gives no answer: no such process, or a
// sandbox that denies reading the process table. sysctl answers for the processes of every
// user, which the walk below needs: Terminal starts its shells through the root-owned login,
// and proc_pidinfo(PROC_PIDTBSDINFO) refuses that process to an unprivileged caller.
inline pid_t parentPidOf(pid_t pid)
{
    struct kinfo_proc info;
    std::memset(&info, 0, sizeof info);
    size_t size = sizeof info;
    int name[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
    if (sysctl(name, 4, &info, &size, nullptr, 0) != 0 || size < sizeof info)
        return -1;                               // a pid nobody has answers with size 0
    return info.kp_eproc.e_ppid;
}

enum ProcessAncestry
{
    AncestryNo,         // the chain reached launchd without meeting the candidate
    AncestryYes,        // the candidate is the parent or a further ancestor
    AncestryUnknown     // a lookup failed before the chain ended
};

// Whether process `candidate` is `parent` or one of its ancestors. `parent` is this process's
// parent, which getppid() answers without a lookup, so the direct parent is recognised even when
// every lookup fails; parentOf(pid) answers a process's parent, or a negative value when it
// cannot. The chain ends at launchd (pid 1), which adopts a process whose parent has exited: an
// application behind such a gap (a tmux or screen server, a launcher script that has ended, the
// first instance of a client that restarted itself) is not found. launchd itself and the kernel
// (pid 0) are nobody's launcher. A client that restarted itself is told by its first instance
// whether it may take the front (RestartInFront.h); this walk has no part in that.
template <class ParentOf>
ProcessAncestry processAncestry(pid_t candidate, pid_t parent, ParentOf parentOf)
{
    if (candidate <= 1)
        return AncestryNo;
    pid_t pid = parent;
    for (int depth = 0; depth < 64; ++depth)     // real chains are about ten processes deep
    {
        if (pid == candidate)
            return AncestryYes;
        if (pid <= 1)
            return AncestryNo;
        pid = parentOf(pid);
        if (pid < 0)
            return AncestryUnknown;
    }
    return AncestryUnknown;                      // deeper than any real chain, or a cycle
}

} } }
#endif
