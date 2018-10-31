THIS INFO IS STALE AND/OR INACCURATE BUT WILL BE UPDATED IN EARLY NOVEMBER

## Create QEMU VM(s) for Linux guest OS

These is offered as a suggestion, not an officially supported directive.  YMMV
so please create an issue if you have problems or a better answer.
All three methods assume proper installation of QEMU, libvirtd, and other
tools and utilities.

### Method 1: Use your favorite/current method

If you have a preferred method, use it, then alter the startup of the
virtual machines accordingly.   

The configuration stanza needed for IVSHMSG is
```
-chardev socket,id=IVSHMSG,path=/tmp/ivshmsg_socket -device ivshmem-doorbell,chardev=IVSHMSG,vectors=16
```
or in a domain XML file, at the end:
```
  <qemu:commandline>
    <qemu:arg value='-chardev'/>
    <qemu:arg value='socket,id=IVSHMSG,path=/tmp/ivshmsg_socket'/>
    <qemu:arg value='-device'/>
    <qemu:arg value='ivshmem-doorbell,chardev=IVSHMSG,vectors=16'/>
  </qemu:commandline>
```

The security configuration of some distros will not allow QEMU to open a
socket in /tmp.  "path" may need to be changed.

### Method 2: qemu-img and virt-install

Two references were selected and combined as a working reference:

https://raymii.org/s/articles/virt-install_introduction_and_copy_paste_distro_install_commands.html#virt-install
    
https://docs.openstack.org/image-guide/virt-install.html

Between the two, a different graphics option was chosen to have "background
access" to the virtual console with "virsh console <VMNAME>".  The
following stanzas were replaced:

    --graphics none --console pty,target_type=serial
    --extra-args 'console=ttyS0,115200n8 serial'

1. qemu-img create -f qcow2 ./TARGET.qcow2 4G
1. virt-install --name &lt;WHATEVER&gt; --virt-type kvm --vcpus 1 --ram 1024 \
	--disk path=./TARGET.qcow2 \
	--network network=default \
	--graphics spice --video qxl --channel spicevmc \
	--os-type linux --os-variant auto \
	--location &lt;DISTRO-DEPENDENT&gt;

    Debian: --location
http://ftp.us.debian.org/debian/dists/stable/main/installer-amd64/

    Bionic: --location
http://us.archive.ubuntu.com/ubuntu/dists/bionic/main/installer-amd64/

    CentOS: --location
http://www.gtlib.gatech.edu/pub/centos/7/os/x86_64/

1. Alter the VM invocation as given in "Method 1"

### Method 3: FAME

If your host OS is a Debian distro (or derivative like Ubuntu) you can use
the [emulated development platform for The Machine](FAME_background.md).
It will do all the necessary things:

* Set up a dedicted virtual network (no need to use default)
* Build and customize multiple VMs
* Use correct startup for IVSHMSG
* Optionally use IVSHMEM as Fabric Attached Memory

The FAM will appear as another virtual PCI device in the guest OS.

## Running Linux guests with IVSHMSG kernel modules

While a QEMU process makes the network connection to ivshmsg_server.py, it's
the guest OS inside QEMU where the messaging endpoints take place.

1. Insure ivshmem_server.py is running with the correct --socketpath
1. Start the VM(s)
1. Log in to the VM and git clone these repos:
   1. IVSHMSG
   1. EmerGen-Z
1. "cd famez_kernel/modular" and "make" which should create two kernel modules, "famez.ko" and "famez_bridge.ko"
1. "sudo insmod famez.ko famez_verbose=2".  dmesg output should indicate the driver found and attached to the IVSHMSG device, a "Redhat Emulation" card.
1. sudo insmod famez_bridge.ko fzbridge_verbose=2.  Again dmesg output should now show the driver bound to the famez driver.  There should also be a new device file /dev/famez_bridge_xx where xx matches the PCI pseudevice address in lspci.
1. Run a famez_client.py program and execute "list".  This shows you participants and their IVSHMSG ID.
1. On a VM, echo "I:hello there" > /dev/famez_bridge_xx, where "I" is the IVHSHMSG client number of a client.
2. On a different VM (the target of the echo command above) "cat < /dev/famez_bridge_xx" and you should see the message.

