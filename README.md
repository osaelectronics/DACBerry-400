# DACberry400 driver setup

## 1. Install using Script

### 1. Prerequisite
------------------------------------------------------------
Install latest Raspberry OS (Kernel version: 5.10) in SD card. 
Current driver only supported to kernel version 5.10.17, 5.10.52, 5.10.60, 5.10.63, 5.10.90, 5.10.92, 5.10.103 and 5.15.32 only.

Note: dacberry400 driver available in raspberry pi OS from kernel version 5.10.61. You need to run only dacberry400 script to enable it.

### 2. Setup
------------------------------------------------------------
1. Download Script dacberry400.sh and place in /home/pi directory. 
2. run the script.
    bash dacberry400.sh

## 2. Custom compilation and setup

You need two drivers to enable Dacberry400 Codec:

1) TLV320AIC3x CODEC driver
2) Dacberry400 Driver

### 1. Compilation Steps to support TLV320AIC3x CODEC driver:

1) Download kernel source code from https://github.com/raspberrypi/linux. 
   Note: Make sure to get the soruce code for same version what you have installed in Raspberry Pi.

2) Install prerequisite and cross toolchain for compilation. Refer following link for download toolchain and setup for specific platform: 
   https://www.raspberrypi.com/documentation/computers/linux_kernel.html
   
3) Before compilation, enable TLV320AIC3x CODEC driver from menconfig.
   it is located at:
   - Device Drivers
     - Sound card support (SOUND [=y])
	   - Advanced Linux Sound Architecture (SND[=m])
	     - ALSA for SoC audio support (SND_SOC [=m])
		   - CODEC drivers
			 - Texas Instruments TLV320AIC3x CODECs
4) Compile kernel for specific platform
	Refer Link: https://www.raspberrypi.com/documentation/computers/linux_kernel.html
	
5) After compilation, TLV320AIC3x CODEC driver located at:
	linux/sound/soc/codecs/snd-soc-tlv320aic3x.ko

### 2. Compilation Steps to support Dacberry400 driver:

1) Download Dacberry400.c and Makefile from https://github.com/osaelectronics/DACBerry-400.

2) compile it: make KDIR=<Kernel_path>
	Define the full kernel path where it is compilied.

	
### 3. Configuration and Setup in Raspberrypi:

#### 1. Config.txt setup:

1) Add following line in boot/config.txt
    - dtoverlay=i2c1
	- dtoverlay=dacberry400
	- gpio=26=op,dh
 
2) Enable i2c overlay:
	- dtparam=i2c_arm=on
	- dtparam=i2s=on
	
3) Disable default Audio: 
	#dtparam=audio=on
	
#### 2. dts file setup:

1) Download dacberry400.dts from https://github.com/osaelectronics/DACBerry-400.

2) dtc-O dtb -o dacberry400.dtbo -b 0 -@ dacberry400.dts

3) copy dacberry400.dtb to boot/overlays/

#### 3. Load Driver in Kernel:

1) Copy snd-tlv-tlv320aic3x.ko in /lib/modules/$(uname -r)/kernel/sound/soc/codecs/
2) Copy dacberry400.ko in /lib/modules/$(uname -r)/kernel/sound/soc/bcm/
3) depmod -a
4) reboot the board
