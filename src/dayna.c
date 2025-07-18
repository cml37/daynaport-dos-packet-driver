/* daynaport.c - MS-DOS Packet Driver for DaynaPORT SCSI/Link */

/* Load ASPI manager for your card (e.g. DEVICE=ASPI7DOS.SYS in CONFIG.SYS). */
/* Run the driver: DAYNAPORT.EXE.
   The driver installs at interrupt 0x60 and stays resident.
*/

/* NOTE: This program uses the Compact memory model since far pointers are present and utilized! */

/* NOTE: THIS IS VERY VERY BETA.  NO GUARANTEES.  NO PROMISES */

/* TODO: When we get this wrapped, test with additional SCSI cards */
/* TODO: Support driver unloading */

/* Resources:                                                                                                           */
/* DaynaPORT Command Set: https://github.com/PiSCSI/piscsi/wiki/Dayna-Port-Command-Set                                  */
/* Adaptec ASPI SDK: https://tinkerdifferent.com/threads/adaptec-aspi-sdk-dos-windows-3-x-16bit-scsi-development.3466/  */
/* PC/TCP Packet Driver Specification: https://web.archive.org/web/20221127060523/http://crynwr.com/packet_driver.html  */
/* DaynaPORT BlueSCSI Code: https://github.com/BlueSCSI/BlueSCSI-v2/blob/main/lib/SCSI2SD/src/firmware/network.c        */

#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <malloc.h>

/* Packet constants */
#define MAX_PACKET_SIZE 1522   /* Max Ethernet packet size (incl. Header, CRC and VLAN tags) */
#define CDB_LEN            6   /* The length of the CDB packet */
#define SENSE_LEN         14   /* The length of the sense packet */

/* SCSI command opcodes*/
#define CMD_READ_PACKET      0x08
#define CMD_RETRIEVE_STATUS  0x09
#define CMD_WRITE_PACKET     0x0A
#define CMD_MULTICAST_ENABLE 0x0D
#define CMD_ENABLE_IF        0x0E
#define CMD_INQUIRY          0x12

/* SRB Direction flags */
#define SRB_DIR_IN  0x08   /* Transfer from SCSI target to host */
#define SRB_DIR_OUT 0x10   /* Transfer from host to SCSI target */

/* SRB Execute command, the only one we use */
#define SRB_EXECUTE_SCSI_IO_CMD 0x2

/* ASPI SRB Status Flags */
#define SS_PENDING         0x00   /* SCSI Request in Progress             */
#define SS_COMP            0x01   /* SCSI Request Completed Without Error */
#define SS_ABORTED         0x02   /* SCSI Request Aborted by Host         */
#define SS_ERR             0x04   /* SCSI Request Completed With Error    */
#define SS_INVALID_CMD     0x80   /* Invalid SCSI Request                 */
#define SS_INVALID_HA      0x81   /* Invalid Host Adapter Number          */
#define SS_NO_DEVICE       0x82   /* SCSI Device Not Installed            */

/* Packet driver function codes */
#define DRIVER_INFO     0x1
#define ACCESS_TYPE     0x2
#define RELEASE_TYPE    0x3
#define SEND_PKT        0x4
#define TERMINATE       0x5
#define GET_ADDRESS     0x6
#define RESET_INTERFACE 0x7

/* The max cycles we will wait for a good command from a SCSI device */
#define MAX_POLLING_WAIT_CYCLES 5

/* How long we should delay in our polling cycle while we wait for a good command from a SCSI device */
#define POLLING_WAIT_DELAY 5

/* The number of DOS timer polling ticks to wait before looking for a new incoming packet */
/* Less ticks = more frequent checks                                                      */
#define DOS_TIMER_POLLING_TICKS 1

/* A pointer to the "old timer handler" for DOS timer polling */
void interrupt (*old_timer_handler)();

/* The polling tick counter*/
int polling_tick_counter = DOS_TIMER_POLLING_TICKS;

