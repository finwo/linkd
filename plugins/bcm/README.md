# plugins/bcm

Broadcom XGS dataplane backend for `linkd`.

This is **not** built by the `linkd` build. It is built and shipped by the
`openbcm` package, which is installed only on hardware that has a Broadcom
switching ASIC.

That split is deliberate: the OS image is byte-identical on every machine.
Hardware capability arrives as a package, not as a separate image flavour.
`linkd` dlopens whatever it finds in `/usr/lib/linkd/dataplane/`, so on a box
without an ASIC the directory is simply empty and the built-in kernel
dataplane is used instead.

## Contract

A plugin is a shared object exporting exactly one symbol:

```c
struct dp_ops linkd_dataplane_ops;
```

`dp_ops` is declared in `src/dataplane.h`. The `abi` field must equal
`LINKD_DATAPLANE_ABI` or the plugin is refused at load time.

`probe()` must be cheap and side-effect free. It runs against every registered
backend before one is chosen. This backend probes for `/dev/linux-kernel-bde`,
which the openbcm kernel modules create once they have enumerated a switch ASIC
over PCIe.

## Building

Requires a built OpenBCM SDK tree:

```sh
make SDK=/path/to/openbcm/sdk-6.5.27
make SDK=/path/to/openbcm/sdk-6.5.27 install DESTDIR=/
```

## Status

Skeleton. Every entry point past `probe()` returns `DP_RET_ERROR` with a TODO
naming the SDK call that belongs there. Real implementation is gated on having
the board configuration for the target switch, without which the SDK cannot map
logical ports to SerDes lanes.
