/***********************************************************************************************************************
PicoMite MMBasic

FileIO.c

<COPYRIGHT HOLDERS>  Geoff Graham, Peter Mather
Copyright (c) 2021, <COPYRIGHT HOLDERS> All rights reserved. 
Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met: 
1.	Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2.	Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the distribution.
3.	The name MMBasic be used when referring to the interpreter in any documentation and promotional material and the original copyright message be displayed 
    on the console at startup (additional copyright messages may be added).
4.	All advertising materials mentioning features or use of this software must display the following acknowledgement: This product includes software developed 
    by the <copyright holder>.
5.	Neither the name of the <copyright holder> nor the names of its contributors may be used to endorse or promote products derived from this software 
    without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDERS> AS IS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDERS> BE LIABLE FOR ANY DIRECT, 
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 

************************************************************************************************************************/#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "ff.h"
#include "diskio.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "pico/binary_info.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/clocks.h"

extern const uint8_t *flash_target_contents;
extern const uint8_t *flash_option_contents;
extern const uint8_t *SavedVarsFlash;
struct option_s Option;
int dirflags;
int GPSfnbr=0;

extern struct s_vartbl {                               // structure of the variable table
	unsigned char name[MAXVARLEN];                       // variable's name
	unsigned char type;                                  // its type (T_NUM, T_INT or T_STR)
	unsigned char level;                                 // its subroutine or function level (used to track local variables)
    unsigned char size;                         // the number of chars to allocate for each element in a string array
    unsigned char dummy;
    int __attribute__ ((aligned (4))) dims[MAXDIM];                     // the dimensions. it is an array if the first dimension is NOT zero
    union u_val{
        MMFLOAT f;                              // the value if it is a float
        long long int i;                        // the value if it is an integer
        MMFLOAT *fa;                            // pointer to the allocated memory if it is an array of floats
        long long int *ia;                      // pointer to the allocated memory if it is an array of integers
        unsigned char *s;                                // pointer to the allocated memory if it is a string
    }  __attribute__ ((aligned (8))) val;
} __attribute__ ((aligned (8))) s_vartbl_val;
volatile union u_flash {
  uint64_t i64[32];
  uint8_t  i8[256];
  uint32_t  i32[64];
} FlashWord;
volatile int i8p=0;
volatile uint32_t realflashpointer;
int FlashLoad=0;
unsigned char *CFunctionFlash = NULL;
unsigned char *CFunctionLibrary = NULL;
void RestoreProg(void);
#define SDbufferSize 512
static char *SDbuffer[MAXOPENFILES+1]={NULL};
int buffpointer[MAXOPENFILES+1]={0};
static uint32_t lastfptr[MAXOPENFILES+1]={[0 ... MAXOPENFILES ] = -1};
uint32_t fmode[MAXOPENFILES+1]={0};
static unsigned int bw[MAXOPENFILES+1]={[0 ... MAXOPENFILES ] = -1};
extern BYTE BMP_bDecode(int x, int y, int fnbr);
#define RoundUp(a)     (((a) + (sizeof(int) - 1)) & (~(sizeof(int) - 1)))// round up to the nearest integer size      [position 27:9]	
/*****************************************************************************************
Mapping of errors reported by the file system to MMBasic file errors
*****************************************************************************************/
const int ErrorMap[] = {        0,                                  // 0
                                1,                                  // Assertion failed
                                2,                                  // Low level I/O error
                                3,                                  // No response from SDcard
                                4,                                  // Could not find the file
                                5,                                  // Could not find the path
                                6,                                  // The path name format is invalid
                                7,                                  // Prohibited access or directory full
                                8,                                  // Directory exists or path to it cannot be found
                                9,                                  // The file/directory object is invalid
                               10,                                  // SD card is write protected
                               11,                                  // The logical drive number is invalid
                               12,                                  // The volume has no work area
                               13,                                  // Not a FAT volume
                               14,                                  // Format aborted
                               15,                                  // Could not access volume
                               16,                                  // File sharing policy
                               17,                                  // Buffer could not be allocated
                               18,                                  // Too many open files
                               19,                                  // Parameter is invalid
							   20									// Not present
                            };

/******************************************************************************************
Text for the file related error messages reported by MMBasic
******************************************************************************************/
const char *FErrorMsg[] = {	"Succeeded ",
		"A hard error occurred in the low level disk I/O layer",
		"Assertion failed",
		"SD Card not found",
		"Could not find the file",
		"Could not find the path",
		"The path name format is invalid",
		"FAccess denied due to prohibited access or directory full",
		"Access denied due to prohibited access",
		"The file/directory object is invalid",
		"The physical drive is write protected",
		"The logical drive number is invalid",
		"The volume has no work area",
		"There is no valid FAT volume",
		"The f_mkfs() aborted due to any problem",
		"Could not get a grant to access the volume within defined period",
		"The operation is rejected according to the file sharing policy",
		"LFN working buffer could not be allocated",
		"Number of open files > FF_FS_LOCK",
		"Given parameter is invalid",
		"SD card not present"
};
extern BYTE MDD_SDSPI_CardDetectState(void);
extern void InitReservedIO(void) ;
void ForceFileClose(int fnbr) ;
FRESULT FSerror;
FATFS FatFs;
union uFileTable {
    unsigned int com;
    FIL *fptr;
};
union uFileTable FileTable[MAXOPENFILES + 1];
volatile BYTE SDCardStat = STA_NOINIT | STA_NODISK;
int OptionFileErrorAbort = true;
bool irqs[32];
void disable_interrupts(void){
    int i;
    for(i=0;i<31;i++){
        irqs[i]=irq_is_enabled(i);
       if(irqs[i]){
           irq_set_enabled(i,false);
       }
     }
}
void enable_interrupts(void){
    int i;
    for(i=0;i<31;i++){
       if(irqs[i])irq_set_enabled(i,true);
    }
}
void ErrorThrow(int e) {
    MMerrno = e;
    FSerror = e;
    strcpy(MMErrMsg, (char *)FErrorMsg[e]);
    if(e && OptionFileErrorAbort) error(MMErrMsg);
    return;
}

void cmd_flash(void){
    char *p;
    if((p = checkstring(cmdline, "ERASE ALL"))) {
        uint32_t j=FLASH_TARGET_OFFSET  + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE;
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE*MAXFLASHSLOTS);
        enable_interrupts();
/*    } else if((p = checkstring(cmdline, "ERASE BACKUP"))) {
        if(CurrentLinePtr) error("Invalid in program");
        uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (MAXFLASHSLOTS * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();*/
    } else if((p = checkstring(cmdline, "ERASE"))) {
        if(CurrentLinePtr) error("Invalid in program");
        int i=getint(p,1,MAXFLASHSLOTS);
        uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i-1) * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