/* Packet driver header (placed at the start of the driver as required by the specification) */
typedef struct PacketDriverHeader {
    unsigned char jmp_instruction[3]; /* JMP to entry point (3 bytes) */
    char signature[9];               /* "PKT DRVR" signature */
    unsigned char another_jump[5];
};

/* Offset Structure for 6-byte CDBs        */
/* Shamelessly stolen from the Adaptec SDK */
#define BYTE unsigned char
#define WORD unsigned short

typedef struct {
    BYTE              SRB_Cmd;                /* 00/000 ASPI command code = SC_EXEC_SCSI_CMD */
    BYTE              SRB_Status;             /* 01/001 ASPI command status byte */
    BYTE              SRB_HaId;               /* 02/002 ASPI host adapter number */
    BYTE              SRB_Flags;              /* 03/003 ASPI request flags */
    BYTE              SRB_Hdr_Rsvd[4];        /* 04/004 Reserved, MUST = 0 */
    BYTE              SRB_Target;             /* 08/008 Target's SCSI ID */
    BYTE              SRB_Lun;                /* 09/009 Target's LUN number */
    WORD              SRB_BufLenLo;           /* 0A/010 Data Allocation Length */
    WORD              SRB_BufLenHi;           /* 0C/010 Data Allocation Length */
    BYTE              SRB_SenseLen;           /* 0E/014 Sense Allocation Length */
    WORD              SRB_BufOffset;          /* 0F/015 Data Buffer Pointer */
    WORD              SRB_BufSegment;         /* 11/015 Data Buffer Pointer */
    WORD              SRB_NextOffset;         /* 13/026 Post routine */
    WORD              SRB_NextSegment;        /* 15/026 Post routine */
    BYTE              SRB_CDBLen;             /* 17/023 CDB Length = 6 */
    BYTE              SRB_HaStat;             /* 18/024 Host Adapter Status */
    BYTE              SRB_TargStat;           /* 19/025 Target Status */
    WORD              SRB_PostOffset;         /* 1A/025 Target Status */
    WORD              SRB_PostSegment;        /* 1C/025 Target Status */
    BYTE              SRB_Rsvd2[34];          /* 1E/030 Reserved, MUST = 0 */
    BYTE              CDBByte[CDB_LEN];       /* 40 SCSI CDB */
    BYTE              SenseArea6[SENSE_LEN];  /* 46 Request Sense buffer */
} ASPI_SRB;


/*********************
 * Global variables  *
 *********************/

/* ASPI entry point */
far (* aspi_entry_point)(unsigned short, unsigned short) = 0;

/* Packet driver handle that we will call back when we have a packet*/
far (*driver_handle)() =  0;

/* Whether an interrupt is being processed or not */
unsigned char interrupt_service_in_process = 0;

/* Packet receive buffer */
unsigned char recv_buffer[MAX_PACKET_SIZE + 10];

/* Packet send buffer */
unsigned char snd_bfr[MAX_PACKET_SIZE + 8];

/* The packet driver header, which is required per the specification */
struct PacketDriverHeader packet_driver_header = {
    { 0xEB, 0x0A, 0x90 },                        /* JMP short to next jump, plus a NOP */
    { 'P', 'K', 'T', ' ', 'D', 'R', 'V', 'R', 0 },
    { 0xEA, 0x00, 0x00, 0x00, 0x00}              /* JMP far will be set later */
};

/* The name of the driver */
const char *driver_name = "RTC DaynaPort";

/* Software interrupt for packet driver */
unsigned char interrupt_vector = 0;

/* SCSI ID for DaynaPORT */
unsigned char scsi_id = 0;

/* SCSI Adapter Number */
unsigned char adapter_id = 0;

/* Function Declarations */
int scsi_command(char command, ASPI_SRB *srb);
int enable_interface(int enable);
int get_mac_address(unsigned char far *result, unsigned short length);
int init_driver();
int send_packet(unsigned char *buffer, unsigned short length);
int receive_packet(unsigned char far *buffer, unsigned short *length);
void terminate_driver();
void interrupt packet_driver_isr();
void interrupt polling_dayanport();
void print_usage_and_exit();
void main(int argc, char *argv[]);

