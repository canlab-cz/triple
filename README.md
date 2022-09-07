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


Tested on Ubuntu 20.04 - kernel 5.13\
Tested on Ubuntu 22.04 - kernel 5.15

Prepared Vmware virtual machine Ubuntu 22.04 with precompiled driver in /home/triple/triple, so it can be simply runned using start.sh script\
also can-utils package is installed so you can use "candump can0" or "cansend can0 ..."\
login: triple\
pass: triple\
https://drive.google.com/file/d/1VFilJfrP_WtX9L-TFU91_EmzbPdkmtHW/view?usp=sharing\

To run this VM you need to install VMware Workstation player :\
https://www.vmware.com/products/workstation-player.html