/*    } else if((p = checkstring(cmdline, "RESTORE BACKUP"))) {
        if(CurrentLinePtr) error("Invalid in program");
        RestoreProg();*/
    } else if((p = checkstring(cmdline, "OVERWRITE"))) {
        if(CurrentLinePtr) error("Invalid in program");
        int i=getint(p,1,MAXFLASHSLOTS);
        uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i-1) * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
        j=(Option.PROG_FLASH_SIZE>>2);
        uSec(250000);
        int *pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
        while(j--)if(*pp++ != 0xFFFFFFFF)error("Already programmed");
        disable_interrupts();
        flash_range_program(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i-1) * MAX_PROG_SIZE), ProgMemory, Option.PROG_FLASH_SIZE);
        enable_interrupts();
    } else if((p = checkstring(cmdline, "LIST"))) {
        int j,i,k;
        int *pp;
        for(i=1;i<=10;i++){
            k=0;
            j=Option.PROG_FLASH_SIZE>>2;
            pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
            while(j--)if(*pp++ != 0xFFFFFFFF){
                char buff[STRINGSIZE]={0};
                MMPrintString("Slot ");
                PInt(i);
                MMPrintString(" in use");
                pp--;
                if((unsigned char)*pp == T_NEWLINE){
                    MMPrintString(": \"");
                    llist(buff, (unsigned char *)pp); 
                    MMPrintString(buff);
                    MMPrintString("\"\r\n");
                } else MMPrintString("\r\n");
                k=1;
                break;
            }
            if(k==0){
                MMPrintString("Slot ");
                PInt(i);
                MMPrintString(" available\r\n");
            }
        } 
    } else if((p = checkstring(cmdline, "SAVE"))) {
        int j=(Option.PROG_FLASH_SIZE>>2),i=getint(p,1,MAXFLASHSLOTS);
        uSec(250000);
        int *pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
        while(j--)if(*pp++ != 0xFFFFFFFF)error("Already programmed");
        disable_interrupts();
        flash_range_program(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((i-1) * MAX_PROG_SIZE), ProgMemory, Option.PROG_FLASH_SIZE);
        enable_interrupts();
    } else if((p = checkstring(cmdline, "LOAD"))) {
        int j=(Option.PROG_FLASH_SIZE>>2),i=getint(p,1,MAXFLASHSLOTS);
        int *pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
        int *qq=(int *)ProgMemory;
        while(j--)*qq++ = *pp++;
        FlashLoad=i;
        SaveProg();
    } else if((p = checkstring(cmdline, "CHAIN"))) {
        if(!CurrentLinePtr) error("Invalid at command prompt");
        int j=(Option.PROG_FLASH_SIZE>>2),i=getint(p,1,MAXFLASHSLOTS);
        int *pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
        int *qq=(int *)ProgMemory;
        while(j--)*qq++ = *pp++;
        FlashLoad=i;
	    PrepareProgram(true);
        nextstmt = (unsigned char *)ProgMemory;
    } else if((p = checkstring(cmdline, "RUN"))) {
        int j=(Option.PROG_FLASH_SIZE>>2),i=getint(p,1,MAXFLASHSLOTS);
        int *pp=(int *)(flash_target_contents+(i-1)*MAX_PROG_SIZE);
        int *qq=(int *)ProgMemory;
        while(j--)*qq++ = *pp++;
        FlashLoad=i;
        SaveProg();
    	ClearRuntime();
	    PrepareProgram(true);
        nextstmt = (unsigned char *)ProgMemory;
    } else error("Syntax");
}

void ErrorCheck(int fnbr) {                                         //checks for an error, if fnbr is specified frees up the filehandle before sending error
    int e;
    e = (int)FSerror;
    if(fnbr != 0 && e != 0) ForceFileClose(fnbr);
    if(e >= 1 && e <= 19) ErrorThrow(ErrorMap[e]);
    return;
}
char *GetCWD(void) {
    char *b;
    b = GetTempMemory(STRINGSIZE);
    if(!InitSDCard()) return b;
    FSerror = f_getcwd(b, STRINGSIZE);
    ErrorCheck(0);
    b[0] = b[0] - '0' + 'A';
    return b;
}
void LoadImage(unsigned char *p) {
	int fnbr;
	int xOrigin, yOrigin;

	// get the command line arguments
	getargs(&p, 5, ",");                                            // this MUST be the first executable line in the function
    if(argc == 0) error("Argument count");
    if(!InitSDCard()) return;

    p = getCstring(argv[0]);                                        // get the file name

    xOrigin = yOrigin = 0;
	if(argc >= 3) xOrigin = getinteger(argv[2]);                    // get the x origin (optional) argument
	if(argc == 5) yOrigin = getinteger(argv[4]);                    // get the y origin (optional) argument

	// open the file
	if(strchr(p, '.') == NULL) strcat(p, ".BMP");
	fnbr = FindFreeFileNbr();
    if(!BasicFileOpen(p, fnbr, FA_READ)) return;
    BMP_bDecode(xOrigin, yOrigin, fnbr);
    FileClose(fnbr);
}

// search for a volume label, directory or file
// s$ = DIR$(fspec, DIR|FILE|ALL)       will return the first entry
// s$ = DIR$()                          will return the next
// If s$ is empty then no (more) files found
void fun_dir(void) {
    static DIR djd;
    unsigned char *p;
    static FILINFO fnod;
    static char pp[STRINGSIZE];
    getargs(&ep, 3, ",");
    if(argc != 0) dirflags = 0;
    if(!(argc == 0 || argc == 3)) error("Syntax");

    if(argc == 3) {
        if(checkstring(argv[2], "DIR"))
            dirflags = AM_DIR;
        else if(checkstring(argv[2], "FILE"))
            dirflags = -1;
        else if(checkstring(argv[2], "ALL"))
            dirflags = 0;
        else
            error("Invalid flag specification");
    }


    if(argc != 0) {
        // this must be the first call eg:  DIR$("*.*", FILE)
        p = getCstring(argv[0]);
        strcpy(pp,p);
        djd.pat = pp;
        if(!InitSDCard()) return;                                   // setup the SD card
        FSerror = f_opendir(&djd, "");
        ErrorCheck(0);
    }
        if(SDCardStat & STA_NOINIT){
           f_closedir(&djd);
            error("SD card not found");
        }
        if(dirflags == AM_DIR){
            for (;;) {
                FSerror = f_readdir(&djd, &fnod);                   // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0]) break;      // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && (fnod.fattrib & AM_DIR) && !(fnod.fattrib & AM_SYS)) break;     // Test for the file name
            }
        }
        else if(dirflags == -1){
            for (;;) {
                FSerror = f_readdir(&djd, &fnod);                   // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0]) break;      // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && !(fnod.fattrib & AM_DIR)&& !(fnod.fattrib & AM_SYS)) break;     // Test for the file name
            }
        }
        else {
            for (;;) {
                FSerror = f_readdir(&djd, &fnod);                   // Get a directory item
                if (FSerror != FR_OK || !fnod.fname[0]) break;      // Terminate if any error or end of directory
                if (pattern_matching(pp, fnod.fname, 0, 0) && !(fnod.fattrib & AM_SYS)) break;        // Test for the file name
            }
        }

    if (FSerror != FR_OK || !fnod.fname[0])f_closedir(&djd);
    sret = GetTempMemory(STRINGSIZE);                                      // this will last for the life of the command
    strcpy(sret, fnod.fname);
    CtoM(sret);                                                     // convert to a MMBasic style string
    targ = T_STR;
}


void cmd_mkdir(void) {
    unsigned char *p;
    p = getCstring(cmdline);                                        // get the directory name and convert to a standard C string
    if(p[1] == ':') *p = toupper(*p) - 'A' + '0';                   // convert a DOS style disk name to FatFs device number
    if(!InitSDCard()) return;
    FSerror = f_mkdir(p);
    ErrorCheck(0);
}



void cmd_rmdir(void){
    unsigned char *p;
    p = getCstring(cmdline);                                        // get the directory name and convert to a standard C string
    if(p[1] == ':') *p = toupper(*p) - 'A' + '0';                   // convert a DOS style disk name to FatFs device number
    if(!InitSDCard()) return;
    FSerror = f_unlink(p);
    ErrorCheck(0);
}



void cmd_chdir(void){
    unsigned char *p;
    p = getCstring(cmdline);                                        // get the directory name and convert to a standard C string
    if(p[1] == ':') *p = toupper(*p) - 'A' + '0';                   // convert a DOS style disk name to FatFs device number
    if(!InitSDCard()) return;
    FSerror = f_chdir(p);
    ErrorCheck(0);
}



void fun_cwd(void) {
    sret = CtoM(GetCWD());
    targ = T_STR;
}



void cmd_kill(void){
    char *p;
    p = getCstring(cmdline);                                        // get the file name
    if(p[1] == ':') *p = toupper(*p) - 'A' + '0';                   // convert a DOS style disk name to FatFs device number
    if(!InitSDCard()) return;
    FSerror = f_unlink(p);
    ErrorCheck(0);
}