/* Send SCSI command via ASPI */
int scsi_command(char command, ASPI_SRB *srb) {
    unsigned short segmt = FP_SEG(srb);
    unsigned short ofst = FP_OFF(srb);
    int poll_counter;
    srb->SRB_Target = scsi_id;
    srb->SRB_Status = 0;
    srb->SRB_SenseLen = SENSE_LEN; /* Request up to SENSE_LEN bytes of sense data */

    if (command == CMD_INQUIRY || command == CMD_RETRIEVE_STATUS) {
        srb->SRB_Cmd = SRB_EXECUTE_SCSI_IO_CMD;
        srb->SRB_CDBLen = CDB_LEN;
        srb->CDBByte[0] = command;
        srb->CDBByte[4] = srb->SRB_BufLenLo; /* Allocation length */
        (*aspi_entry_point)(ofst,segmt);
    } else if (command == CMD_ENABLE_IF) {
        srb->SRB_Cmd = SRB_EXECUTE_SCSI_IO_CMD;
        srb->SRB_CDBLen = CDB_LEN;
        _fmemcpy(srb->CDBByte, MK_FP(srb->SRB_BufSegment, srb->SRB_BufOffset), 6); /* Copy enable/disable command */
        (*aspi_entry_point)(ofst,segmt);
    } else if (command == CMD_READ_PACKET || command == CMD_WRITE_PACKET) {
        srb->SRB_Cmd = SRB_EXECUTE_SCSI_IO_CMD;
        srb->SRB_CDBLen = CDB_LEN;
        srb->CDBByte[0] = command;
        srb->CDBByte[3] = (srb->SRB_BufLenLo >> 8) & 0xFF;
        srb->CDBByte[4] = srb->SRB_BufLenLo & 0xFF;
        srb->CDBByte[5] = 0x80;
        (*aspi_entry_point)(ofst,segmt);
    }

    poll_counter = MAX_POLLING_WAIT_CYCLES;
    do {
        delay(POLLING_WAIT_DELAY);
        poll_counter--;
    } while (srb->SRB_Status == SS_PENDING && poll_counter > 0);

    /* For now, we will ignore errors, since legit sent packets come back with an error */
    /* We will also move on if in the pending status, which seems to be the case with received packets */
    if (srb->SRB_Status != SS_PENDING && srb->SRB_Status != SS_COMP && srb->SRB_Status != SS_ERR ) {
        return -1;
    }
    return 0;
}

/* Enable or Disable the DaynaPORT Interface */
int enable_interface(int enable) {
    ASPI_SRB srb;
    unsigned char cmd_enable[6] = {CMD_ENABLE_IF, 0, 0, 0, 0, 0x80};

    if (!enable) {
        cmd_enable[5] = 0x00;
    }

    /* Enable DaynaPORT interface */
    memset(&srb, 0, sizeof(srb));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_IN;
    srb.SRB_BufOffset = FP_OFF(cmd_enable);
    srb.SRB_BufSegment = FP_SEG(cmd_enable);
    srb.SRB_BufLenLo = sizeof(cmd_enable);
    if (scsi_command(CMD_ENABLE_IF, &srb)) {
        return -1;
    } else {
        /* Need to sleep for at least half a second per docs */
        /* TODO: Consider letting the caller handle the sleep, otherwise the caller will pause */
        delay(500);
    }
    return 0;
}

/* Get the DaynaPORT MAC Address */
/* Returns the length of the MAC address */
int get_mac_address(unsigned char far *result, unsigned short length) {
    ASPI_SRB srb;
    unsigned char buffer[18];

    memset(&srb, 0, sizeof(ASPI_SRB));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_IN;
    srb.SRB_BufLenLo = sizeof(buffer);
    srb.SRB_BufSegment = FP_SEG(buffer);
    srb.SRB_BufOffset = FP_OFF(buffer);
    if (scsi_command(CMD_RETRIEVE_STATUS, &srb)) {
        return 0;
    }
    memcpy(result,buffer,length);
    return length;
}

