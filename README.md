# DaynaPORT DOS Packet Driver

The most excellent references that made this driver possible!
* [DaynaPORT Command Set](https://github.com/PiSCSI/piscsi/wiki/Dayna-Port-Command-Set)
* [Adaptec ASPI SDK](https://tinkerdifferent.com/threads/adaptec-aspi-sdk-dos-windows-3-x-16bit-scsi-development.3466)
* [PC/TCP Packet Driver Specification](https://web.archive.org/web/20221127060523/http://crynwr.com/packet_driver.html)
* [DaynaPORT BlueSCSI Code](https://github.com/BlueSCSI/BlueSCSI-v2/blob/main/lib/SCSI2SD/src/firmware/network.c)


## Building
1. Download and install Borland Turbo C++ 3.x for DOS
2. Launch it and open the DAYNA.PRJ file
3. From there Compile | Build All and you will have an .EXE!

If you would rather create your own Turbo C++ project file, or wish to try and port this to another compiler, note the following:
* All source code is present in `DAYNA.C`
* You'll want to use the Compact memory model since this application uses far pointers
   * Options | Compiler | Code Generation | Compact

## Running
Simple! 
1. `dayna.exe vector scsi_id <adapter_id>`
  * Note that the adapter_id is optional and defaults to zero
  * Note that valid vector numbers range from 0x60 to 0x80
  * example command: `dayna.exe 0x60 4`
2. From there you can configure mTCP or your favorite program that uses a packet driver!

## Unloading
Uhh... we'll get back to you on that


## Known Issues
1. No unloading yet
2. Uses too much memory
3. Needs testing with different SCSI cards
4. Only supports class 1 operations (which is fine for packet drivers, but suboptimal)
   *  Well, some of them.  Not Terminate.  Not Reset Interface.  Did I mention this is very Beta, maybe even Alpha?
5. Responds to all packet types, independent of what the client request