void cmd_seek(void) {
    int fnbr, idx;
    char *buff;
    getargs(&cmdline, 5, ",");
    if(argc != 3) error("Syntax");
    if(*argv[0] == '#') argv[0]++;
    fnbr = getinteger(argv[0]);
    if(fnbr < 1 || fnbr > MAXOPENFILES || FileTable[fnbr].com <= MAXCOMPORTS) error("File number");
    if(FileTable[fnbr].com == 0) error("File number #% is not open", fnbr);
    if(!InitSDCard()) return;
    idx = getinteger(argv[2]) - 1;
    if(idx < 0) idx = 0;
    if(fmode[fnbr] & FA_WRITE){
        FSerror = f_lseek(FileTable[fnbr].fptr,idx);
        ErrorCheck(fnbr);
    } else {
    	buff=SDbuffer[fnbr];
    	FSerror = f_lseek(FileTable[fnbr].fptr,idx - (idx % 512));
    	ErrorCheck(fnbr);
    	FSerror = f_read(FileTable[fnbr].fptr, buff,SDbufferSize, &bw[fnbr]);
    	ErrorCheck(fnbr);
    	buffpointer[fnbr]=idx % 512;
    	lastfptr[fnbr]=(uint32_t)FileTable[fnbr].fptr;
    }
}


void cmd_name(void) {
    char *old, *new, ss[2];
    ss[0] = tokenAS;                                                // this will be used to split up the argument line
    ss[1] = 0;
    {                                                               // start a new block
        getargs(&cmdline, 3, ss);                                   // getargs macro must be the first executable stmt in a block
        if(argc != 3) error("Syntax");
        old = getCstring(argv[0]);                                  // get the old name
        if(old[1] == ':') *old = toupper(*old) - 'A' + '0';         // convert a DOS style disk name to FatFs device number
        new = getCstring(argv[2]);                                  // get the new name
        if(new[1] == ':') *new = toupper(*new) - 'A' + '0';         // convert a DOS style disk name to FatFs device number
        if(!InitSDCard()) return;
        FSerror = f_rename(old, new);
        ErrorCheck(0);
    }
}