/* Initialize the driver */
int init_driver() {
    ASPI_SRB srb;
    char scsi_manager[10] = "SCSIMGR$";
    unsigned char inquiry_data[36];
    unsigned int found;
    unsigned long offset;
    unsigned char handle;
    union REGS inregs, outregs;
    struct SREGS segregs;

    /* Go looking for the string "SCSIMGR$" */
    inregs.x.ax = 0x3D00;
    inregs.x.dx = FP_OFF(scsi_manager);
    segregs.ds  = FP_SEG(scsi_manager);
    int86x( 0x21, &inregs, &outregs, &segregs);
    found = !outregs.x.cflag;

    /* If SCSIMGR$ found, get the entry point for it. */
    /* 4 bytes, since it is segment and offset */
    if(found) {
        handle = outregs.x.ax;
        inregs.x.bx = handle;
        inregs.x.ax = 0x4402;
        inregs.x.dx = FP_OFF(&aspi_entry_point);
        segregs.ds  = FP_SEG(&aspi_entry_point);
        inregs.x.cx = 4;
        int86x( 0x21, &inregs, &outregs, &segregs);

        // Close the SCSI Manager handle
        // that we used to find the segment and offset
        inregs.x.bx = handle;
        inregs.x.ax = 0x3E00;
        int86x( 0x21, &inregs, &outregs, &segregs);
    }

    if (!FP_SEG(aspi_entry_point)) {
        printf("ASPI entry point not found, did you load SCSI drivers in CONFIG.SYS?\n");
        return -1;
    }

    /* Send SCSI Inquiry to confirm DaynaPORT */
    memset(&srb, 0, sizeof(ASPI_SRB));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_IN;
    srb.SRB_BufLenLo = sizeof(inquiry_data);
    memset(inquiry_data, 0, sizeof(inquiry_data));
    srb.SRB_BufOffset = FP_OFF(inquiry_data);
    srb.SRB_BufSegment = FP_SEG(inquiry_data);
    if (scsi_command(CMD_INQUIRY, &srb)) {
       printf("Inquiry failed\n");
       return -2;
    }
    if (strncmp((char*)inquiry_data + 8, "Dayna", 5) != 0) {
        printf("DaynaPORT not found\n");
        return -3;
    }

    if (enable_interface(1)) {
        return -4;
    }

    /* Configure a "long jump" so that we can hit the packet_driver_isr code */
    /* This is very unorthodox, but that's what you get when you write a     */
    /* packet driver in C :)                                                 */
    offset = (unsigned long)packet_driver_isr;
    packet_driver_header.another_jump[1] = (unsigned char)((offset & 0xFF));
    packet_driver_header.another_jump[2] = (unsigned char)((offset >> 8) & 0xFF);
    packet_driver_header.another_jump[3] = (unsigned char)((offset >> 16) & 0xFF);
    packet_driver_header.another_jump[4] = (unsigned char)((offset >> 24) & 0xFF);

    /* Set up interrupt vector */
    setvect(interrupt_vector, (void interrupt (*)(int))&packet_driver_header);
    printf("Driver initialized\n");
    return 0;
}

/* Send a packet */
int send_packet(unsigned char *buffer, unsigned short length) {
    ASPI_SRB srb;

    if (length > MAX_PACKET_SIZE) return -1;

    memset(snd_bfr, 0, length + 8);
    memcpy(&snd_bfr[4], buffer, length);

    /* We have to inject the length, in big endian byte order */
    /* to the front of the packet */
    snd_bfr[0] = (length >> 8) & 0xFF;
    snd_bfr[1] = length & 0xFF;

    memset(&srb, 0, sizeof(ASPI_SRB));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_OUT;
    srb.SRB_BufLenLo = length + 8;
    srb.SRB_BufSegment = FP_SEG(snd_bfr);
    srb.SRB_BufOffset = FP_OFF(snd_bfr);
    return scsi_command(CMD_WRITE_PACKET, &srb);
}

