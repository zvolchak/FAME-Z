# FAME Background

[Gen-Z is a new memory-semantic fabric](https://genzconsortium.org/) designed as the glue for constructing exascale computing.  It is an open specification evolved from the fabric used in [The Machine from Hewlett Packard Enterprise](https://www.hpe.com/TheMachine).  Such fabrics allow "wide-area" connectivity of computing resources such as CPU, GPU, memory (legacy and persistent) and other devices via a memory-semantic programming model.

The Machine consists of up to 40 nodes of an SoC running independent instances of Linux.  All nodes share a 160 TB fabric-attached memory (FAM) global address space via the Gen-Z precursor fabric.  Software to manage the FAM was being designed well in advance of hardware availability, and a suitable development platform was needed.  QEMU/KVM provided the basis for such a platform: a single node was represented by a single VM.  The QEMU feature Inter-VM Shared Memory (IVSHMEM) presents a file in the host operating system as physical address space in a VM.  If all VMs use the same backing store, you get ["Fabric-Attached Memory Emulation" or FAME](https://github.com/FabricAttachedMemory/Emulation).

Similarly, the Gen-Z spec and working groups are evolving that standard, and early hardware is beginning to appear.  However there is not an open "platform" on which to develop system software.  The success of FAME suggests extended use should be considered. 

## FAME Configuration

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
  
# FAME-Z Potential

IVSHMEM has another feature of interest in a multi-actor messaging environment such as Gen-Z.  By applying a slightly different stanza, the IVSHMEM virtual PCI device is enabled to drive interrupts.   An interrupt to the virtual PCI device is generated from an "event notification" issued to the QEMU process via a UNIX domain socket.  But where do these events originate?

The scheme requires a separate process delivered with QEMU, [```/usr/bin/ivshmem-server. ivshmem-server```](http://www.lmgtfy.com/?q=ivshmem-spec.txt) establishes the socket and must be started before any properly-configured VMs.  As a QEMU process starts, it connects to ```ivshmem-server``` and is given its own set of event channels, as well as being advised of all other peers.  The idea is that each VM can issue messages through the IVSHMEM device which are delivered directly to another VM as an interrupt.  After 
  
