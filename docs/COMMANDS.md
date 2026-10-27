# MiniOS command list

Generated from `scripts/commands.txt`. The live status is also shown by `help <category>` and `cmdinfo <name>` inside MiniOS.

| Level | Meaning | Count |
|---|---|---|
| L1 | real, changes real kernel state | 51 |
| L2 | real, built on existing subsystems | 219 |
| L3 | SIMULATED, never touches real hardware | 30 |
| | **Total** | **300** |

Implemented now: **300** of 300 - every registered command has a handler.

## Shell (001-025)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 001 | `help` | List commands / usage | L1 | SH | implemented |
| 002 | `clear` | Clear screen | L1 | DEV | implemented |
| 003 | `echo` | Print text (supports > redirect) | L1 | SH | implemented |
| 004 | `history` | Show command history | L1 | SH | implemented |
| 005 | `exit` | End shell session | L1 | SH | implemented |
| 006 | `alias` | Define alias | L2 | SH | implemented |
| 007 | `unalias` | Remove alias | L2 | SH | implemented |
| 008 | `set` | Set shell variable | L1 | SH | implemented |
| 009 | `unset` | Remove variable | L2 | SH | implemented |
| 010 | `env` | List variables | L2 | SH | implemented |
| 011 | `export` | Mark variable exported | L2 | SH | implemented |
| 012 | `source` | Run commands from file | L2 | SH+FS | implemented |
| 013 | `type` | Show what a name resolves to | L2 | SH | implemented |
| 014 | `which` | Locate command | L2 | SH | implemented |
| 015 | `man` | Show command manual entry | L2 | SH | implemented |
| 016 | `apropos` | Search command descriptions | L2 | SH | implemented |
| 017 | `cmdcount` | Count registered commands | L2 | SH | implemented |
| 018 | `cmdinfo` | Show registry entry details | L2 | SH | implemented |
| 019 | `sleep` | Delay N milliseconds (timer-based) | L1 | SYS | implemented |
| 020 | `repeat` | Run a command N times | L2 | SH | implemented |
| 021 | `true` | Return success | L1 | SH | implemented |
| 022 | `false` | Return failure | L1 | SH | implemented |
| 023 | `test` | Evaluate condition | L2 | SH | implemented |
| 024 | `prompt` | Change prompt string | L2 | SH | implemented |
| 025 | `banner` | Show startup banner | L2 | DEV | implemented |

## File (026-060)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 026 | `touch` | Create empty file | L1 | FS | implemented |
| 027 | `cat` | Print file | L1 | FS | implemented |
| 028 | `write` | Write text to file | L1 | FS | implemented |
| 029 | `append` | Append text to file | L1 | FS | implemented |
| 030 | `rm` | Delete file | L1 | FS | implemented |
| 031 | `cp` | Copy file | L1 | FS | implemented |
| 032 | `mv` | Move/rename file | L1 | FS | implemented |
| 033 | `rename` | Rename in place | L2 | FS | implemented |
| 034 | `stat` | Show inode info | L1 | FS | implemented |
| 035 | `file` | Guess type (text/binary/empty) | L2 | FS | implemented |
| 036 | `head` | First N lines | L2 | FS | implemented |
| 037 | `tail` | Last N lines | L2 | FS | implemented |
| 038 | `more` | Page through file | L2 | FS+DEV | implemented |
| 039 | `truncate` | Set file size | L2 | FS | implemented |
| 040 | `ln` | Create link (stored as copy) | L3 | FS | implemented |
| 041 | `shred` | Overwrite then delete | L2 | FS | implemented |
| 042 | `trash` | Move file to /tmp/.trash | L2 | FS | implemented |
| 043 | `undelete` | Restore from trash | L2 | FS | implemented |
| 044 | `emptytrash` | Purge trash | L2 | FS | implemented |
| 045 | `exists` | Test path exists | L2 | FS | implemented |
| 046 | `newer` | Compare modified times | L2 | FS | implemented |
| 047 | `diff` | Line diff of two files | L2 | FS+LIB | implemented |
| 048 | `cmp` | Byte compare | L2 | FS | implemented |
| 049 | `touchn` | Create N numbered files | L2 | FS | implemented |
| 050 | `fill` | Fill file with pattern | L2 | FS | implemented |
| 051 | `fdlist` | List open descriptors | L2 | FS | implemented |
| 052 | `fdinfo` | Show one descriptor | L2 | FS | implemented |
| 053 | `readfile` | sys_read demo with offset/length | L1 | FS | implemented |
| 054 | `filesize` | Print size in bytes | L2 | FS | implemented |
| 055 | `lock` | Set advisory lock flag | L2 | FS | implemented |
| 056 | `unlock` | Clear lock flag | L2 | FS | implemented |
| 057 | `backup` | Copy to name.bak | L2 | FS | implemented |
| 058 | `restore` | Restore from .bak | L2 | FS | implemented |
| 059 | `swapfiles` | Swap two files' contents | L2 | FS | implemented |
| 060 | `mkfile` | Create file of given size | L2 | FS | implemented |