/* Receive a packet.  */
/* Return back the location in the buffer where you should start reading data */
/* Return -1 if there is a lost packet */
/* Return -2 if there is no data to read */
int receive_packet(unsigned char far *buffer, unsigned short *length) {
    ASPI_SRB srb;
    unsigned long flags;

    memset(&srb, 0, sizeof(srb));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_IN;
    srb.SRB_BufLenLo = MAX_PACKET_SIZE + 10;
    srb.SRB_BufSegment = FP_SEG(buffer);
    srb.SRB_BufOffset = FP_OFF(buffer);
    if (scsi_command(CMD_READ_PACKET, &srb)) return -1;

    *length = (buffer[0] << 8) | buffer[1];
    memcpy(&flags, buffer + 2, 4);
    if (flags == 0xFFFFFFFF) {
        return -1;
    }
    /* We might be double counting here with the CRC, but allow for 4 bytes */
    if (*length > MAX_PACKET_SIZE + 4) return -3;

    if (*length == 0) {
      return -2;
    }

    /* We do not want to send back the 4-byte CRC, so discard it */
    *length = *length - 4;

    /* Skip past the first 6 bytes which are the length and flags */
    /* This is a cheaper operation than doing a memcpy to another buffer! */
    return 6;
}

/* Terminate the driver */
/* TODO: currently unused and untested */
void terminate_driver() {
    ASPI_SRB srb;
    unsigned char cmd_disable[6] = {CMD_ENABLE_IF, 0, 0, 0, 0, 0};

    /* Disable DaynaPORT interface */
    memset(&srb, 0, sizeof(srb));
    srb.SRB_HaId = adapter_id;
    srb.SRB_Flags = SRB_DIR_IN;
    srb.SRB_BufSegment = FP_SEG(cmd_disable);
    srb.SRB_BufOffset = FP_OFF(cmd_disable);
    srb.SRB_BufLenLo = sizeof(cmd_disable);
    scsi_command(CMD_ENABLE_IF, &srb);

    /* Free resources */
    setvect(interrupt_vector, 0); /* Clear interrupt vector */

    /* Unlink the clock vector too */
    /* TODO: this may clobber any handlers that came along later. If so, fix. */
    _dos_setvect(0x1C, old_timer_handler);
}

