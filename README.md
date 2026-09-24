linkd
=====

A network interface management daemon: an `ifupdown` replacement with
pluggable dataplane backends. It owns `/etc/network/ports` and
`/etc/network/interfaces`, and applies that configuration to the kernel over
raw netlink.

`linkd` is a multicall binary. It dispatches on `basename(argv[0])`, and the
package ships `ifup`, `ifdown`, `ifquery`, `ifreload` and `linkctl` as symlinks
to it. `linkctl` is the exception: it is a thin shim that takes its subcommand
from `argv[1]` and does not dispatch on `argv[0]`.

This repository contains only the daemon. Packaging, OS images and any
integration tests that boot a real machine belong to whatever OS consumes it.


Design constraints
------------------

**No shelling out to `ip`.** Every link, address, route, VLAN, bridge and VRF
operation goes through `src/netlink/rtnl.c` against a raw `AF_NETLINK` socket.
This is not a style preference. Calling `ip` reintroduces a `PATH` dependency
on the switch and, worse, makes operations asynchronous: the command returns
before the kernel has applied anything, which is what forces tests to sleep and
retry. Netlink acks before we reply, so callers are synchronous by
construction. `tests/unit/test_netlink.sh` enforces this.

**No libnl or libmnl.** `src/netlink/netlink.h` talks to the kernel directly.
There are no link-time dependencies: plugins are separate processes, so
nothing is dlopen'd.

**Accepted IPC connections are blocking.** The listener is non-blocking so it
can sit in `poll()`, but the accepted connection must not be: a non-blocking
accepted fd makes the server read `EAGAIN`, stdio report EOF, and the parser
see an empty stream, so it replies with nothing. That was the actual cause of
what looked like IPC flakiness. Bounded instead by `SO_RCVTIMEO`/`SO_SNDTIMEO`.

**Clients half-close after writing.** Without `shutdown(sock, SHUT_WR)` the
server waits for a second command until its receive timeout expires.


Building
--------

    make                  # build for linux-glibc-amd64
    make targets          # list available target triples
    make clean