## Directory (061-080)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 061 | `pwd` | Print working directory | L1 | FS | implemented |
| 062 | `ls` | List directory | L1 | FS | implemented |
| 063 | `cd` | Change directory | L1 | FS | implemented |
| 064 | `mkdir` | Create directory | L1 | FS | implemented |
| 065 | `rmdir` | Remove empty directory | L1 | FS | implemented |
| 066 | `tree` | Print directory tree | L1 | FS | implemented |
| 067 | `find` | Search by name | L1 | FS | implemented |
| 068 | `pushd` | Push dir and cd | L2 | FS+SH | implemented |
| 069 | `popd` | Pop dir and cd | L2 | FS+SH | implemented |
| 070 | `dirs` | Show dir stack | L2 | SH | implemented |
| 071 | `home` | cd to home | L2 | FS | implemented |
| 072 | `up` | cd to parent | L2 | FS | implemented |
| 073 | `dircount` | Count entries | L2 | FS | implemented |
| 074 | `lsdirs` | List only directories | L2 | FS | implemented |
| 075 | `lsfiles` | List only files | L2 | FS | implemented |
| 076 | `lsr` | Recursive listing | L2 | FS | implemented |
| 077 | `mkpath` | Create nested path | L2 | FS | implemented |
| 078 | `rmtree` | Recursive delete | L2 | FS | implemented |
| 079 | `cpdir` | Recursive copy | L2 | FS | implemented |
| 080 | `isdir` | Test path is directory | L2 | FS | implemented |

## Process (081-110)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 081 | `ps` | List processes | L1 | PROC | implemented |
| 082 | `spawn` | Create process (kernel task) | L1 | PROC | implemented |
| 083 | `kill` | Terminate process | L1 | PROC | implemented |
| 084 | `wait` | Wait for process exit | L1 | PROC | implemented |
| 085 | `top` | Live process view (tick-refresh) | L2 | PROC | implemented |
| 086 | `pstree` | Parent/child tree | L2 | PROC | implemented |
| 087 | `pid` | Print current PID | L2 | PROC | implemented |
| 088 | `ppid` | Print parent PID | L2 | PROC | implemented |
| 089 | `nice` | Spawn with priority | L2 | PROC | implemented |
| 090 | `renice` | Change priority | L2 | PROC | implemented |
| 091 | `priority` | Show priority | L2 | PROC | implemented |
| 092 | `suspend` | READY/RUNNING -> WAITING | L2 | PROC | implemented |
| 093 | `resume` | WAITING -> READY | L2 | PROC | implemented |
| 094 | `sched` | Show scheduler config | L2 | PROC | implemented |
| 095 | `quantum` | Show time slice | L2 | PROC | implemented |
| 096 | `setquantum` | Set time slice | L2 | PROC | implemented |
| 097 | `yield` | Voluntarily give up CPU | L2 | PROC | implemented |
| 098 | `jobs` | List shell-spawned processes | L2 | PROC+SH | implemented |
| 099 | `bg` | Background job (flag only) | L3 | SH | implemented |
| 100 | `fg` | Foreground job (flag only) | L3 | SH | implemented |
| 101 | `pinfo` | Detailed PCB dump | L2 | PROC | implemented |
| 102 | `pkill` | Kill by name | L2 | PROC | implemented |
| 103 | `killall` | Kill all by name | L2 | PROC | implemented |
| 104 | `pgrep` | Find PIDs by name | L2 | PROC | implemented |
| 105 | `runqueue` | Show READY queue order | L2 | PROC | implemented |
| 106 | `ctxstat` | Context-switch counters | L2 | PROC | implemented |
| 107 | `proccount` | Count by state | L2 | PROC | implemented |
| 108 | `exitcode` | Last exit status | L2 | PROC | implemented |
| 109 | `procdemo` | Spawn demo workloads | L2 | PROC | implemented |
| 110 | `schedtrace` | Log scheduling decisions | L2 | PROC+LOG | implemented |