/* Packet driver interrupt service routine */
void interrupt packet_driver_isr() {

    unsigned char function = _AH;
    unsigned short DS;

    /* BAD HACK: Borland's ISR code is going to "stomp" on our DS register                                   */
    /* So, we are going to go back in the stack and grab it, which is PRETTY NONDETERMINISTIC.               */
    /* We're better off converting this entire ISR to assembly, but this works at least                      */
    /* We will clobber AX in the process, but we already saved off AH above, and that is all we need from AX */
    asm {
      push bp
      mov bp,sp
      mov ax, word ptr 0xc[bp]
      pop bp
    }
    DS = _AX;

    interrupt_service_in_process = 1;

    /* Don't do anything here that is not re-entrant or you will be sorry.                   */
    /* We use "raw register values.                                                          */
    /* If you do something like call a "printf" here, you'll for sure clobber your registers */

    /* BAD HACK ALERT: When the ISR finishes, the registers will "pop".  So we need to travel */
    /* back in the stack and "fix" the values with the return values we want.                  */

    /* TODO: eventually we might want to expand to support more than class 1 support */
    switch (function) {
        case DRIVER_INFO:
            /* Return driver info (class=1 for Ethernet, type=unknown, number=1) */
            _BX = 1;
            _CH = 1;
            _DX = 0xFF;
            _CL = 0;
            _AL = 1;

            asm {
                push bp
                mov bp,sp
                mov word ptr 0x16[bp], ax
                mov word ptr 0x14[bp], bx
                mov word ptr 0x12[bp], cx
                mov word ptr 0x10[bp], dx
                pop bp
            }

            _AX = FP_SEG(driver_name);
            _BX = FP_OFF(driver_name);

            asm {
                push bp
                mov bp,sp
                mov word ptr 0x0c[bp], ax
                mov word ptr 0x0a[bp], bx
                pop bp
                clc
            }
            break;
        case ACCESS_TYPE:
            /* Simplified: accept any type and we will only save off one handle */
            /* TODO: Create a handle table, and only process the types requested */
            driver_handle=(MK_FP(_ES,_DI));
            _AX = 0;
            asm {
                push bp
                mov bp,sp
                mov word ptr 0x16[bp], ax
                pop bp
                clc
            }
            break;
        case RELEASE_TYPE:
            driver_handle = 0;
            asm { clc }
            break;
        case SEND_PKT:
            send_packet(MK_FP(DS, _SI), _CX);
            asm { clc }
            break;
        case GET_ADDRESS:
            _CX = get_mac_address(MK_FP(_ES,_DI), _CX);
            asm {
                push bp
                mov bp,sp
                mov word ptr 0x12[bp], cx
                pop bp
                clc
            }
            break;
        case TERMINATE:
            /* TODO: we should probably do something here */
            asm { clc }
            break;
        case RESET_INTERFACE:
            enable_interface(0);
            enable_interface(1);
            asm { clc }
            break;
        default:
            _AX = 0xFF;
            asm {
                push bp
                mov bp,sp
                mov word ptr 0x16[bp], ax
                pop bp
                clc
            }
    }
    interrupt_service_in_process = 0;
}

/* Interrupt handler to poll for new packets! */
void interrupt polling_dayanport() {
    int position;
    unsigned short length;

    polling_tick_counter --;
    if (polling_tick_counter <=0) {
        polling_tick_counter = DOS_TIMER_POLLING_TICKS;

        if (interrupt_service_in_process == 0 ) {
            /* TODO: this call will block on I/O, which will starve the interrupt handling. Make this call multi-part. */
            position = receive_packet(recv_buffer, &length);
            if (position > 0) {
                if (driver_handle != 0 && length > 0) {
                    _AX = 0;
                    _CX = length;
                    _BX = 0;
                    (*driver_handle)();
                    if (_ES !=0 || _DI != 0) {
                        memcpy(MK_FP(_ES,_DI),recv_buffer + position, length);
                        _AX = 1;
                        _BX = 0;
                        (*driver_handle)();
                    }
                }
            } else if (position == -1) {
                /* Per the spec, if we get back a lost packet result, we should re-enable the interface */
                enable_interface(0);
                enable_interface(1);
            }
        }
    }
    _chain_intr(old_timer_handler);
}

/* Prints program usage and... well... exits! I know, right? */
void print_usage_and_exit() {
    printf("\nUsage: dayna.exe vector scsi_id <adapter_id>\n");
    printf("Example dayna.exe 0x60 4\n");
    exit(1);
}

/* Main function */
void main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage_and_exit();
    }

    interrupt_vector = (unsigned char) strtol(argv[1], NULL, 16);
    if (interrupt_vector < 0x60 || interrupt_vector > 0x80) {
        print_usage_and_exit();
    }

    scsi_id = atoi(argv[2]);

    if (argc > 3) {
        adapter_id = atoi(argv[3]);
    }

    printf("DaynaPORT SCSI/Link Packet Driver\n");
    if (init_driver() != 0) {
        printf("Initialization failed\n");
        return;
    }

    printf("Driver installed at 0x%02X for SCSI ID %d and Adapter ID %d\n",
        interrupt_vector, scsi_id, adapter_id);

    /* Set up polling for packet receive, we'll hook the DOS timer software interrupt */
    old_timer_handler = getvect(0x1C);
    setvect(0x1C, polling_dayanport);

    /* TODO: find real size that we need, this is probably overkill */
    keep(0, (_SS + (_SP/16) - _psp));
}