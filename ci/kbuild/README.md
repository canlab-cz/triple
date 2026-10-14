# Kernel build matrix

Compiles `driver/` and `utility/` against distribution kernel headers inside
Docker containers, without needing those kernels on the host.

| Target | Image | Kernel headers |
|---|---|---|
| `u2204` | ubuntu:22.04 | linux-headers-generic (5.15) |
| `u2404` | ubuntu:24.04 | linux-headers-generic (6.8) |
| `fedora` | fedora:latest | kernel-devel (7.x) |

```
sh ci/kbuild/run.sh           # all targets
sh ci/kbuild/run.sh u2404     # one target
```

The containers only compile; loading the module needs a machine running that kernel.