## Memory (111-125)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 111 | `mem` | Memory summary | L1 | MEM | implemented |
| 112 | `free` | Free memory report | L1 | MEM | implemented |
| 113 | `meminfo` | Detailed statistics | L2 | MEM | implemented |
| 114 | `memmap` | Multiboot memory map | L2 | MEM | implemented |
| 115 | `heap` | Heap layout/blocks | L2 | MEM | implemented |
| 116 | `alloc` | Allocate N bytes | L2 | MEM | implemented |
| 117 | `dealloc` | Free an allocation | L2 | MEM | implemented |
| 118 | `memstat` | Alloc/free counters | L2 | MEM | implemented |
| 119 | `memtest` | Heap stress test | L2 | MEM | implemented |
| 120 | `memfrag` | Fragmentation estimate | L2 | MEM | implemented |
| 121 | `blocks` | List allocated blocks | L2 | MEM | implemented |
| 122 | `memdump` | Hex dump of address range | L2 | MEM | implemented |
| 123 | `memfill` | Fill allocated block | L2 | MEM | implemented |
| 124 | `pages` | Physical page-frame map | L2 | MEM | implemented |
| 125 | `memlimit` | Per-process limit (accounting only) | L3 | MEM+PROC | implemented |

## User (126-145)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 126 | `whoami` | Current user | L1 | SEC | implemented |
| 127 | `users` | List users | L1 | SEC | implemented |
| 128 | `login` | Log in | L1 | SEC | implemented |
| 129 | `logout` | Log out | L1 | SEC | implemented |
| 130 | `su` | Switch user | L2 | SEC | implemented |
| 131 | `sudo` | Run one command as root | L2 | SEC | implemented |
| 132 | `useradd` | Add user | L2 | SEC | implemented |
| 133 | `userdel` | Delete user | L2 | SEC | implemented |
| 134 | `passwd` | Change password | L2 | SEC | implemented |
| 135 | `id` | UID/GID info | L2 | SEC | implemented |
| 136 | `groups` | Show groups | L2 | SEC | implemented |
| 137 | `who` | Logged-in sessions | L2 | SEC | implemented |
| 138 | `last` | Login history (from log) | L2 | SEC+LOG | implemented |
| 139 | `usermod` | Modify user | L2 | SEC | implemented |
| 140 | `userinfo` | Show user record | L2 | SEC | implemented |
| 141 | `lockuser` | Disable account | L2 | SEC | implemented |
| 142 | `unlockuser` | Enable account | L2 | SEC | implemented |
| 143 | `homedir` | Print home directory | L2 | SEC | implemented |
| 144 | `sessions` | Session table | L2 | SEC | implemented |
| 145 | `motd` | Show message of the day | L2 | SEC+FS | implemented |