Requires [`dep`](https://github.com/finwo/dep) on `PATH` and network access to
`git.finwo.net`.

The top-level `Makefile` is an assembler, not a compiler. It stages a
self-contained tree by layering three things into `build/<triple>/`:

    src/              ->  build/<triple>/src      the daemon sources
    target/common/    ->  build/<triple>/         the real Makefile + .dep
    target/<triple>/  ->  build/<triple>/         per-triple overlay

then runs `dep install` inside it and builds there. `target/<triple>/` is an
extension point for a `Makefile.target`, which `target/common/Makefile` picks
up with `-include`; the one existing triple has none, so it is currently empty.

`build/` is generated and gitignored in full.

### Dependencies

`target/common/.dep` declares three: `cofyc/argparse`, `finwo/cnfparse` and
`rxi/log`. A fourth, `finwo/buf`, arrives transitively through cnfparse's own
manifest. All four are compiled from source straight into the binary; nothing
is linked against a prebuilt library.

They are fetched from branch tips rather than tags, and are not checksummed.
That means a build is not reproducible across upstream changes, and a sha256 on
a release tarball of *this* repository pins its own code but not the four
libraries compiled into the resulting binary. This is a deliberate, known
deviation from the pin-everything rule this project otherwise follows, and the fix
(tagging those repositories) is upstream of here.


Testing
-------

    ./tests/run.sh

All tests are unit tests: they build the daemon and exercise it directly.
`test_parse.sh` links the real `ifaces`/`ports`/`config` objects and parses a
generated config, so it fails when the grammar changes rather than when a
fixture drifts.

Anything requiring a booted system -- bridges and VLANs against a real kernel,
VRF table allocation, address application -- is an integration test and lives
in the consuming OS repository, which can build a rootfs and run a VM.

Note for anyone editing the source-grepping guards in `test_netlink.sh`: each
one asserts the file exists before grepping it. A grep that matches nothing
because a file moved is indistinguishable from one that matches nothing because
the code is correct, and only the second should pass.


Configuration
-------------

`/etc/linkd.cnf`, same grammar as the rest:

    config_ports  /etc/network/ports /etc/network/ports.d/*.cnf
    config_iface  /etc/network/interfaces /etc/network/interfaces.d/*.cnf
    listen        unix:///var/run/linkd.sock
    listen        tcp://127.0.0.1:6789
    authfile      /etc/linkd.passwd
    plugin        /usr/lib/linkd/bcm

A missing file is not an error; the defaults above are what you get. Patterns
are expanded by `source`, so one matching nothing is fine -- a system with no
drop-ins is normal.


Clients and authorization
-------------------------

The daemon speaks RESP on every address it is told to `listen` on, so
`redis-cli` works as a client. `linkctl` sends whatever you give it:

    linkctl PING
    linkctl COMMAND
    linkctl IFQUERY lo

`ifup`, `ifdown`, `ifquery` and `ifreload` are symlinks that send the
corresponding command.

A connection has one of three roles, and every command names the minimum it
needs:

| Role       | Commands                                      |
| ---------- | --------------------------------------------- |
| *none*     | `PING`, `AUTH`, `QUIT`                        |
| `readonly` | + `IFQUERY`, `INFO`, `COMMAND`                |
| `full`     | + `IFUP`, `IFDOWN`, `IFRELOAD`                |

**A root peer on a unix socket starts with `full`**: the kernel vouches for the
uid via `SO_PEERCRED`, which beats any password. Everyone else starts at *none*
and authenticates with `AUTH <user> <password>`.

Credentials live in the file named by `authfile`:

    admin:$pbkdf2-sha256$29000$<salt>$<hash>:full
    watcher:$pbkdf2-sha256$29000$<salt>$<hash>:readonly

Generate entries with `linkctl hash [user]`. The format is passlib's
`pbkdf2_sha256`, adapted base64 and all, so entries made by passlib or Django
work unchanged. A missing role field means `readonly`.

The file is refused outright if it is world-writable, and warned about if it is
readable beyond its owner. **Binding a `tcp://` listener without an `authfile`
is a hard error**, so an unauthenticated network listener cannot be created by
forgetting something.


Dataplane plugins
-----------------

A plugin is a **program**, not a shared library. linkd talks to it in RESP
(the Redis wire protocol), so a plugin can be written in anything that can read
and write a pipe. `tests/fixtures/plugin-echo.sh` is a working one in POSIX
shell, and the test suite drives it, so that claim is tested rather than
asserted.

Plugins are declared in `/etc/linkd.cnf`:

    plugin /usr/lib/linkd/bcm              spawned, RESP over stdin/stdout
    plugin tcp://user:pass@host:6789       connected to, AUTH if credentials
        optional                           failures do not fail operations

On startup linkd asks each plugin `COMMAND` for the verbs it implements and
`INFO` for its identity. `INFO` must report `hardware_detected:0|1` in its
`# Stats` section; a plugin answering `0` is refused, which is how a driver for
absent hardware declines rather than failing later and opaquely.

Operations are **broadcast** to every plugin advertising the verb, and all of
them are called even if an earlier one fails, so plugins cannot end up
disagreeing about what was applied:

    PORT ADMIN <if> <up|down>      RIF   ADD|DEL <if> <cidr> table <n>
    PORT MTU <if> <mtu>            NEIGH ADD|DEL <if> <ip> <mac> table <n>
    PORT APPLY <if> [speed N] [fec X] [autoneg 0|1]
    ROUTE ADD|DEL <dst> [via <gw> dev <if>]... [metric N] table <n>
    RESYNC

Any `-ERR` from a plugin fails the operation unless that plugin is `optional`.

A plugin may also own configuration. When the `plugin` stanza hits a
sub-directive linkd does not recognise, it starts the plugin, asks `CONFIG
LIST` for the directives it accepts, and forwards matching ones as
`CONFIG SET <directive> <args...>`. A directive neither side claims ends the
stanza and is re-dispatched normally.

A plugin that dies or stops answering is restarted with backoff (0.5s
doubling to 30s). While it is down its operations fail, so a switch never
silently accepts configuration it is not programming into hardware.

`plugins/bcm/` is the Broadcom XGS plugin, currently a skeleton that answers
`COMMAND`/`INFO` and errors on everything else. It is not built by the
top-level Makefile and requires `SDK=` pointing at a built OpenBCM tree:

    make -C plugins/bcm SDK=/path/to/opennsl

It is intended to be built and shipped by the `openbcm` package, which does not
exist yet.
