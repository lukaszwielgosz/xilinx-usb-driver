# Xilinx Platform Cable USB driver
Tested on fedora33


```
cd /opt/Xilinx/
sudo git clone git@github.com:lukaszwielgosz/xilinx-usb-driver.git
cd xilinx-usb-driver/
sudo dnf install fxload libusb-devel
sudo make
./setup_pcusb /opt/Xilinx/14.7/ISE_DS/ISE
sudo udevadm control --reload-rules
```

plug and unplug


You may need to set the following env variable after sourcing a settings file.

in `~/.bashrc` add line
```
export LD_PRELOAD=/opt/Xilinx/xilinx-usb-driver/libusb-driver.so
```

```
source ~/.bashrc
impact
```