## Security (146-160)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 146 | `chmod` | Change mode bits | L1 | SEC+FS | implemented |
| 147 | `chown` | Change owner | L1 | SEC+FS | implemented |
| 148 | `chgrp` | Change group | L2 | SEC+FS | implemented |
| 149 | `umask` | Default creation mask | L2 | SEC | implemented |
| 150 | `access` | Check r/w/x for user | L2 | SEC | implemented |
| 151 | `perms` | Show permission string | L2 | SEC | implemented |
| 152 | `audit` | Show security events | L2 | SEC+LOG | implemented |
| 153 | `secstatus` | Security summary | L2 | SEC | implemented |
| 154 | `hashpw` | Show hash of input (salted) | L2 | SEC | implemented |
| 155 | `checkpw` | Verify password | L2 | SEC | implemented |
| 156 | `readonly` | Set read-only | L2 | SEC+FS | implemented |
| 157 | `writable` | Clear read-only | L2 | SEC+FS | implemented |
| 158 | `protect` | Protect from deletion | L2 | SEC+FS | implemented |
| 159 | `unprotect` | Remove protection | L2 | SEC+FS | implemented |
| 160 | `whoowns` | Show owner | L2 | SEC+FS | implemented |

## Disk (161-185)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 161 | `df` | Space usage per filesystem | L1 | FS | implemented |
| 162 | `du` | Space used by path | L1 | FS | implemented |
| 163 | `mount` | Mount (table only) | L3 | BLK | implemented |
| 164 | `umount` | Unmount (table only) | L3 | BLK | implemented |
| 165 | `lsblk` | List block devices | L2 | BLK | implemented |
| 166 | `fdisk` | Partition table view | L3 | BLK | implemented |
| 167 | `mkfs` | Format RAM-disk with MiniFS | L2 | BLK+FS | implemented |
| 168 | `fsck` | Check MiniFS consistency | L2 | FS | implemented |
| 169 | `sync` | Explain that nothing is cached (use savefs) | L2 | BLK | implemented |
| 170 | `diskinfo` | RAM-disk geometry | L2 | BLK | implemented |
| 171 | `ramdisk` | RAM-disk size/status | L2 | BLK | implemented |
| 172 | `blkread` | Read block N | L2 | BLK | implemented |
| 173 | `blkwrite` | Write block N | L2 | BLK | implemented |
| 174 | `blkdump` | Hex dump of block | L2 | BLK | implemented |
| 175 | `inodes` | Inode usage | L2 | FS | implemented |
| 176 | `freeblocks` | Free-block count | L2 | FS | implemented |
| 177 | `fsstat` | Filesystem statistics | L2 | FS | implemented |
| 178 | `fsdump` | Dump superblock/layout | L2 | FS | implemented |
| 179 | `savefs` | Save MiniFS image to the RAM-disk (root; not ATA) | L2 | BLK | implemented |
| 180 | `loadfs` | Restore MiniFS from the RAM-disk image (root) | L2 | BLK | implemented |
| 181 | `defrag` | Defragmentation | L3 | BLK | implemented |
| 182 | `quota` | Per-user quota (display only) | L3 | FS | implemented |
| 183 | `diskbench` | Timing benchmark of RAM-disk sectors (root) | L2 | BLK | implemented |
| 184 | `smart` | Fake health report | L3 | BLK | implemented |
| 185 | `badblocks` | Read/write-test RAM-disk sectors (root) | L2 | BLK | implemented |

## System (186-210)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 186 | `version` | MiniOS version | L1 | SYS | implemented |
| 187 | `sysinfo` | System summary | L1 | SYS | implemented |
| 188 | `uname` | OS/arch name | L2 | SYS | implemented |
| 189 | `uptime` | Time since boot (timer ticks) | L1 | SYS | implemented |
| 190 | `date` | Date (CMOS RTC) | L1 | SYS | implemented |
| 191 | `time` | Time (CMOS RTC) | L1 | SYS | implemented |
| 192 | `hostname` | Show/set hostname | L2 | SYS | implemented |
| 193 | `cpuinfo` | CPU vendor/features | L2 | CPU | implemented |
| 194 | `cpuid` | Raw CPUID leaf | L2 | CPU | implemented |
| 195 | `ticks` | Raw timer ticks | L2 | DEV | implemented |
| 196 | `timerinfo` | PIT frequency/config | L2 | DEV | implemented |
| 197 | `irqinfo` | IRQ counters | L2 | CPU | implemented |
| 198 | `idtinfo` | IDT entries | L2 | CPU | implemented |
| 199 | `gdtinfo` | GDT entries | L2 | CPU | implemented |
| 200 | `kernelinfo` | Kernel size/layout | L2 | SYS | implemented |
| 201 | `bootinfo` | Multiboot info | L2 | SYS | implemented |
| 202 | `vgainfo` | Text-mode info | L2 | DEV | implemented |
| 203 | `kbdinfo` | Keyboard state | L2 | DEV | implemented |
| 204 | `devices` | List registered devices | L2 | DEV | implemented |
| 205 | `reboot` | Restart (keyboard-controller reset) | L1 | SYS | implemented |
| 206 | `shutdown` | Power off (QEMU ACPI port) | L1 | SYS | implemented |
| 207 | `halt` | Stop CPU | L2 | SYS | implemented |
| 208 | `arch` | Print architecture | L2 | SYS | implemented |
| 209 | `osname` | Print OS name | L2 | SYS | implemented |
| 210 | `about` | Project credits/info | L2 | SYS | implemented |

