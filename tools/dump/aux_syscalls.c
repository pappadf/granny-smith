// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// aux_syscalls.c
// A/UX 1.x..3.x syscall numbers.  Pulled from sys/syscall.h conventions
// in the A/UX SDK: A/UX inherits the SVR2 system-call numbering with a
// few Mac-specific extensions (toolbox callbacks, MMU operations).  The
// table is gappy — A/UX reserved unused slots — but the well-known
// POSIX-y numbers all line up with classic Unix.

#include "aux_syscalls.h"

#include <stddef.h>

static const struct {
    uint32_t num;
    const char *name;
} g_syscalls[] = {
    {0,   "indir"      }, // indirect — actual call number is on the stack
    {1,   "exit"       },
    {2,   "fork"       },
    {3,   "read"       },
    {4,   "write"      },
    {5,   "open"       },
    {6,   "close"      },
    {7,   "wait"       },
    {8,   "creat"      },
    {9,   "link"       },
    {10,  "unlink"     },
    {11,  "execv"      },
    {12,  "chdir"      },
    {13,  "time"       },
    {14,  "mknod"      },
    {15,  "chmod"      },
    {16,  "chown"      },
    {17,  "break"      }, // sbrk-style
    {18,  "stat"       },
    {19,  "lseek"      },
    {20,  "getpid"     },
    {21,  "mount"      },
    {22,  "umount"     },
    {23,  "setuid"     },
    {24,  "getuid"     },
    {25,  "stime"      },
    {26,  "ptrace"     },
    {27,  "alarm"      },
    {28,  "fstat"      },
    {29,  "pause"      },
    {30,  "utime"      },
    {31,  "stty"       },
    {32,  "gtty"       },
    {33,  "access"     },
    {34,  "nice"       },
    {35,  "ftime"      },
    {36,  "sync"       },
    {37,  "kill"       },
    {38,  "switch"     }, // SVR2 multi-environment switch (A/UX extension)
    {41,  "dup"        },
    {42,  "pipe"       },
    {43,  "times"      },
    {44,  "prof"       },
    {46,  "setgid"     },
    {47,  "getgid"     },
    {48,  "signal"     },
    {51,  "acct"       },
    {52,  "phys"       },
    {53,  "lock"       },
    {54,  "ioctl"      },
    {55,  "reboot"     },
    {56,  "mpx"        },
    {59,  "execve"     },
    {60,  "umask"      },
    {61,  "chroot"     },
    {62,  "fcntl"      },
    {63,  "ulimit"     },
    {64,  "advfs"      },
    {65,  "unadvfs"    },
    // SVR2 IPC family
    {86,  "sigsys"     },
    {87,  "lstat"      },
    {88,  "readlink"   },
    {89,  "symlink"    },
    {99,  "syssgi"     }, // SGI-style "system info"; A/UX cribbed the number
    {103, "sigprocmask"},
    {104, "sigsuspend" },
    {105, "sigaltstack"},
    {106, "sigaction"  },
    {107, "sigpending" },
    {115, "msgsys"     }, // SVR2 IPC: msgctl / msgget / msgsnd / msgrcv
    {116, "shmsys"     }, // SVR2 IPC: shmat / shmctl / shmdt / shmget
    {117, "semsys"     }, // SVR2 IPC: semop / semget / semctl
    // A/UX-specific extensions (Mac-side calls).
    {123, "getitimer"  },
    {124, "setitimer"  },
    {127, "select"     },
};
static const size_t g_syscalls_count = sizeof(g_syscalls) / sizeof(g_syscalls[0]);

const char *aux_syscall_name(uint32_t num) {
    for (size_t i = 0; i < g_syscalls_count; i++) {
        if (g_syscalls[i].num == num)
            return g_syscalls[i].name;
    }
    return NULL;
}
