sudo pkill -2 tripled_64
sleep 0.2
sudo rmmod usb2cansocketcan
sudo insmod usb2cansocketcan.ko
sudo cp usb2cansocketcan.ko /lib/modules/$(uname -r)/kernel/drivers/net/can
sudo depmod -a
sudo modprobe usb2cansocketcan
sudo ./tripled_64 ttyACM0 -s0x09:0x0A:0x0A -ncan0:can1:canfd2
sleep 1
sudo ip link set can0 up qlen 1000
sleep 1
sudo ip link set can1 up qlen 1000
sleep 1
sudo ip link set canfd2 up qlen 2000
#sleep 5
#sudo ./tripled_64 -s250 ttyACM1 can3 can4 canfd5
#sudo sudo ip link set can3 up qlen 1000
#sleep 1
#sudo sudo ip link set can4 up qlen 1000
#sleep 1
#sudo sudo ip link set canfd5 up qlen 1000
#sleep 1
