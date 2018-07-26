[Gen-Z is a new memory-semantic fabric](https://genzconsortium.org/) designed as the glue for constructing exascale computing.  It is an open specification evolved from the fabric used in [The Machine from Hewlett Packard Enterprise](https://www.hpe.com/TheMachine).  Such fabrics allow "wide-area" connectivity of computing resources such as CPU, GPU, memory (legacy and persistent) and other devices via a memory-semantic programming model.

The Gen-Z spec and working groups are evolving that standard, and early hardware is beginning to appear.  However there is not an open "platform" on which to develop system software.  [The success of FAME as an emulated development platform for The Machine](docs/FAME_background.md) suggests an extended use should be considered. 
  
### Beyond IVSHMEM

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

Another program, ```ivshmem-client```, is also delivered with QEMU.  Using just ivshmem-server and multiple ivshmem-clients you can get a feel for the messaging abilities of IVSHMEM/IVSHMSG.  Combining the mailbox idea of IVHSMEM suggests a potential development environment for Gen-Z, once the server is extended.

## Potential Gen-Z Emulation

Unfortunately [```ivshmem-server.c```](https://github.com/qemu/qemu/tree/master/contrib/ivshmem-server) is written within the QEMU build framework.  It is not a standalone program that permits easy development.  And C is a bit limited for higher-level data constructs anticipated for a Gen-Z emulation.

This FAME-Z project started out as a rewrite of ivshmem-server in Python using Twisted as the network-handling framework.  ```famez_server.py``` is run in place of ```ivshmem-server```.  It correctly serves ```ivshmem-client``` as well as real QEMU processes.  A new feature over ```ivshmem-server``` is that ```famez_server``` can receive messages from clients.  I anticipate this will be needed to facilitate some of the Gen-Z messaging support.

FAME-Z is intended for simple device connectivity, ie, bridge-to-bridge with some simple switches.  As with all emulations there will be a point at which the effort fails to reproduce the environment accurately.  This experiment will find that breakdown point and determine whether the functional section is indeed sufficient to allow Gen-Z software development in the absence of hardware.

![alt text][FAME-Z]

[FAME-Z]: https://github.com/coloroco/FAME-Z/blob/master/docs/images/FAME-Z%20block.png "Figure 2"

## Running with ivshmem-client

1. Install python3 packages daemonize and twisted.
1. Install libvirt and qemu packages for your distribution.  These should also get /usr/bin/ivshmem-client.
1. In one terminal window run './famez_server.py'.  The server is peer ID zero.  You can run 'famez_server.py --help' for an option synopsis.
1. In a second and third terminal window run 'ivshmem-client -v -S /tmp/famez_socket'.  You'll see them get added in the server log output.
1. In one of the clients, hit return, then type "help".  Play with sending interrupts to the other client or the server.

Building a QEMU image is beyond the scope of this project.  The FAME project mentioned previously is a great place to accomplish that.

## TODO
1. Write a simple kernel driver so a VM can receive IVSHMSG interrupts, test it with ivshmem-client.  This would actually be a Gen-Z bridge device.
1. Extend the Gen-Z bridge IVSHMSG driver to generate messages to create interrupts on other clients (VMs or ivshmem-client).
1. Rewrite ```ivshem-client.c``` in Python to easily extend its abilities as needed.
1. Add command line input to famez_server.py to issue commands/messages.
1. Extend the FAME-Z framework to handle Gen-Z abstractions.
1. Write a proper setup.py for setuptools installer.
1. ...ad infinitum
