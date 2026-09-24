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
The only link-time dependency is `-ldl`, for `dlopen`ing dataplane plugins.

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


Dataplane plugins
-----------------

`src/dataplane/plugin.c` `dlopen`s shared objects from
`/usr/lib/linkd/dataplane/` (`LINKD_DATAPLANE_DIR` in `src/dataplane.h`), each
exporting a `struct dp_ops` matching `LINKD_DATAPLANE_ABI`. Without one, linkd
uses the built-in kernel dataplane.

`plugins/bcm/` is the Broadcom XGS plugin. It is not built by the top-level
Makefile and requires `SDK=` pointing at a built OpenBCM tree:

    make -C plugins/bcm SDK=/path/to/opennsl

It is intended to be built and shipped by the `openbcm` package, which does not
exist yet.
