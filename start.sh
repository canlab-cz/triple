sudo pkill -2 tripled_64
sleep 0.2
sudo rmmod usb2cansocketcan
sudo insmod usb2cansocketcan.ko
sudo cp usb2cansocketcan.ko /lib/modules/$(uname -r)/kernel/drivers/net/can
sudo depmod -a
sudo modprobe usb2cansocketcan
sudo ./tripled_64 -s250 ttyACM0 triplecan0 triplecan1 triplecan2
sudo sudo ip link set triplecan0 up qlen 1000
sleep 1
sudo sudo ip link set triplecan1 up qlen 1000
sleep 1
sudo sudo ip link set triplecan2 up qlen 1000