void cmd_save(void) {
    int fnbr;
	unsigned char *pp, *flinebuf, *p;                                    // get the file name and change to the directory
	int maxH=VRes;
    int maxW=HRes;
     if(!InitSDCard()) return;
    fnbr = FindFreeFileNbr();
    if((p = checkstring(cmdline, "IMAGE")) !=NULL){
        unsigned int nbr;
        int i, x,y,w,h, filesize;
        unsigned char bmpfileheader[14] = {'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0};
        unsigned char bmpinfoheader[40] = {40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0};
        unsigned char bmppad[3] = {0,0,0};
    	getargs(&p,9,",");
        if(!InitSDCard()) return;
        if((void *)ReadBuffer == (void *)DisplayNotSet) error("SAVE IMAGE not available on this display");
        pp = getCstring(argv[0]);
        if(argc!=1 && argc!=9)error("Syntax");
        if(strchr(pp, '.') == NULL) strcat(pp, ".BMP");
        if(!BasicFileOpen(pp, fnbr, FA_WRITE | FA_CREATE_ALWAYS)) return;
        if(argc==1){
        	x=0; y=0; h=maxH; w=maxW;
        } else {
        	x=getint(argv[2],0,maxW-1);
        	y=getint(argv[4],0,maxH-1);
        	w=getint(argv[6],1,maxW-x);
        	h=getint(argv[8],1,maxH-y);
        }
        filesize=54 + 3*w*h;
        bmpfileheader[ 2] = (unsigned char)(filesize    );
        bmpfileheader[ 3] = (unsigned char)(filesize>> 8);
        bmpfileheader[ 4] = (unsigned char)(filesize>>16);
        bmpfileheader[ 5] = (unsigned char)(filesize>>24);

        bmpinfoheader[ 4] = (unsigned char)(       w    );
        bmpinfoheader[ 5] = (unsigned char)(       w>> 8);
        bmpinfoheader[ 6] = (unsigned char)(       w>>16);
        bmpinfoheader[ 7] = (unsigned char)(       w>>24);
        bmpinfoheader[ 8] = (unsigned char)(       h    );
        bmpinfoheader[ 9] = (unsigned char)(       h>> 8);
        bmpinfoheader[10] = (unsigned char)(       h>>16);
        bmpinfoheader[11] = (unsigned char)(       h>>24);
		f_write(FileTable[fnbr].fptr, bmpfileheader, 14, &nbr);
		f_write(FileTable[fnbr].fptr, bmpinfoheader, 40, &nbr);
        flinebuf = GetTempMemory(maxW * 4);
        for(i = y+h-1; i >= y; i--){
           ReadBuffer(x, i, x+w-1, i, flinebuf);
           f_write(FileTable[fnbr].fptr, flinebuf, w*3, &nbr);
           if((w*3) % 4 !=0) f_write(FileTable[fnbr].fptr, bmppad, 4-((w*3) % 4) , &nbr);
        }
        FileClose(fnbr);
        return;
    }  else {
	    unsigned char b[STRINGSIZE];
        p = getCstring(cmdline);                           // get the file name and change to the directory
        if(strchr(p, '.') == NULL) strcat(p, ".BAS");
        if(!BasicFileOpen(p, fnbr, FA_WRITE | FA_CREATE_ALWAYS)) return;
        p  = ProgMemory;
        while(!(*p == 0 || *p == 0xff)) {                           // this is a safety precaution
            p = llist(b, p);                                        // expand the line
            pp = b;
            while(*pp) FilePutChar(*pp++, fnbr);                    // write the line to the SD card
            FilePutChar('\r', fnbr); FilePutChar('\n', fnbr);       // terminate the line
            if(p[0] == 0 && p[1] == 0) break;                       // end of the listing ?
        }
        FileClose(fnbr);
    }
}
// load a file into program memory
int FileLoadProgram(unsigned char *fname) {
    int fnbr;
    char *p, *buf;
    int c;

    if(!InitSDCard()) return false;
    ClearProgram();                                                 // clear any leftovers from the previous program
    fnbr = FindFreeFileNbr();
    p = getCstring(fname);
    if(strchr(p, '.') == NULL) strcat(p, ".BAS");
    if(!BasicFileOpen(p, fnbr, FA_READ)) return false;
    p = buf = GetTempMemory(EDIT_BUFFER_SIZE - 256*5);              // get all the memory while leaving space for the couple of buffers defined and the file handle
    while(!FileEOF(fnbr)) {                                         // while waiting for the end of file
        if((p - buf) >= EDIT_BUFFER_SIZE - 512) error("Not enough memory");
        c = FileGetChar(fnbr) & 0x7f;
        if(isprint(c) || c == '\r' || c == '\n' || c == TAB) {
            if(c == TAB) c = ' ';
            *p++ = c;                                               // get the input into RAM
        }
    }
    *p = 0;                                                         // terminate the string in RAM
    FileClose(fnbr);
    ClearSavedVars();                                               // clear any saved variables
    SaveProgramToMemory(buf, false);
    return true;
}


void cmd_load(void) {
    int autorun = false;
    unsigned char *p;

    p = checkstring(cmdline, "IMAGE");
    if(p) {
        LoadImage(p);
        return;
    }

    getargs(&cmdline, 3, ",");
    if(!(argc & 1) || argc == 0) error("Syntax");
    if(argc == 3) {
        if(toupper(*argv[2]) == 'R')
            autorun = true;
        else
            error("Syntax");
    } else if(CurrentLinePtr != NULL)
        error("Invalid in a program");

    if(!FileLoadProgram(argv[0])) return;
	FlashLoad=0;
    if(autorun) {
        if(*ProgMemory != 0x01) return;                              // no program to run
        ClearRuntime();
        WatchdogSet = false;
        PrepareProgram(true);
        IgnorePIN = false;
        nextstmt = ProgMemory;
    }
}


/*void cmd_load(void) {
    if(CurrentLinePtr) error("Invalid in a program");
    strcpy((char *)LastFile, (char *)GetFileName(cmdline, NULL));	                // get the file name and save into LastFile
    if(*LastFile == 0) error("Cannot find file");
	if(strchr((char *)LastFile, '.') == NULL) strcat((char *)LastFile, ".BAS");
	ClearProgram();								                    // clear the program space
	mergefile(LastFile, ProgMemory);						                // load
	ProgramChanged = true;
	longjmp(mark, 1);							                    // jump back to the input prompt
}*/

char fullpathname[FF_MAX_LFN];
char fullfilepathname[FF_MAX_LFN];
typedef struct sa_dlist {
    char from[STRINGSIZE];
    char to[STRINGSIZE];
} a_dlist;
a_dlist *dlist;


char __not_in_flash_func(FileGetChar)(int fnbr) {
    char ch;
    char *buff=SDbuffer[fnbr];
;
    if(!InitSDCard()) return 0;
    if(fmode[fnbr] & FA_WRITE){
        FSerror = f_read(FileTable[fnbr].fptr, &ch,1, &bw[fnbr]);
        ErrorCheck(fnbr);
    } else {
    	if(!(lastfptr[fnbr]==(uint32_t)FileTable[fnbr].fptr && buffpointer[fnbr]<SDbufferSize)){
    		FSerror = f_read(FileTable[fnbr].fptr, buff, SDbufferSize, &bw[fnbr]);
    		ErrorCheck(fnbr);
    		buffpointer[fnbr]=0;
    		lastfptr[fnbr]=(uint32_t)FileTable[fnbr].fptr;
    	}
    	ch=buff[buffpointer[fnbr]];
    	buffpointer[fnbr]++;
    }
    diskchecktimer=DISKCHECKRATE;
    return ch;
}

char __not_in_flash_func(FilePutChar)(char c, int fnbr) {
    static char t;
    unsigned int bw;
    t = c;
    if(!InitSDCard()) return 0;
    FSerror = f_write(FileTable[fnbr].fptr, &t, 1, &bw);
    lastfptr[fnbr]=-1; //invalidate the read file buffer
    ErrorCheck(fnbr);
    diskchecktimer=DISKCHECKRATE;
    return t;
}
int FileEOF(int fnbr) {
    int i;
    if(!InitSDCard()) return 0;
    if(buffpointer[fnbr]<=bw[fnbr]-1) i=0;
    else {
    	i = f_eof(FileTable[fnbr].fptr);
    }
    return i;
}
// send a character to a file or the console
// if fnbr == 0 then send the char to the console
// otherwise the COM port or file opened as #fnbr
unsigned char __not_in_flash_func(MMfputc)(unsigned char c, int fnbr) {
  if(fnbr == 0) return MMputchar(c,1);                                // accessing the console
    if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if(FileTable[fnbr].com == 0) error("File number is not open");
    if(FileTable[fnbr].com > MAXCOMPORTS)
        return FilePutChar(c, fnbr);
    else 
        return SerialPutchar(FileTable[fnbr].com, c);                   // send the char to the serial port
}
int __not_in_flash_func(MMfgetc)(int fnbr) {
  int ch;
  if(fnbr == 0) return MMgetchar();                                 // accessing the console
    if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if(FileTable[fnbr].com == 0) error("File number is not open");
    if(FileTable[fnbr].com > MAXCOMPORTS)
        ch = FileGetChar(fnbr);
    else 
        ch = SerialGetchar(FileTable[fnbr].com);                        // get the char from the serial port
  return ch;
}
void MMfopen(unsigned char *fname, unsigned char *mode, int fnbr){
    
}
int MMfeof(int fnbr) {
  if(fnbr == 0) return (kbhitConsole() == 0);                       // accessing the console
    if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if(FileTable[fnbr].com == 0) error("File number is not open");
    if(FileTable[fnbr].com > MAXCOMPORTS)
        return FileEOF(fnbr);
    else 
        return SerialRxStatus(FileTable[fnbr].com) == 0;
}
//close the file and free up the file handle
// it will generate an error if needed
void FileClose(int fnbr) {
    ForceFileClose(fnbr);
    ErrorThrow(FSerror);
}


//close the file and free up the file handle
// it will NOT generate an error
void ForceFileClose(int fnbr) {
    if(fnbr && FileTable[fnbr].fptr != NULL){
        FSerror = f_close(FileTable[fnbr].fptr);
        FreeMemory((void *)FileTable[fnbr].fptr);
        FreeMemory((void *)SDbuffer[fnbr]);
        FileTable[fnbr].fptr = NULL;
        buffpointer[fnbr]=0;
        lastfptr[fnbr]=-1;
        bw[fnbr]=-1;
        fmode[fnbr]=0;
    }
}
// finds the first available free file number.  Throws an error if no free file numbers
int FindFreeFileNbr(void) {
    int i;
    for(i = 1; i <= MAXOPENFILES; i++)
        if(FileTable[i].com == 0) return i;
    error("Too many files open");
    return 0;
}

void CloseAllFiles(void) {
  int i;
  for(i = 1; i <= MAXOPENFILES; i++) {
        if(FileTable[i].com != 0) {
            if(FileTable[i].com > MAXCOMPORTS) {
                ForceFileClose(i);
            } else
                SerialClose(FileTable[i].com);
        FileTable[i].com = 0;
        }
  }
}

void FilePutStr(int count, char *c, int fnbr){
    unsigned int bw;
    InitSDCard();
    FSerror = f_write(FileTable[fnbr].fptr, c, count, &bw);
    ErrorCheck(fnbr);
    diskchecktimer=DISKCHECKRATE;
}



// output a string to a file
// the string must be a MMBasic string
void MMfputs(unsigned char *p, int filenbr) {
	int i;
	i = *p++;
    if(FileTable[filenbr].com > MAXCOMPORTS){
    	FilePutStr(i, p, filenbr);
    } else {
    	while(i--) MMfputc(*p++, filenbr);
    }
}


// this is invoked as a command (ie, date$ = "6/7/2010")
// search through the line looking for the equals sign and step over it,
// evaluate the rest of the command, split it up and save in the system counters
int InitSDCard(void) {
    int i;
    ErrorThrow(0);    // reset mm.errno to zero
    if((IsInvalidPin(Option.SD_CS) || (IsInvalidPin(Option.SYSTEM_MOSI) && IsInvalidPin(Option.SD_MOSI_PIN)) || (IsInvalidPin(Option.SYSTEM_MISO) &&  IsInvalidPin(Option.SD_MISO_PIN)) || (IsInvalidPin(Option.SYSTEM_CLK) && IsInvalidPin(Option.SD_CLK_PIN))))error("SDcard not configured");
    if(!(SDCardStat & STA_NOINIT)) return 1;  // if the card is present and has been initialised we have nothing to do
    for(i = 0; i < MAXOPENFILES; i++)
        if(FileTable[i].com > MAXCOMPORTS)
            if(FileTable[i].fptr != NULL)
                ForceFileClose(i);
    i = f_mount(&FatFs, "", 1);
    if(i) { ErrorThrow(ErrorMap[i]); return false; }
    return 2;
}

// this performs the basic duties of opening a file, all file opens in MMBasic should use this
// it will open the file, set the FileTable[] entry and populate the file descriptor
// it returns with true if successful or false if an error
int BasicFileOpen(char *fname, int fnbr, int mode) {
    if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if(FileTable[fnbr].com != 0) error("File number already open");
    if(!InitSDCard()) return false;
    // if we are writing check the write protect pin (negative pin number means that low = write protect)
        FileTable[fnbr].fptr = GetMemory(sizeof(FIL));              // allocate the file descriptor
        SDbuffer[fnbr]=GetMemory(SDbufferSize);
        if(fname[1] == ':') *fname = toupper(*fname) - 'A' + '0';   // convert a DOS style disk name to FatFs device number
        FSerror = f_open(FileTable[fnbr].fptr, fname, mode);        // open it
        ErrorCheck(fnbr);
        buffpointer[fnbr]=0;
        lastfptr[fnbr]=-1;
        bw[fnbr]=-1;
        fmode[fnbr]=mode;

    if(FSerror) {
        ForceFileClose(fnbr);
        return false;
    } else
        return true;
}

#define MAXFILES 200
typedef struct ss_flist {
    char fn[FF_MAX_LFN];
    int fs;
} s_flist;

int strcicmp(char const *a, char const *b)
{
    for (;; a++, b++) {
        int d = tolower(*a) - tolower(*b);
        if (d != 0 || !*a)
            return d;
    }
}
void cmd_copy(void){
	char ss[2];														// this will be used to split up the argument line
	char *fromfile, *tofile;
	ss[0] = tokenTO;
	ss[1] = 0;
	char buff[512];
	unsigned int nbr=0, bw;
	int fnbr1, fnbr2;
	getargs(&cmdline,3,ss);
	if(argc!=3)error("Syntax");
    fnbr1 = FindFreeFileNbr();
	fromfile = getCstring(argv[0]);
	BasicFileOpen(fromfile, fnbr1, FA_READ);
    fnbr2 = FindFreeFileNbr();
	tofile = getCstring(argv[2]);
    if(!BasicFileOpen(tofile, fnbr2, FA_WRITE | FA_CREATE_ALWAYS)) {
    	FileClose(fnbr1);
    }
    while(!f_eof(FileTable[fnbr1].fptr)) {
		FSerror = f_read(FileTable[fnbr1].fptr, buff,512, &nbr);
		ErrorCheck(fnbr1);
	    FSerror = f_write(FileTable[fnbr2].fptr, buff, nbr, &bw);
	    ErrorCheck(fnbr2);
    }
	FileClose(fnbr1);
	FileClose(fnbr2);
}

void cmd_files(void) {
  int i, dirs, ListCnt;
  char *p, *q;
  int fcnt;
  char ts[STRINGSIZE] = "";
  s_flist *flist;
    static DIR djd;
    static FILINFO fnod;
  if(CurrentLinePtr) error("Invalid in a program");
//    OptionFileErrorAbort = 0;
    fcnt = 0;
   if(*cmdline)
      p = getCstring(cmdline);
    else
      p = "*";

    if(!InitSDCard()) error((char *)FErrorMsg[1]);                  // setup the SD card
    flist=GetMemory(sizeof(s_flist)*MAXFILES);
     // print the current directory
    q = GetCWD();
    MMPrintString(&q[1]); MMPrintString("\r\n");

    // search for the first file/dir
    FSerror = f_findfirst(&djd, &fnod, "", p);
    ErrorCheck(0);
    // add the file to the list, search for the next and keep looping until no more files
    while(FSerror == FR_OK && fnod.fname[0]) {
        if(fcnt >= MAXFILES) {
                error("Too many files to list");
        }
        if(!(fnod.fattrib & (AM_SYS | AM_HID))){
            // add a prefix to each line so that directories will sort ahead of files
            if(fnod.fattrib & AM_DIR)
                ts[0] = 'D';
            else
                ts[0] = 'F';

            // and concatenate the filename found
            strcpy(&ts[1], fnod.fname);

            // sort the file name into place in the array
            for(i = fcnt; i > 0; i--) {
                if( strcicmp((flist[i - 1].fn), (ts)) > 0)
                    flist[i] = flist[i - 1];
                else
                    break;
            }
            strcpy(flist[i].fn, ts);
            flist[i].fs = fnod.fsize;
            fcnt++;
        }
        FSerror = f_findnext(&djd, &fnod);
   }

    // list the files with a pause every screen full
  ListCnt = 2;
  for(i = dirs = 0; i < fcnt; i++) {
      if(flist[i].fn[0] == 'D') {
          dirs++;
            MMPrintString("   <DIR>  ");
      }
      else {
            IntToStrPad(ts, flist[i].fs, ' ', 10, 10); MMPrintString(ts);
            MMPrintString("  ");
        }
        MMPrintString(flist[i].fn + 1);
      MMPrintString("\r\n");
      // check if it is more than a screen full
      if(++ListCnt >= 24 && i < fcnt) {
          MMPrintString("PRESS ANY KEY ...");
          MMgetchar();
          MMPrintString("\r                 \r");
          ListCnt = 1;
      }
  }
    // display the summary
    IntToStr(ts, dirs, 10); MMPrintString(ts);
    MMPrintString(" director"); MMPrintString(dirs==1?"y, ":"ies, ");
    IntToStr(ts, fcnt - dirs, 10); MMPrintString(ts);
    MMPrintString(" file"); MMPrintString((fcnt-dirs)==1?"":"s");
    MMPrintString("\r\n");
    FreeMemory((void *)(char *)flist);
    f_closedir(&djd);
    memset(inpbuf,0,STRINGSIZE);
    longjmp(mark, 1);                                                 // jump back to the input prompt
}
// remove unnecessary text
void CrunchData(unsigned char **p, int c) {
    static unsigned char inquotes, lastch, incomment;

    if(c == '\n') c = '\r';                                         // CR is the end of line terminator
    if(c == 0  || c == '\r' ) {
        inquotes = false; incomment = false;                        // newline so reset our flags
        if(c) {
            if(lastch == '\r') return;                              // remove two newlines in a row (ie, empty lines)
            *((*p)++) = '\r';
        }
        lastch = '\r';
        return;
    }
        
    if(incomment) return;                                           // discard comments
    if(c == ' ' && lastch == '\r') return;                          // trim all spaces at the start of the line
    if(c == '"') inquotes = !inquotes;
    if(inquotes) {
        *((*p)++) = c;                                              // copy everything within quotes
        return;
    }
    if(c == '\'') {                                                 // skip everything following a comment
        incomment = true;
        return;
    }
    if(c == ' ' && (lastch == ' ' || lastch == ',')) {
        lastch = ' ';
        return;                                                     // remove more than one space or a space after a comma
    }
    *((*p)++) = lastch = c;
}
void cmd_autosave(void) {
    unsigned char *buf, *p;
    int c, prevc = 0, crunch = false;
    int count=0;
    uint64_t timeout;
    if(CurrentLinePtr) error("Invalid in a program");
    if(*cmdline) {
        if(toupper(*cmdline) == 'C') 
            crunch = true;
        else
            error("Unknown command");
    }
    
    ClearProgram();                                                 // clear any leftovers from the previous program
    p = buf = GetMemory(EDIT_BUFFER_SIZE);
    CrunchData(&p, 0);                                              // initialise the crunch data subroutine
    while((c = (getConsole() & 0x7f)) != 0x1a) {                    // while waiting for the end of text char
        if(c==127 && count && time_us_64()-timeout>100000) {fflush(stdout);count=0;}
        if(p == buf && c == '\n') continue;                         // throw away an initial line feed which can follow the command
        if((p - buf) >= EDIT_BUFFER_SIZE) error("Not enough memory");
        if(isprint(c) || c == '\r' || c == '\n' || c == TAB) {
            if(c == TAB) c = ' ';
            if(crunch)
                CrunchData(&p, c);                                  // insert into RAM after throwing away comments. etc
            else
                *p++ = c;                                           // insert the input into RAM
            {
                if(!(c == '\n' && prevc == '\r')) {MMputchar(c,0); count++; timeout=time_us_64();}    // and echo it
                if(c == '\r') {MMputchar('\n',1); count=0;}
            }
            prevc = c;
        }
    }
    fflush(stdout);

    *p = 0;                                                         // terminate the string in RAM
    while(getConsole() != -1);                                      // clear any rubbish in the input
//    ClearSavedVars();                                               // clear any saved variables
    SaveProgramToMemory(buf, true);
    FreeMemory(buf);
}

void FileOpen(char *fname, char *fmode, char *ffnbr) {
    int fnbr;
    BYTE mode = 0;
    if(str_equal(fmode, "OUTPUT"))
        mode = FA_WRITE | FA_CREATE_ALWAYS;
    else if(str_equal(fmode, "APPEND"))
        mode = FA_WRITE | FA_OPEN_APPEND;
    else if(str_equal(fmode, "INPUT"))
        mode = FA_READ;
    else if(str_equal(fmode, "RANDOM"))
        mode = FA_WRITE | FA_OPEN_APPEND | FA_READ;
    else
        error("File access mode");

    if(*ffnbr == '#') ffnbr++;
    fnbr = getinteger(ffnbr);
    BasicFileOpen(fname, fnbr, mode);
}

void cmd_open(void) {
    int fnbr;
    char *fname;
    char ss[4];                                                       // this will be used to split up the argument line

    ss[0] = tokenAS;
    ss[1] = tokenFOR;
    ss[2] = ',';
    ss[3] = 0;
    {                                                                 // start a new block
		getargs(&cmdline, 7, ss);									// getargs macro must be the first executable stmt in a block
        if(!(argc == 3 || argc == 5 || argc == 7)) error("Syntax");
        fname = getCstring(argv[0]);

      // check that it is a serial port that we are opening
        if(argc == 5 && !(mem_equal(fname, "COM1:", 5) || mem_equal(fname, "COM2:", 5))) {
            FileOpen(fname, argv[2], argv[4]);
            diskchecktimer=DISKCHECKRATE;
            return;
        }
      if(!(mem_equal(fname, "COM1:", 5) || mem_equal(fname, "COM2:", 5) ))  error("Invalid COM port");
        if((*argv[2] == 'G') || (*argv[2] == 'g')){
            MMFLOAT timeadjust=0.0;
            argv[2]++;
            if(!((*argv[2] == 'P') || (*argv[2] == 'p')))error("Syntax");
            argv[2]++;
            if(!((*argv[2] == 'S') || (*argv[2] == 's')))error("Syntax");
            if(argc >= 5)timeadjust=getnumber(argv[4]);
            if(timeadjust<-12.0 || timeadjust>14.0)error("Invalid Time Offset");
            gpsmonitor=0;
            if(argc==7)gpsmonitor=getint(argv[6],0,1);
            GPSadjust=(int)(timeadjust*3600.0);
		// check that it is a serial port that we are opening
            SerialOpen(fname);
            fnbr = FindFreeFileNbr();
            GPSfnbr=fnbr;
            FileTable[fnbr].com = fname[3] - '0';
            if(mem_equal(fname, "COM1:", 5))GPSchannel=1;
            if(mem_equal(fname, "COM2:", 5))GPSchannel=2;
            gpsbuf=gpsbuf1;
            gpscurrent=0;
            gpscount=0;
        } else {
            if(*argv[2] == '#') argv[2]++;
            fnbr = getint(argv[2], 1, MAXOPENFILES);
            if(FileTable[fnbr].com != 0) error("Already open");
            SerialOpen(fname);
            FileTable[fnbr].com = fname[3] - '0';
        }
      }
}

void fun_inputstr(void) {
  int i, nbr, fnbr;
  getargs(&ep, 3, ",");
  if(argc != 3) error("Syntax");
  sret = GetTempMemory(STRINGSIZE);                                        // this will last for the life of the command
  nbr = getint(argv[0], 1, MAXSTRLEN);
  if(*argv[2] == '#') argv[2]++;
  fnbr = getinteger(argv[2]);
    if(fnbr == 0) {                                                 // accessing the console
        for(i = 1; i <= nbr && kbhitConsole(); i++)
            sret[i] = getConsole();                                 // get the char from the console input buffer and save in our returned string
    } else {
        if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
        if(FileTable[fnbr].com == 0) error("File number is not open");
        targ = T_STR;
        if(FileTable[fnbr].com > MAXCOMPORTS) {
            for(i = 1; i <= nbr && !MMfeof(fnbr); i++)
                sret[i] = FileGetChar(fnbr);                        // get the char from the SD card and save in our returned string
            *sret = i - 1;                                          // update the length of the string
            return;                                                 // all done so skip the rest
        }
        for(i = 1; i <= nbr && SerialRxStatus(FileTable[fnbr].com); i++)
            sret[i] = SerialGetchar(FileTable[fnbr].com);           // get the char from the serial input buffer and save in our returned string
    }
    *sret = i - 1;
}



void fun_eof(void) {
    int fnbr;
    getargs(&ep, 1, ",");
    if(argc == 0) error("Syntax");
    if(*argv[0] == '#') argv[0]++;
    fnbr = getinteger(argv[0]);
    iret = MMfeof(fnbr);
    targ = T_INT;
}


void fun_loc(void) {
  int fnbr;
  getargs(&ep, 1, ",");
  if(argc == 0) error("Syntax");
  if(*argv[0] == '#') argv[0]++;
  fnbr = getinteger(argv[0]);
    if(fnbr == 0)                                                   // accessing the console
        iret = kbhitConsole();
    else {
        if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
        if(FileTable[fnbr].com == 0) error("File number is not open");
        if(FileTable[fnbr].com > MAXCOMPORTS) {
            iret = (*(FileTable[fnbr].fptr)).fptr + 1;
        } else
        iret = SerialRxStatus(FileTable[fnbr].com);
    }
    targ = T_INT;
}


void fun_lof(void) {
  int fnbr;
  getargs(&ep, 1, ",");
  if(argc == 0) error("Syntax");
  if(*argv[0] == '#') argv[0]++;
  fnbr = getinteger(argv[0]);
    if(fnbr == 0)                                                   // accessing the console
        iret = 0;
    else {
        if(fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
        if(FileTable[fnbr].com == 0) error("File number is not open");
        if(FileTable[fnbr].com > MAXCOMPORTS) {
            iret = f_size(FileTable[fnbr].fptr);
        } else
            iret = (TX_BUFFER_SIZE - SerialTxStatus(FileTable[fnbr].com));
    }
    targ = T_INT;
}




void cmd_close(void) {
  int i, fnbr;
  getargs(&cmdline, (MAX_ARG_COUNT * 2) - 1, ",");                 // getargs macro must be the first executable stmt in a block
  if((argc & 0x01) == 0) error("Syntax");
  for(i = 0; i < argc; i += 2) {
        if((*argv[i] == 'G') || (*argv[i] == 'g')){
            argv[i]++;
            if(!((*argv[i] == 'P') || (*argv[i] == 'p')))error("Syntax");
            argv[i]++;
            if(!((*argv[i] == 'S') || (*argv[i] == 's')))error("Syntax");
            if(!GPSfnbr)error("Not open");
            SerialClose(FileTable[GPSfnbr].com);
            FileTable[GPSfnbr].com = 0;
            GPSfnbr=0;
            GPSchannel=0;
            GPSlatitude=0;
            GPSlongitude=0;
            GPSspeed=0;
            GPSvalid=0;
            GPStime[1]='0';GPStime[2]='0';GPStime[4]='0';GPStime[5]='0';GPStime[7]='0';GPStime[8]='0';
            GPSdate[1]='0';GPSdate[2]='0';GPSdate[4]='0';GPSdate[5]='0';GPSdate[9]='0';GPSdate[10]='0';
            GPStrack=0;
            GPSdop=0;
            GPSsatellites=0;
            GPSaltitude=0;
            GPSfix=0;
            GPSadjust=0;
            gpsmonitor=0;
        } else {
            if(*argv[i] == '#') argv[i]++;
            fnbr = getint(argv[i], 1, MAXOPENFILES);
            if(FileTable[fnbr].com == 0) error("File number is not open");
            while(SerialTxStatus(FileTable[fnbr].com) && !MMAbort);     // wait for anything in the buffer to be transmitted
            if(FileTable[fnbr].com > MAXCOMPORTS){
                FileClose(fnbr);
                diskchecktimer=DISKCHECKRATE;
            } else
                SerialClose(FileTable[fnbr].com);

            FileTable[fnbr].com = 0;
            }
  }
}
void CheckSDCard(void) {
    if(Option.SD_CS==0)return;
    if(CurrentlyPlaying == P_NOTHING){
        if(diskchecktimer== 0) {
            if(!(SDCardStat & STA_NOINIT)){ //the card is supposed to be initialised - lets check
                char buff[4];
                if (disk_ioctl(0, MMC_GET_OCR, buff) != RES_OK){
                    BYTE s;
                    s = SDCardStat;
                    s |= (STA_NODISK | STA_NOINIT);
                    SDCardStat = s;
                    MMPrintString("Warning: SDcard Removed\r\n");
                }
            }
            diskchecktimer=DISKCHECKRATE;
        }
    } else if(CurrentlyPlaying == P_WAV) checkWAVinput();
}
void LoadOptions(void) {
	int i=256;
    char *pp=(char *)flash_option_contents;
    char *qq=(char *)&Option;
    while(i--)*qq++ = *pp++;
}

void ResetAllFlash(void) {
    disable_sd();
//    disable_audio();
//    disable_touch();
//    disable_LCD();
    memset((void *)&Option.Autorun,0,512);
    Option.Autorun=0;
    Option.Height = SCREENHEIGHT;
    Option.Width = SCREENWIDTH;
    Option.Tab = 2;
    Option.DefaultFont = 0x01;
    Option.DefaultBrightness = 100;
    Option.MaxCtrls = 101;
//    Option.ProgFlashSize = Option.PROG_FLASH_SIZE;
    Option.DefaultFC = WHITE;
    Option.Baudrate = CONSOLE_BAUDRATE;
    Option.CPU_Speed=125000;
    Option.SD_CS=0;
    Option.SYSTEM_MOSI=0;
    Option.SYSTEM_MISO=0;
    Option.SYSTEM_CLK=0;
    Option.AUDIO_L=0;
    Option.AUDIO_R=0;
    Option.AUDIO_SLICE=99;
    Option.SDspeed=10;
    Option.DISPLAY_TYPE = 0;
    Option.DISPLAY_ORIENTATION = 0;
    Option.TOUCH_XSCALE = 0;
    Option.TOUCH_CS = 0;
    Option.TOUCH_IRQ = 0;
   	Option.LCD_CD = 0;
   	Option.LCD_Reset = 0;
   	Option.LCD_CS = 0;
    Option.DefaultFont = 0x01;
    Option.DefaultFC = WHITE;
    Option.DefaultBC = BLACK;
    Option.LCDVOP = 0xB1;
    Option.I2Coffset = 0;
    Option.E_INKbusy = 0;
    Option.Refresh = 0;
	Option.fullrefresh = 0;
    Option.SYSTEM_I2C_SCL = 0;
    Option.SYSTEM_I2C_SDA = 0;
    Option.RTC_Clock = 0;
    Option.RTC_Data = 0;
    Option.RTC=0;
    Option.PWM=0;
    Option.PROG_FLASH_SIZE=80*1024;
    Option.HEAP_SIZE=80*1024;
    Option.MaxCtrls=0;
    Option.INT1pin=9;
    Option.INT2pin=10;
    Option.INT3pin=11;
    Option.INT4pin=12;
    Option.SD_CLK_PIN=0;
    Option.SD_MOSI_PIN=0;
    Option.SD_MISO_PIN=0;
    memset(Option.F5key,0,sizeof(Option.F5key));
    memset(Option.F6key,0,sizeof(Option.F6key));
    memset(Option.F7key,0,sizeof(Option.F7key));
    memset(Option.F8key,0,sizeof(Option.F8key));
    memset(Option.F9key,0,sizeof(Option.F9key));
    SaveOptions();
    uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + ((10) * MAX_PROG_SIZE);
    uSec(250000);
    disable_interrupts();
    flash_range_erase(j, MAX_PROG_SIZE);
    enable_interrupts();
}
void FlashWriteBlock(void){
    int i;
    uint32_t address=realflashpointer-256;
    uint8_t *there = (uint8_t *)address+XIP_BASE;
    if(address % 256)error("Flash write address");
    for(i=0;i<256;i++){
    	if(there[i]!=0xFF) error("flash not erased");
    }
    disable_interrupts();
    flash_range_program(address , (const uint8_t *)&FlashWord.i8[0], 256);
    enable_interrupts();

	for(i=0;i<64;i++)FlashWord.i32[i]=0xFFFFFFFF;
}
// write a byte to flash
// this will buffer four bytes so that the write to flash can be a word
void FlashWriteByte(unsigned char b) {
	realflashpointer++;
	FlashWord.i8[i8p]=b;
	i8p++;
	i8p %= 256;
	if(i8p==0){
		FlashWriteBlock();
	}
}



// flush any bytes in the buffer to flash
void FlashWriteClose(void) {
	  while(i8p != 0) {
		  FlashWriteByte(0xff);
	  }
}


/*******************************************************************************************************************
 The variables are stored in a reserved flash area (which in total is 2K).
 The first few bytes are used for the options. So we must save the options in RAM before we erase, then write the
 options back.  The variables saved by this command are then written to flash starting just after the options.
********************************************************************************************************************/
void cmd_var(void) {
    unsigned char *p, *buf, *bufp, *varp, *vdata, lastc;
    int i, j, nbr = 1, nbr2=1, array, type, SaveDefaultType;
    int VarList[MAX_ARG_COUNT];
    unsigned char *VarDataList[MAX_ARG_COUNT];
    if((p = checkstring(cmdline, "CLEAR"))) {
        checkend(p);
        ClearSavedVars();
        return;
    }
    if((p = checkstring(cmdline, "RESTORE"))) {
        char b[MAXVARLEN + 3];
        checkend(p);
//        SavedVarsFlash = (char*)FLASH_SAVED_VAR_ADDR;      // point to where the variables were saved
        if(*SavedVarsFlash == 0xFF) return;                          // zero in this location means that nothing has ever been saved
        SaveDefaultType = DefaultType;                              // save the default type
        bufp = (unsigned char *)SavedVarsFlash;   // point to where the variables were saved
        while(*bufp != 0xff) {                                      // 0xff is the end of the variable list
            type = *bufp++;                                         // get the variable type
            array = type & 0x80;  type &= 0x7f;                     // set array to true if it is an array
            DefaultType = TypeMask(type);                           // and set the default type to this
            if(array) {
                strcpy(b, bufp);
                strcat(b, "()");
                vdata = findvar(b, type | V_EMPTY_OK | V_NOFIND_ERR);     // find an array
            } else
                vdata = findvar(bufp, type | V_FIND);               // find or create a non arrayed variable
            if(TypeMask(vartbl[VarIndex].type) != TypeMask(type)) error("$ type conflict", bufp);
            if(vartbl[VarIndex].type & T_CONST) error("$ is a constant", bufp);
            bufp += strlen((char *)bufp) + 1;                       // step over the name and the terminating zero byte
            if(array) {                                             // an array has the data size in the next two bytes
                nbr = *bufp++;
                nbr |= (*bufp++) << 8;
                nbr |= (*bufp++) << 16;
                nbr |= (*bufp++) << 24;
                nbr2 = 1;
                for(j = 0; vartbl[VarIndex].dims[j] != 0 && j < MAXDIM; j++)
                    nbr2 *= (vartbl[VarIndex].dims[j] + 1 - OptionBase);
                if(type & T_STR) nbr2 *= vartbl[VarIndex].size +1;
                if(type & T_NBR) nbr2 *= sizeof(MMFLOAT);
                if(type & T_INT) nbr2 *= sizeof(long long int);
                if(nbr2!=nbr)error("Array size");
            } else {
               if(type & T_STR) nbr = *bufp + 1;
               if(type & T_NBR) nbr = sizeof(MMFLOAT);
               if(type & T_INT) nbr = sizeof(long long int);
            }
            while(nbr--) *vdata++ = *bufp++;                        // copy the data
        }
        DefaultType = SaveDefaultType;
        return;
    }

     if((p = checkstring(cmdline, "SAVE"))) {
        getargs(&p, (MAX_ARG_COUNT * 2) - 1, ",");                  // getargs macro must be the first executable stmt in a block
        if(argc && (argc & 0x01) == 0) error("Invalid syntax");

        // befor we start, run through the arguments checking for errors
        // before we start, run through the arguments checking for errors
        for(i = 0; i < argc; i += 2) {
            checkend(skipvar(argv[i], false));
            VarDataList[i/2] = findvar(argv[i], V_NOFIND_ERR | V_EMPTY_OK);
            VarList[i/2] = VarIndex;
            if((vartbl[VarIndex].type & (T_CONST | T_PTR)) || vartbl[VarIndex].level != 0) error("Invalid variable");
            p = &argv[i][strlen(argv[i]) - 1];                      // pointer to the last char
            if(*p == ')') {                                         // strip off any empty brackets which indicate an array
                p--;
                if(*p == ' ') p--;
                if(*p == '(')
                    *p = 0;
                else
                    error("Invalid variable");
            }
        }
        // load the current variable save table into RAM
        // while doing this skip any variables that are in the argument list for this save
        bufp = buf = GetTempMemory(SAVEDVARS_FLASH_SIZE);           // build the saved variable table in RAM
//        SavedVarsFlash = (char*)FLASH_SAVED_VAR_ADDR;      // point to where the variables were saved
        varp = (unsigned char *)SavedVarsFlash;   // point to where the variables were saved
        while(*varp != 0 && *varp != 0xff) {            // 0xff is the end of the variable list, SavedVarsFlash[4] = 0 means that the flash has never been written to
            type = *varp++;                                         // get the variable type
            array = type & 0x80;  type &= 0x7f;                     // set array to true if it is an array
            vdata = varp;                                           // save a pointer to the name
            while(*varp) varp++;                                    // skip the name
            varp++;                                                 // and the terminating zero byte
            if(array) {                                             // an array has the data size in the next two bytes
                 nbr = (varp[0] | (varp[1] << 8) | (varp[2] << 16) | (varp[3] << 24)) + 4;
            } else {
                if(type & T_STR) nbr = *varp + 1;
                if(type & T_NBR) nbr = sizeof(MMFLOAT);
                if(type & T_INT) nbr = sizeof(long long int);
            }
            for(i = 0; i < argc; i += 2) {                          // scan the argument list
                p = &argv[i][strlen(argv[i]) - 1];                  // pointer to the last char
                lastc = *p;                                         // get the last char
                if(lastc <= '%') *p = 0;                            // remove the type suffix for the compare
                if(strncasecmp(vdata, argv[i], MAXVARLEN) == 0) {   // does the entry have the same name?
                    while(nbr--) varp++;                            // found matching variable, skip over the entry in flash (ie, do not copy to RAM)
                    i = 9999;                                       // force the termination of the for loop
                }
                *p = lastc;                                         // restore the type suffix
            }
            // finished scanning the argument list, did we find a matching variable?
            // if not, copy this entry to RAM
            if(i < 9999) {
                *bufp++ = type | array;
                while(*vdata) *bufp++ = *vdata++;                   // copy the name
                *bufp++ = *vdata++;                                 // and the terminating zero byte
                while(nbr--) *bufp++ = *varp++;                     // copy the data
            }
        }


        // initialise for writing to the flash
//        FlashWriteInit(SAVED_VARS_FLASH);
        ClearSavedVars();
        // now write the variables in RAM recovered from the var save list
        while(buf < bufp){
        	FlashWriteByte(*buf++);
        }
        // now save the variables listed in this invocation of VAR SAVE
        for(i = 0; i < argc; i += 2) {
            VarIndex = VarList[i/2];                                // previously saved index to the variable
            vdata = VarDataList[i/2];                               // pointer to the variable's data
            type = TypeMask(vartbl[VarIndex].type);                 // get the variable's type
            type |= (vartbl[VarIndex].type & T_IMPLIED);            // set the implied flag
            array = (vartbl[VarIndex].dims[0] != 0);

            nbr = 1;                                                // number of elements to save
            if(array) {                                             // if this is an array calculate the number of elements
                for(j = 0; vartbl[VarIndex].dims[j] != 0 && j < MAXDIM; j++)
                    nbr *= (vartbl[VarIndex].dims[j] + 1 - OptionBase);
                type |= 0x80;                                       // an array has the top bit set
            }

            if(type & T_STR) {
                if(array)
                    nbr *= (vartbl[VarIndex].size + 1);
                else
                    nbr = *vdata + 1;                               // for a simple string variable just save the string
            }
            if(type & T_NBR) nbr *= sizeof(MMFLOAT);
            if(type & T_INT) nbr *= sizeof(long long int);
            if((uint32_t)realflashpointer + XIP_BASE - (uint32_t)SavedVarsFlash + 36 + nbr > SAVEDVARS_FLASH_SIZE) {
                FlashWriteClose();
                error("Not enough memory");
            }
            FlashWriteByte(type);                              // save its type
            for(j = 0, p = vartbl[VarIndex].name; *p && j < MAXVARLEN; p++, j++) FlashWriteByte(*p);                            // save the name
            FlashWriteByte(0);                                 // terminate the name
            if(array) {                                             // if it is an array save the number of data bytes
               FlashWriteByte(nbr); FlashWriteByte(nbr >> 8); FlashWriteByte(nbr >>16); FlashWriteByte(nbr >>24);
            }
            while(nbr--) FlashWriteByte(*vdata++);             // write the data
        }
        FlashWriteClose();
        return;
     }
    error("Unknown command");
}


void ClearSavedVars(void) {
    uSec(250000);
    disable_interrupts();
    realflashpointer=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE;
    flash_range_erase(realflashpointer , SAVEDVARS_FLASH_SIZE);
    enable_interrupts();
}
void SaveOptions(void){
    uSec(100000);
    disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_ERASE_SIZE);
    enable_interrupts();
    uSec(10000);
    disable_interrupts();
    flash_range_program(FLASH_TARGET_OFFSET , (const uint8_t *)&Option, 512);
    enable_interrupts();
    uSec(10000);
 }
 void SaveProg(void){
        uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (MAXFLASHSLOTS * MAX_PROG_SIZE);
        uSec(250000);
        disable_interrupts();
        flash_range_erase(j, MAX_PROG_SIZE);
        enable_interrupts();
        j=(Option.PROG_FLASH_SIZE>>2);
        uSec(250000);
        int *pp=(int *)(flash_target_contents+MAXFLASHSLOTS*MAX_PROG_SIZE);
        while(j--)if(*pp++ != 0xFFFFFFFF)error("Flash erase problem");
        disable_interrupts();
        flash_range_program(FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (MAXFLASHSLOTS * MAX_PROG_SIZE), ProgMemory, Option.PROG_FLASH_SIZE);
        enable_interrupts();

 }
 void RestoreProg(void){
    int j=(Option.PROG_FLASH_SIZE>>2);
    int *pp=(int *)(flash_target_contents+MAXFLASHSLOTS*MAX_PROG_SIZE);
    char *p=(char *)pp;
    if(*p==1){
        int *qq=(int *)ProgMemory;
        while(j--)*qq++ = *pp++;
    } else {
        if(*p!=0xff){
            uint32_t j=FLASH_TARGET_OFFSET + FLASH_ERASE_SIZE + SAVEDVARS_FLASH_SIZE + (MAXFLASHSLOTS * MAX_PROG_SIZE);
            uSec(250000);
            disable_interrupts();
            flash_range_erase(j, MAX_PROG_SIZE);
            enable_interrupts();
        }
//        error("Nothing to restore");
    }
 }
