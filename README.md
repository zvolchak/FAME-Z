## FAME Background

[Gen-Z is a new memory-semantic fabric](https://genzconsortium.org/) designed as the glue for constructing exascale computing.  It is an open specification evolved from the fabric used in [The Machine from Hewlett Packard Enterprise](https://www.hpe.com/TheMachine).  Such fabrics allow "wide-area" connectivity of computing resources such as CPU, GPU, memory (legacy and persistent) and other devices via a memory-semantic programming model.

The Machine consists of up to 40 nodes of an SoC running independent instances of Linux.  All nodes share a 160 TB fabric-attached memory (FAM) global address space via the Gen-Z precursor fabric.  Software to manage the FAM was being designed well in advance of hardware availability, and a suitable development platform was needed.  QEMU/KVM provided the basis for such a platform: a single node was represented by a single VM.  The QEMU feature Inter-VM Shared Memory (IVSHMEM) presents a file in the host operating system as physical address space in a VM.  If all VMs use the same backing store, you get ["Fabric-Attached Memory Emulation" or FAME](https://github.com/FabricAttachedMemory/Emulation).

Similarly, the Gen-Z spec and working groups are evolving that standard, and early hardware is beginning to appear.  However there is not an open "platform" on which to develop system software.  The success of FAME suggests extended use should be considered. 

### FAME Configuration

When QEMU is invoked with IVSHMEM configuration, a new PCI device appears in the VM.  The size/space of the file is represented as physical address space behind BAR2 of that device.  To configure a VM for IVSHMEM/FAME, first allocate the file somewhere (such as /home/rocky/FAME/FAM of 32G), then start QEMU with the added stanza

```
-object memory-backend-file,mem-path=/home/rocky/FAME/FAM,size=32G,id=FAM,share=on -device ivshmem-plain,memdev=FAM
```
or add these lines to the end of a libvirt XML domain declaration for a VM:
```XML
  <qemu:commandline>
    <qemu:arg value='-object'/>
    <qemu:arg value='memory-backend-file,mem-path=/home/rocky/FAME/FAM,size=32G,id=FAM,share=on'/>
    <qemu:arg value='-device'/>
    <qemu:arg value='ivshmem-plain,memdev=FAM'/>
  </qemu:commandline>

```
From such a VM configured with a 32G IVSHMEM file:
```bash
rocky@node02 $ lspci -v
  :
00:09.0 RAM memory: Red Hat, Inc Inter-VM shared memory (rev 01)
Subsystem: Red Hat, Inc QEMU Virtual Machine
Flags: fast devsel
Memory at fc05a000 (32-bit, non-prefetchable) [size=256]
Memory at 800000000 (64-bit, prefetchable) [size=32G]
Kernel modules: virtio_pci
```
The precise base address for the 32G space may vary depending on other VM settings.  All of this is handled by the setup script of the [FAME project](https://github.com/FabricAttachedMemory/Emulation).  Be sure and read the wiki there, too.
  
### IVSHMSG Configuration

IVSHMEM has another feature of interest in a multi-actor messaging environment such as Gen-Z.  By applying a slightly different stanza, the IVSHMEM virtual PCI device is enabled to drive interrupts.   An interrupt to the virtual PCI device is generated from an "event notification" issued to the QEMU process via a UNIX domain socket.  But where do these events originate?

The scheme requires a separate process delivered with QEMU, [```/usr/bin/ivshmem-server. ivshmem-server```](https://www.google.com/search?newwindow=1&qivshmem-spec.txt) establishes the socket and must be started before any properly-configured VMs.  As a QEMU process starts, it connects to ```ivshmem-server``` and is given its own set of event channels, as well as being advised of all other peers.  The idea is that each VM can issue messages through the IVSHMEM device which are delivered directly to another VM as an interrupt.  ```ivshmem-server``` only informs each QEMU of the others, it does not participate in further peer-to-peer communcation.

A backing file can also be specified during this IVSHMG (Inter-VM Shared Messaging) scenario.  This is used as a mailbox for larger messages.  A kernel driver in one VM puts data in the shared space, then sends a message (i.e., rings the doorbell).  The target VM driver receives the interrupt and retrieves the appropriate data.  The configuration stanza is
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

## FAME-Z Potential

Unfortunately [```ivshmem-server.c```](https://github.com/qemu/qemu/tree/master/contrib/ivshmem-server) is written within the QEMU build framework.  It is not a standalone program that permits easy development.  And C is a bit limited for higher-level data constructs anticipated for a Gen-Z emulation.

This FAME-Z project started out as a rewrite of ivshmem-server in Python using Twisted as the network-handling framework.  ```famez_server.py``` is run in place of ```ivshmem-server``` and correctly serves ```ivshmem-client``` as well as real VMs.  A new feature over ```ivshmem-server``` is that ```famez_server``` can receive messages from clients.  I anticipate this will be needed to facilitate some of the Gen-Z messaging support.

The intended first use of FAME-Z is support for the [Gen-Z Management Architecture Authoring Sub-Team (MAAST)](https://genz.causewaynow.com/wg/swmgmt/document/folder/100).  MAAST is providing a suggested reference procedure and architecture for Gen-Z discovery, configuration and management.  Once again a suitable platform for actual software development is lacking and FAME-Z could fill that void.

## TODO
1. Write a simple kernel driver so a VM can receive IVSHMSG interrupts, test it with ivshmem-client.  This would actually be a Gen-Z bridge device.
1. Extend the Gen-Z bridge IVSHMSG driver to generate messages to create interrupts on other clients (VMs or ivshmem-client).
1. Rewrite ```ivshem-client.c``` in Python to easily extend its abilities as needed.
1. Extend the FAME-Z framework to handle Gen-Z abstractions.
1. ...ad infinitum