## Text (211-240)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 211 | `wc` | Count lines/words/bytes | L1 | LIB+FS | implemented |
| 212 | `sort` | Sort lines | L2 | LIB | implemented |
| 213 | `uniq` | Remove adjacent duplicates | L2 | LIB | implemented |
| 214 | `grep` | Search pattern in file | L1 | LIB+FS | implemented |
| 215 | `cut` | Cut fields | L2 | LIB | implemented |
| 216 | `tr` | Translate characters | L2 | LIB | implemented |
| 217 | `rev` | Reverse each line | L2 | LIB | implemented |
| 218 | `nl` | Number lines | L2 | LIB | implemented |
| 219 | `tac` | Reverse line order | L2 | LIB | implemented |
| 220 | `fold` | Wrap lines | L2 | LIB | implemented |
| 221 | `upper` | To uppercase | L2 | LIB | implemented |
| 222 | `lower` | To lowercase | L2 | LIB | implemented |
| 223 | `capitalize` | Capitalize words | L2 | LIB | implemented |
| 224 | `sum` | Add numbers | L2 | LIB | implemented |
| 225 | `min` | Smallest number | L2 | LIB | implemented |
| 226 | `max` | Largest number | L2 | LIB | implemented |
| 227 | `avg` | Average | L2 | LIB | implemented |
| 228 | `join` | Join lines with separator | L2 | LIB | implemented |
| 229 | `split` | Split by delimiter | L2 | LIB | implemented |
| 230 | `paste` | Merge two files side by side | L2 | LIB+FS | implemented |
| 231 | `strlen` | String length | L2 | LIB | implemented |
| 232 | `repeatstr` | Repeat string N times | L2 | LIB | implemented |
| 233 | `trim` | Trim whitespace | L2 | LIB | implemented |
| 234 | `replace` | Replace substring | L2 | LIB | implemented |
| 235 | `contains` | Substring test | L2 | LIB | implemented |
| 236 | `startswith` | Prefix test | L2 | LIB | implemented |
| 237 | `endswith` | Suffix test | L2 | LIB | implemented |
| 238 | `hexdump` | Hex view of file | L2 | LIB+FS | implemented |
| 239 | `ascii` | ASCII table | L2 | LIB | implemented |
| 240 | `wordfreq` | Word frequency | L2 | LIB | implemented |

## Network (241-260)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 241 | `ping` | Simulated reply from loopback | L3 | NET | implemented |
| 242 | `ifconfig` | Show virtual interface | L3 | NET | implemented |
| 243 | `ip` | Virtual address info | L3 | NET | implemented |
| 244 | `netstat` | Virtual socket table | L3 | NET | implemented |
| 245 | `traceroute` | Simulated hops | L3 | NET | implemented |
| 246 | `nslookup` | Lookup in a static table | L3 | NET | implemented |
| 247 | `dig` | Same, verbose | L3 | NET | implemented |
| 248 | `host` | Static host table | L3 | NET | implemented |
| 249 | `arp` | Virtual ARP cache | L3 | NET | implemented |
| 250 | `route` | Virtual route table | L3 | NET | implemented |
| 251 | `curl` | Fetch from virtual local store | L3 | NET | implemented |
| 252 | `wget` | Same, saves to file | L3 | NET | implemented |
| 253 | `telnet` | Simulated session | L3 | NET | implemented |
| 254 | `ssh` | Simulated session | L3 | NET | implemented |
| 255 | `ftp` | Simulated transfer | L3 | NET | implemented |
| 256 | `nc` | Loopback message demo | L3 | NET | implemented |
| 257 | `whois` | Static record | L3 | NET | implemented |
| 258 | `netinfo` | Explains what is simulated | L3 | NET | implemented |
| 259 | `loopback` | In-memory message queue between tasks | L2 | NET+PROC | implemented |
| 260 | `netsim` | Toggle simulation settings | L2 | NET | implemented |

