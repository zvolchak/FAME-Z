[Gen-Z is a new memory-semantic fabric](https://genzconsortium.org/) designed as the glue for constructing exascale computing.  It is an open specification evolved from the fabric used in [The Machine from Hewlett Packard Enterprise](https://www.hpe.com/TheMachine).  Such fabrics allow "wide-area" connectivity of computing resources such as CPU, GPU, memory (legacy and persistent) and other devices via a memory-semantic programming model.

The Gen-Z spec and working groups are evolving that standard, and early hardware is beginning to appear.  However there is not an open "platform" on which to develop system software.  [The success of QEMU/IVSHMEM as an emulated development platform for The Machine](docs/FAME_background.md) suggests an extended use should be considered. 
  
### Beyond IVSHMEM - a rudimentary fabric

QEMU has another feature of interest in a multi-actor messaging environment like that of Gen-Z.  By applying a slightly different stanza, the IVSHMEM virtual PCI device is enabled to send messages and handle interrupts in a "mailbox/doorbell" setup.   An interrupt to the virtual PCI device is generated from an "event notification" issued to the QEMU process by a similarly configured peer QEMU.  But how are these peers connected?

The scheme starts with a separate program delivered with QEMU, [```/usr/bin/ivshmem-server. ivshmem-server```](https://www.google.com/search?newwindow=1&qivshmem-spec.txt) establishes a UNIX-domain socket and must be started before any properly-configured QEMU VMs.  As a QEMU process starts, it connects to the socket and is given its own set of event channels, as well as being advised of all other peers.  The idea is that each VM can issue messages through its PCI device which are delivered directly to another QEMU process as a Linux event.  The target QEMU turns that event into a PCI interrupt for its guest OS.  ```ivshmem-server``` only informs each QEMU of the other peers, it does not participate in further peer-to-peer communcation.  A backing file must also be specified during this scenario to be used as a mailbox for larger messages.

![alt text][IVSHMSG]

[IVSHMSG]: https://github.com/coloroco/FAME-Z/blob/master/docs/images/IVSHMSG%20block.png "Figure 1"

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

Another program, ```ivshmem-client```, is also delivered with QEMU.  Using just ivshmem-server and multiple ivshmem-clients you can get a feel for the messaging abilities of ivshmem-clients.  The final use case is actually VM-to-VM
communication over the "IVSHMEM doorbell/mailbox fabric" which I call IVSHMSG.  This requires handling the interrupts and mailbox usage in a coordinated fashion.

## Potential Gen-Z Emulation

VM-to-VM communication will involve a (new) kernel driver and other abstractions to hide the mechanics of IVSHMSG.  If the driver functions as a simple Gen-Z bridge, it is anticipated that a great deal of "pure Gen-Z" software development
can be done on this simple platform.  Certain Gen-Z primitive operations for discovery and crawlout would also be abetted by intelligence "in the fabric", ie, the ivshmem-server process.

Extending the existing C program is not a simple challenge as it is not a standalone program.
Unfortunately [```ivshmem-server.c```](https://github.com/qemu/qemu/tree/master/contrib/ivshmem-server) is written within the QEMU build framework.   And C is a bit limited for higher-level data constructs anticipated for a Gen-Z emulation.

This FAME-Z project started out as a rewrite of ivshmem-server in Python using Twisted as the network-handling framework.  ```famez_server.py``` is run in place of ```ivshmem-server```.  It correctly serves ```ivshmem-client``` as well as real QEMU processes.  

![alt text][FAME-Z]

[FAME-Z]: https://github.com/coloroco/FAME-Z/blob/master/docs/images/FAME-Z%20block.png "Figure 2"

FAME-Z is intended for simple device connectivity, ie, bridge-to-bridge with some simple switches.  As with all emulations there will be a point at which the effort fails to reproduce the environment accurately.  This experiment will find that breakdown point and determine whether the functional section is indeed sufficient to allow Gen-Z software development in the absence of hardware.

A new feature over ```ivshmem-server``` is that ```famez_server``` can receive messages from clients.  Thus the famez_server can participate in fabric messaging and serve as fabric intelligence (ie, a switch).  Again, the accuracy and validity of the project must lead to the creation of "pure Gen-Z" software above it, meaning it will run on real hardware someday without modification.

## Running the Python rewrites

As the famez_server.py was created, there is also a famez_client.py to supplant the stock ivshmem-client.  It has an expanded command set over the original.  Over time its use as a monitor/debugger/injector will certainly grow.  To see these packages function as a simple chat framework, you don't need QEMU.

1. Clone this repo
1. Install python3 package :twisted"
1. In one terminal window run './famez_server.py  --nClients 8'.  This provides acceptance for six attached clients such as famez_client.c, ivshmem-client, or a properly-configured QEMU process.  By default this creates /tmp/ivshmsg_socket to which clients attach, and /dev/shm/famez_mailbox which is shared among all clients for messaging.
1. In a second (or more) terminal window run 'ivshmem-client '.  You'll see them get added in the server log output.
1. In one of the clients, hit return, then type "help".  Play with sending messages to the other client(s) or the server.

## Connecting VMs

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

## BUGS

As the QEMU docs say, "(IVSHMSG) is simple and fragile" and the driver modules are still under active development.  Sometime a QEMU hangs and you have to restart it.  Sometimes you have to restart all QEMUs.  Rarely do you have to restart a server that was running --recycle, but it happens.  The way I crafted an interlock protocol in the kernel
drivers can cause a VM to go into RCU stall which usually leads to a virtual panic.

## TODO

As mentioned earlier the idea is to write "up the stack" for Gen-Z support for the various entity "Managers" in the specification.   So it's back to the spec which is beyond the scope of this document, but more will be revealed.
