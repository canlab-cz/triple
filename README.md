# USB2CAN

Driver is experimental !!!

SLCAN like driver for PIC based USB to CAN adapter USB2CAN Triple from Canlab s.r.o.
For more info https://github.com/canlab-cz/triple/wiki/Manual

You can find HW here : www.canlab.cz

Easy start :
1. `git clone https://github.com/canlab-cz/triple`
2. `cd triple`
3. `make`
4. `sh ./start.sh`

Start script runs as default this config\
port 1 - speed 250K, listen_only false\
port 2 - speed 500k. listen_only false\
port 3 - speed FD 125k-6M7, listen_only false, iso false, esi false

To kill and unload all `sh ./end.sh` !!!!  Call it before you disconnect adapter from USB !!!!


Driver v1.4 compiles against these kernel headers (build matrix: `sh ci/kbuild/run.sh`, see doc/PLAN.md):

| Distribution | Kernel | Result |
|---|---|---|
| Ubuntu 22.04 | 5.15 | builds |
| Ubuntu 24.04 | 6.8 | builds |
| Fedora 44 | 7.2 | builds |

Older driver versions were tested on Ubuntu 20.04 (5.13) and Ubuntu 22.04 (5.15).

Notes for v1.4:
* The line discipline number is now fixed to 29 (`N_DEVELOPMENT`). The previous value `NR_LDISCS - 1` collides with the in-tree `can327` driver on kernels 5.19 and newer.
* Unplugging the adapter while the driver is attached no longer crashes the kernel, but it still leaves `tripled` running; `sh ./end.sh` remains the clean way to stop.
* Received frames with a hardware timestamp (timestamp mode) are not decoded yet; `tripled` keeps timestamp mode off.
* Two protocol bugs of v1.3 were found with two adapters wired back to back: received extended/standard identifiers were swapped, and standard frames with 6 data bytes could never be sent. Both fixed and verified on hardware.
* Host-side protocol codec test: `make -C tests/codec test`
* Refactoring and analyzer roadmap: doc/PLAN.md

Prepared Vmware virtual machine Ubuntu 22.04 with precompiled driver in /home/triple/triple, so it can be simply runned using start.sh script\
also can-utils package is installed so you can use "candump can0" or "cansend can0 ..."\
login: triple\
pass: triple\
https://drive.google.com/file/d/1kBT1YvstEnOCgCjsyL4rnoIupsYYywCf/view?usp=sharing\

To run this VM you need to install VMware Workstation player :\
https://www.vmware.com/products/workstation-player.html