## Debug (261-280)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 261 | `logs` | Show kernel log | L1 | LOG | implemented |
| 262 | `dmesg` | Boot messages | L2 | LOG | implemented |
| 263 | `loglevel` | Set minimum level | L2 | LOG | implemented |
| 264 | `logclear` | Clear log buffer | L2 | LOG | implemented |
| 265 | `logsave` | Save log to /var/log/system.log | L2 | LOG+FS | implemented |
| 266 | `logtail` | Last N log lines | L2 | LOG | implemented |
| 267 | `logfind` | Search log | L2 | LOG | implemented |
| 268 | `logstats` | Counts per level | L2 | LOG | implemented |
| 269 | `panic` | Trigger test kernel panic | L2 | SYS | implemented |
| 270 | `regs` | Dump CPU registers | L2 | CPU | implemented |
| 271 | `stack` | Dump stack words | L2 | CPU | implemented |
| 272 | `peek` | Read memory address | L2 | MEM | implemented |
| 273 | `poke` | Write memory address (guarded) | L2 | MEM | implemented |
| 274 | `trace` | Toggle syscall trace log | L3 | LOG | implemented |
| 275 | `strace` | Per-process trace (partial view) | L3 | LOG | implemented |
| 276 | `errno` | Explain error code | L2 | SYS | implemented |
| 277 | `errlist` | List all error codes | L2 | SYS | implemented |
| 278 | `selftest` | Run in-kernel tests | L2 | SYS | implemented |
| 279 | `diag` | Subsystem health summary | L2 | SYS | implemented |
| 280 | `breakpoint` | Software breakpoint (int3) | L2 | CPU | implemented |

## Other (281-300)

| # | Command | Description | Level | Subsystem | Status |
|---|---|---|---|---|---|
| 281 | `calc` | Integer expression evaluator | L1 | LIB | implemented |
| 282 | `cal` | Monthly calendar | L2 | SYS | implemented |
| 283 | `random` | Pseudo-random number (LCG) | L2 | LIB | implemented |
| 284 | `base` | Convert number bases | L2 | LIB | implemented |
| 285 | `color` | Set VGA text colors | L1 | DEV | implemented |
| 286 | `beep` | PC speaker tone | L2 | DEV | implemented |
| 287 | `clock` | Live clock (tick-refresh) | L2 | SYS | implemented |
| 288 | `timer` | Countdown | L2 | SYS | implemented |
| 289 | `stopwatch` | Start/stop timing | L2 | SYS | implemented |
| 290 | `note` | Append quick note to file | L2 | FS | implemented |
| 291 | `factor` | Prime factors | L2 | LIB | implemented |
| 292 | `prime` | Primality test | L2 | LIB | implemented |
| 293 | `fib` | Fibonacci | L2 | LIB | implemented |
| 294 | `gcd` | Greatest common divisor | L2 | LIB | implemented |
| 295 | `seq` | Number sequence | L2 | LIB | implemented |
| 296 | `yes` | Repeat text (Ctrl-C to stop) | L2 | SH | implemented |
| 297 | `dice` | Random dice roll | L2 | LIB | implemented |
| 298 | `cowsay` | ASCII speech bubble | L2 | LIB | implemented |
| 299 | `demo` | Run guided presentation script | L2 | SH | implemented |
| 300 | `credits` | Team/project credits | L2 | SYS | implemented |
