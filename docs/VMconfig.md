THIS INFO IS STALE AND/OR INACCURATE BUT WILL BE UPDATED IN EARLY NOVEMBER

While a QEMU process does the actual connection to the famez_server.py, it's the VM inside QEMU where the messaging endpoints take place.  Building a QEMU image is beyond the scope of this project.  The FAME project mentioned previously is a great place to accomplish that.

Before starting down this path, stop and restart the famez_server.py with the additional argument "--recyle".  This extension over the stock server makes the QEMU members of the fabric a little more resilient when other peers die.

Then build and boot your VMs.

1. Log in to each VM and git clone this repo.
1. "cd famez_kernel/modular" and "make" which should create two kernel modules, "famez.ko" and "famez_bridge.ko"
1. "sudo insmod famez.ko famez_verbose=2".  dmesg output should indicate the driver found and attached to the IVSHMSG device, a "Redhat Emulation" card.
1. sudo insmod famez_bridge.ko fzbridge_verbose=2.  Again dmesg output should now show the driver bound to the famez driver.  There should also be a new device file /dev/famez_bridge_xx where xx matches the PCI pseudevice address in lspci.
1. Run a famez_client.py program and execute "list".  This shows you participants and they're IVSHMSG ID.
1. On a VM, echo "I:hello there" > /dev/famez_bridge_xx, where "I" is the IVHSHMSG client number of a client.
2. On a different VM (the target of the echo command above) "cat < /dev/famez_bridge_xx" and you should see the message.

You can exercise the link a little harder with the programs in the "rocpyle" directory.

The configuration stanza used for IVSHMEM + IVSHMSG:
```
-chardev socket,id=ZMSG,path=/tmp/ivshmsg_socket -device ivshmem-doorbell,chardev=ZMSG,vectors=4
```
or in a domain XML file,
```
  <qemu:commandline>
    <qemu:arg value='-chardev'/>
    <qemu:arg value='socket,id=ZMSG,path=/tmp/ivshmsg_socket'/>
    <qemu:arg value='-device'/>
    <qemu:arg value='ivshmem-doorbell,chardev=ZMSG,vectors=4'/>
  </qemu:commandline>
```

