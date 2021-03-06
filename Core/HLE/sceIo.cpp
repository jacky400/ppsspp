// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#ifdef _WIN32
#include <windows.h>
#undef DeleteFile
#endif

#include "../System.h"
#include "../Config.h"
#include "HLE.h"
#include "../MIPS/MIPS.h"
#include "../HW/MemoryStick.h"

#include "../FileSystems/FileSystem.h"
#include "../FileSystems/MetaFileSystem.h"
#include "../FileSystems/ISOFileSystem.h"
#include "../FileSystems/DirectoryFileSystem.h"

#include "sceIo.h"
#include "sceRtc.h"
#include "sceKernel.h"
#include "sceKernelMemory.h"
#include "sceKernelThread.h"

#define ERROR_ERRNO_FILE_NOT_FOUND               0x80010002

#define ERROR_MEMSTICK_DEVCTL_BAD_PARAMS         0x80220081
#define ERROR_MEMSTICK_DEVCTL_TOO_MANY_CALLBACKS 0x80220082

/*

TODO: async io is missing features!

flash0: - fat access - system file volume
flash1: - fat access - configuration file volume
flashfat#: this too

lflash: - block access - entire flash
fatms: memstick
isofs: fat access - umd
disc0: fat access - umd
ms0: - fat access - memcard
umd: - block access - umd
irda?: - (?=0..9) block access - infra-red port (doesnt support seeking, maybe send/recieve data from port tho)
mscm0: - block access - memstick cm??
umd00: block access - umd
umd01: block access - umd
*/

#define O_RDONLY		0x0001
#define O_WRONLY		0x0002
#define O_RDWR			0x0003
#define O_NBLOCK		0x0010
#define O_APPEND		0x0100
#define O_CREAT		 0x0200
#define O_TRUNC		 0x0400
#define O_NOWAIT		0x8000


typedef s32 SceMode;
typedef s64 SceOff;
typedef u64 SceIores;

std::string emuDebugOutput;

const std::string &EmuDebugOutput() {
	return emuDebugOutput;
}

typedef u32 (*DeferredAction)(SceUID id, int param);
DeferredAction defAction = 0;
u32 defParam;

#define SCE_STM_FDIR 0x1000
#define SCE_STM_FREG 0x2000
#define SCE_STM_FLNK 0x4000
enum {
	TYPE_DIR=0x10,
	TYPE_FILE=0x20
};

#ifdef __SYMBIAN32__
#undef st_ctime
#undef st_atime
#undef st_mtime
#endif

struct SceIoStat {
	SceMode st_mode;
	unsigned int st_attr;
	SceOff st_size;
	ScePspDateTime st_ctime;
	ScePspDateTime st_atime;
	ScePspDateTime st_mtime;
	unsigned int st_private[6];
};

struct SceIoDirEnt {
	SceIoStat d_stat;
	char d_name[256];
	u32 d_private;
};
#ifndef __SYMBIAN32__
struct dirent {
	u32 unk0;
	u32 type;
	u32 size;
	u32 unk[19];
	char name[0x108];
};
#endif

class FileNode : public KernelObject {
public:
	FileNode() : callbackID(0), callbackArg(0), asyncResult(0), pendingAsyncResult(false), sectorBlockMode(false) {}
	~FileNode() {
		pspFileSystem.CloseFile(handle);
	}
	const char *GetName() {return fullpath.c_str();}
	const char *GetTypeName() {return "OpenFile";}
	void GetQuickInfo(char *ptr, int size) {
		sprintf(ptr, "Seekpos: %08x", (u32)pspFileSystem.GetSeekPos(handle));
	}
	static u32 GetMissingErrorCode() { return SCE_KERNEL_ERROR_BADF; }
	int GetIDType() const { return 0; }

	std::string fullpath;
	u32 handle;

	u32 callbackID;
	u32 callbackArg;

	u32 asyncResult;

	bool pendingAsyncResult;
	bool sectorBlockMode;
};

void __IoInit() {
	INFO_LOG(HLE, "Starting up I/O...");

#ifdef _WIN32

	char path_buffer[_MAX_PATH], drive[_MAX_DRIVE] ,dir[_MAX_DIR], file[_MAX_FNAME], ext[_MAX_EXT];
	char memstickpath[_MAX_PATH];
	char flashpath[_MAX_PATH];

	GetModuleFileName(NULL,path_buffer,sizeof(path_buffer));

	char *winpos = strstr(path_buffer, "Windows");
	if (winpos)
	*winpos = 0;
	strcat(path_buffer, "dummy.txt");

	_splitpath_s(path_buffer, drive, dir, file, ext );

	// Mount a couple of filesystems
	sprintf(memstickpath, "%s%sMemStick\\", drive, dir);
	sprintf(flashpath, "%s%sFlash\\", drive, dir);

#else
	// TODO
	std::string memstickpath = g_Config.memCardDirectory;
	std::string flashpath = g_Config.flashDirectory;
#endif

	DirectoryFileSystem *memstick;
	DirectoryFileSystem *flash;

	memstick = new DirectoryFileSystem(&pspFileSystem, memstickpath);
	flash = new DirectoryFileSystem(&pspFileSystem, flashpath);
	pspFileSystem.Mount("ms0:", memstick);
	pspFileSystem.Mount("fatms0:", memstick);
	pspFileSystem.Mount("fatms:", memstick);
	pspFileSystem.Mount("flash0:", flash);
	pspFileSystem.Mount("flash1:", flash);
}

void __IoShutdown() {

}

u32 sceIoAssign(const char *aliasname, const char *physname, const char *devname, u32 flag) {
	ERROR_LOG(HLE, "UNIMPL sceIoAssign(%s, %s, %s, %08x, ...)", aliasname,
			physname, devname, flag);
	return 0;
}

u32 sceKernelStdin() {
	DEBUG_LOG(HLE, "3=sceKernelStdin()");
	return 3;
}

u32 sceKernelStdout() {
	DEBUG_LOG(HLE, "1=sceKernelStdout()");
	return 1;
}

u32 sceKernelStderr() {
	DEBUG_LOG(HLE, "2=sceKernelStderr()");
	return 2;
}

void __IoCompleteAsyncIO(SceUID id) {
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		if (f->callbackID) {
			// __KernelNotifyCallbackType(THREAD_CALLBACK_IO, __KernelGetCurThread(), f->callbackID, f->callbackArg);
		}
	}
}

void __IoGetStat(SceIoStat *stat, PSPFileInfo &info) {
	memset(stat, 0xfe, sizeof(SceIoStat));
	stat->st_size = (s64) info.size;

	int type, attr;
	if (info.type & FILETYPE_DIRECTORY)
		type = SCE_STM_FDIR, attr = TYPE_DIR;
	else
		type = SCE_STM_FREG, attr = TYPE_FILE;

	stat->st_mode = type; //0777 | type;
	stat->st_attr = attr;
	stat->st_size = info.size;
	stat->st_private[0] = info.startSector;
}

u32 sceIoGetstat(const char *filename, u32 addr) {
	SceIoStat stat;
	PSPFileInfo info = pspFileSystem.GetFileInfo(filename);
	__IoGetStat(&stat, info);
	Memory::WriteStruct(addr, &stat);

	DEBUG_LOG(HLE, "sceIoGetstat(%s, %08x) : sector = %08x", filename, addr,
			info.startSector);

	return 0;
}

//Not sure about wrapping it or not, since the log seems to take the address of the data var
u32 sceIoRead(int id, u32 data_addr, int size) {
	if (id == 3) {
		DEBUG_LOG(HLE, "sceIoRead STDIN");
		return 0; //stdin
	}

	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		if (data_addr) {
			u8 *data = (u8*) Memory::GetPointer(data_addr);
			f->asyncResult = (u32) pspFileSystem.ReadFile(f->handle, data,
					size);
			DEBUG_LOG(HLE, "%i=sceIoRead(%d, %08x , %i)", f->asyncResult, id,
					data_addr, size);
			return f->asyncResult;
		} else {
			ERROR_LOG(HLE, "sceIoRead Reading into zero pointer");
			return -1;
		}
	} else {
		ERROR_LOG(HLE, "sceIoRead ERROR: no file open");
		return error;
	}
}

u32 sceIoWrite(int id, void *data_ptr, int size) //(int fd, void *data, int size);
{
	if (id == 2) {
		//stderr!
		const char *str = (const char*) data_ptr;
		DEBUG_LOG(HLE, "stderr: %s", str);
		return size;
	} else if (id == 1) {
		//stdout!
		char *str = (char *) data_ptr;
		char temp = str[size];
		str[size] = 0;
		DEBUG_LOG(HLE, "stdout: %s", str);
		str[size] = temp;
		return size;
	}
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		u8 *data = (u8*) data_ptr;
		f->asyncResult = (u32) pspFileSystem.WriteFile(f->handle, data, size);
		return f->asyncResult;
	} else {
		ERROR_LOG(HLE, "sceIoWrite ERROR: no file open");
		return error;
	}
}

s64 sceIoLseek(int id, s64 offset, int whence) {
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		FileMove seek = FILEMOVE_BEGIN;
		switch (whence) {
		case 0:
			break;
		case 1:
			seek = FILEMOVE_CURRENT;
			break;
		case 2:
			seek = FILEMOVE_END;
			break;
		}

		f->asyncResult = (u32) pspFileSystem.SeekFile(f->handle, (s32) offset,
				seek);
		DEBUG_LOG(HLE, "%i = sceIoLseek(%d,%i,%i)", f->asyncResult, id,
				(int) offset, whence);
		return f->asyncResult;
	} else {
		ERROR_LOG(HLE, "sceIoLseek(%d, %i, %i) - ERROR: invalid file", id,
				(int) offset, whence);
		return error;
	}
}

u32 sceIoLseek32(int id, int offset, int whence) {
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		DEBUG_LOG(HLE, "sceIoLseek32(%d,%08x,%i)", id, (int) offset, whence);

		FileMove seek = FILEMOVE_BEGIN;
		switch (whence) {
		case 0:
			break;
		case 1:
			seek = FILEMOVE_CURRENT;
			break;
		case 2:
			seek = FILEMOVE_END;
			break;
		}

		f->asyncResult = (u32) pspFileSystem.SeekFile(f->handle, (s32) offset,
				seek);
		return f->asyncResult;
	} else {
		ERROR_LOG(HLE, "sceIoLseek32 ERROR: no file open");
		return error;
	}
}

u32 sceIoOpen(const char* filename, int mode) {
	//memory stick filename
	int access = FILEACCESS_NONE;
	if (mode & O_RDONLY)
		access |= FILEACCESS_READ;
	if (mode & O_WRONLY)
		access |= FILEACCESS_WRITE;
	if (mode & O_APPEND)
		access |= FILEACCESS_APPEND;
	if (mode & O_CREAT)
		access |= FILEACCESS_CREATE;

	u32 h = pspFileSystem.OpenFile(filename, (FileAccess) access);
	if (h == 0)
	{
		ERROR_LOG(HLE,
				"ERROR_ERRNO_FILE_NOT_FOUND=sceIoOpen(%s, %08x) - file not found",
				filename, mode);
		return ERROR_ERRNO_FILE_NOT_FOUND;
	}

	FileNode *f = new FileNode();
	SceUID id = kernelObjects.Create(f);
	f->handle = h;
	f->fullpath = filename;
	f->asyncResult = id;
	DEBUG_LOG(HLE, "%i=sceIoOpen(%s, %08x)", id, filename, mode);
	return id;
}

u32 sceIoClose(int id) {
	DEBUG_LOG(HLE, "sceIoClose(%d)", id);
	return kernelObjects.Destroy < FileNode > (id);
}

u32 sceIoRemove(const char *filename) {
	DEBUG_LOG(HLE, "sceIoRemove(%s)", filename);
	if (pspFileSystem.DeleteFile(filename))
		return 0;
	else
		return -1;
}

u32 sceIoMkdir(const char *dirname, int mode) {
	DEBUG_LOG(HLE, "sceIoMkdir(%s, %i)", dirname, mode);
	if (pspFileSystem.MkDir(dirname))
		return 0;
	else
		return -1;
}

u32 sceIoRmdir(const char *dirname) {
	DEBUG_LOG(HLE, "sceIoRmdir(%s)", dirname);
	if (pspFileSystem.RmDir(dirname))
		return 0;
	else
		return -1;
}

void sceIoSync() {
	DEBUG_LOG(HLE, "UNIMPL sceIoSync not implemented");
}

struct DeviceSize {
	u32 maxSectors;
	u32 sectorSize;
	u32 sectorsPerCluster;
	u32 totalClusters;
	u32 freeClusters;
};

u32 sceIoDevctl(const char *name, int cmd, u32 argAddr, int argLen, u32 outPtr, int outLen) {

	if (strcmp(name, "emulator:")) {
		DEBUG_LOG(HLE,"sceIoDevctl(\"%s\", %08x, %08x, %i, %08x, %i)", name, cmd,argAddr,argLen,outPtr,outLen);
	}

	// UMD checks
	switch (cmd) {
	case 0x01F20001:  // Get Disc Type.
		if (Memory::IsValidAddress(outPtr)) {
			Memory::Write_U32(0x10, outPtr);  // Game disc
			return 0;
		} else {
			return -1;
		}
		break;
	case 0x01F20002:  // Get current LBA.
		if (Memory::IsValidAddress(outPtr)) {
			Memory::Write_U32(0, outPtr);  // Game disc
			return 0;
		} else {
			return -1;
		}
		break;
	case 0x01F100A3:  // Seek
		return 0;
	}

	// This should really send it on to a FileSystem implementation instead.

	if (!strcmp(name, "mscmhc0:") || !strcmp(name, "ms0:"))
	{
		switch (cmd)
		{
		// does one of these set a callback as well? (see coded arms)
		case 0x02015804:	// Register callback
			if (Memory::IsValidAddress(argAddr) && argLen == 4) {
				u32 cbId = Memory::Read_U32(argAddr);
				if (0 == __KernelRegisterCallback(THREAD_CALLBACK_MEMORYSTICK, cbId)) {
					DEBUG_LOG(HLE, "sceIoDevCtl: Memstick callback %i registered, notifying immediately.", cbId);
					__KernelNotifyCallbackType(THREAD_CALLBACK_MEMORYSTICK, cbId, MemoryStick_State());
					return 0;
				} else {
					return ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
				}
			}
			break;

		case 0x02025805:	// Unregister callback
			if (Memory::IsValidAddress(argAddr) && argLen == 4) {
				u32 cbId = Memory::Read_U32(argAddr);
				if (0 == __KernelUnregisterCallback(THREAD_CALLBACK_MEMORYSTICK, cbId)) {
					DEBUG_LOG(HLE, "sceIoDevCtl: Unregistered memstick callback %i", cbId);
					return 0;
				} else {
					return ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
				}
			}
			break;

		case 0x02025806:	// Memory stick inserted?
		case 0x02025801:	// Memstick Driver status?
			if (Memory::IsValidAddress(outPtr)) {
				Memory::Write_U32(1, outPtr);
				return 0;
			} else {
				return ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
			}

		case 0x02425818:  // Get memstick size etc
			// Pretend we have a 2GB memory stick.
			if (Memory::IsValidAddress(argAddr)) {  // "Should" be outPtr but isn't
				u32 pointer = Memory::Read_U32(argAddr);

				u64 totalSize = (u32)2 * 1024 * 1024 * 1024;
				u64 freeSize	= 1 * 1024 * 1024 * 1024;
				DeviceSize deviceSize;
				deviceSize.maxSectors				= 512;
				deviceSize.sectorSize				= 0x200;
				deviceSize.sectorsPerCluster = 0x08;
				deviceSize.totalClusters		 = (u32)((totalSize * 95 / 100) / (deviceSize.sectorSize * deviceSize.sectorsPerCluster));
				deviceSize.freeClusters			= (u32)((freeSize	* 95 / 100) / (deviceSize.sectorSize * deviceSize.sectorsPerCluster));
				Memory::WriteStruct(pointer, &deviceSize);
				return 0;
			} else {
				return ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
			}
		}
	}

	if (!strcmp(name, "fatms0:"))
	{
		switch (cmd) {
		case 0x02415821:  // MScmRegisterMSInsertEjectCallback
			{
				u32 cbId = Memory::Read_U32(argAddr);
				if (0 == __KernelRegisterCallback(THREAD_CALLBACK_MEMORYSTICK_FAT, cbId)) {
					DEBUG_LOG(HLE, "sceIoDevCtl: Memstick FAT callback %i registered, notifying immediately.", cbId);
					__KernelNotifyCallbackType(THREAD_CALLBACK_MEMORYSTICK_FAT, cbId, MemoryStick_FatState());
					return 0;
				} else {
					return -1;
				}
			}
			break;
		case 0x02415822: // MScmUnregisterMSInsertEjectCallback
			{
				u32 cbId = Memory::Read_U32(argAddr);
				if (0 == __KernelUnregisterCallback(THREAD_CALLBACK_MEMORYSTICK_FAT, cbId)) {
					DEBUG_LOG(HLE, "sceIoDevCtl: Unregistered memstick FAT callback %i", cbId);
					return 0;
				} else {
					return -1;
				}
			}

		case 0x02415823:  // Set FAT as enabled
			if (Memory::IsValidAddress(argAddr) && argLen == 4) {
				MemoryStick_SetFatState((MemStickFatState)Memory::Read_U32(argAddr));
				return 0;
			} else {
				ERROR_LOG(HLE, "Failed 0x02415823 fat");
				return -1;
			}
			break;

		case 0x02425823:  // Check if FAT enabled
			if (Memory::IsValidAddress(outPtr) && outLen == 4) {
				Memory::Write_U32(MemoryStick_FatState(), outPtr);
				return 0;
			} else {
				ERROR_LOG(HLE, "Failed 0x02425823 fat");
				return -1;
			}
			break;

		case 0x02425818:  // Get memstick size etc
			// Pretend we have a 2GB memory stick.
			{
				if (Memory::IsValidAddress(argAddr)) {  // "Should" be outPtr but isn't
					u32 pointer = Memory::Read_U32(argAddr);

					u64 totalSize = (u32)2 * 1024 * 1024 * 1024;
					u64 freeSize	= 1 * 1024 * 1024 * 1024;
					DeviceSize deviceSize;
					deviceSize.maxSectors				= 512;
					deviceSize.sectorSize				= 0x200;
					deviceSize.sectorsPerCluster = 0x08;
					deviceSize.totalClusters		 = (u32)((totalSize * 95 / 100) / (deviceSize.sectorSize * deviceSize.sectorsPerCluster));
					deviceSize.freeClusters			= (u32)((freeSize	* 95 / 100) / (deviceSize.sectorSize * deviceSize.sectorsPerCluster));
					Memory::WriteStruct(pointer, &deviceSize);
					return 0;
				} else {
					return ERROR_MEMSTICK_DEVCTL_BAD_PARAMS;
				}
			}
		}
	}

	if (!strcmp(name, "kemulator:") || !strcmp(name, "emulator:"))
	{
		// Emulator special tricks!
		switch (cmd)
		{
		case 1:	// EMULATOR_DEVCTL__GET_HAS_DISPLAY
			if (Memory::IsValidAddress(outPtr))
				Memory::Write_U32(0, outPtr);	 // TODO: Make a headless mode for running tests!
			return 0;
		case 2:	// EMULATOR_DEVCTL__SEND_OUTPUT
			{
				std::string data(Memory::GetCharPointer(argAddr), argLen);
				if (PSP_CoreParameter().printfEmuLog)
				{
					printf("%s", data.c_str());
#ifdef _WIN32
					OutputDebugString(data.c_str());
#endif
					// Also collect the debug output
					emuDebugOutput += data;
				}
				else
				{
					DEBUG_LOG(HLE, "%s", data.c_str());
				}
				return 0;
			}
		case 3:	// EMULATOR_DEVCTL__IS_EMULATOR
			if (Memory::IsValidAddress(outPtr))
				Memory::Write_U32(1, outPtr);	 // TODO: Make a headless mode for running tests!
			return 0;
		}

		ERROR_LOG(HLE, "sceIoDevCtl: UNKNOWN PARAMETERS");

		return 0;
	}

	//089c6d1c weird branch
	/*
	089c6bdc ]: HLE: sceKernelCreateCallback(name= MemoryStick Detection ,entry= 089c7484 ) (z_un_089c6bc4)
	089c6c18 ]: HLE: sceIoDevctl("mscmhc0:", 02015804, 09ffb9c0, 4, 00000000, 0) (z_un_089c6bc4)
	089c6c40 ]: HLE: sceKernelCreateCallback(name= MemoryStick Assignment ,entry= 089c7534 ) (z_un_089c6bc4)
	089c6c78 ]: HLE: sceIoDevctl("fatms0:", 02415821, 09ffb9c4, 4, 00000000, 0) (z_un_089c6bc4)
	089c6cac ]: HLE: sceIoDevctl("mscmhc0:", 02025806, 00000000, 0, 09ffb9c8, 4) (z_un_089c6bc4)
	*/
	return SCE_KERNEL_ERROR_UNSUP;
}

u32 sceIoRename(const char *from, const char *to) {
	DEBUG_LOG(HLE, "sceIoRename(%s, %s)", from, to);
	if (pspFileSystem.RenameFile(from, to))
		return 0;
	else
		return -1;
}

u32 sceIoChdir(const char *dirname) {
	pspFileSystem.ChDir(dirname);
	DEBUG_LOG(HLE, "sceIoChdir(%s)", dirname);
	return 1;
}

void sceIoChangeAsyncPriority()
{
	ERROR_LOG(HLE, "UNIMPL sceIoChangeAsyncPriority(%d)", PARAM(0));
	RETURN(0);
}

u32 __IoClose(SceUID id, int param)
{
	DEBUG_LOG(HLE, "Deferred IoClose(%d)", id);
	__IoCompleteAsyncIO(id);
	return kernelObjects.Destroy < FileNode > (id);
}

//TODO Not really sure if this should be wrapped nor how
void sceIoCloseAsync()
{
	DEBUG_LOG(HLE, "sceIoCloseAsync(%d)", PARAM(0));
	//sceIoClose();
	defAction = &__IoClose;
	RETURN(0);
}

u32 sceIoLseekAsync(int id, s64 offset, int whence)
{
	sceIoLseek(id, offset, whence);
	__IoCompleteAsyncIO(id);
	return 0;
}

u32 sceIoSetAsyncCallback(int id, u32 clbckId, u32 clbckArg)
{
	DEBUG_LOG(HLE, "sceIoSetAsyncCallback(%d, %i, %08x)", id, clbckId,
			clbckArg);

	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f)
	{
		f->callbackID = clbckId;
		f->callbackArg = clbckArg;
		return 0;
	}
	else
	{
		return error;
	}
}

u32 sceIoLseek32Async(int id, int offset, int whence)
{
	DEBUG_LOG(HLE, "sceIoLseek32Async(%d) sorta implemented", id);
	sceIoLseek32(id, offset, whence);
	__IoCompleteAsyncIO(id);
	return 0;
}

void sceIoOpenAsync(const char *filename, int mode)
{
	DEBUG_LOG(HLE, "sceIoOpenAsync() sorta implemented");
    RETURN(sceIoOpen(filename, mode));
//	__IoCompleteAsyncIO(currentMIPS->r[2]);	// The return value
	// We have to return a UID here, which may have been destroyed when we reach Wait if it failed.
	// Now that we're just faking it, we just don't RETURN(0) here.
}

u32 sceIoReadAsync(int id, u32 data_addr, int size)
{
	DEBUG_LOG(HLE, "sceIoReadAsync(%d)", id);
	sceIoRead(id, data_addr, size);
	__IoCompleteAsyncIO(id);
	return 0;
}

u32 sceIoGetAsyncStat(int id, u32 address, u32 uknwn)
{
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f)
	{
		Memory::Write_U64(f->asyncResult, address);
		DEBUG_LOG(HLE, "%i = sceIoGetAsyncStat(%i, %i, %08x) (HACK)",
				(u32) f->asyncResult, id, address, uknwn);
		return 0; //completed
	}
	else
	{
		ERROR_LOG(HLE, "ERROR - sceIoGetAsyncStat with invalid id %i", id);
		return -1;
	}
}

void sceIoWaitAsync(int id, u32 address, u32 uknwn) {
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		u64 res = f->asyncResult;
		if (defAction) {
			res = defAction(id, defParam);
			defAction = 0;
		}
		Memory::Write_U64(res, address);
		DEBUG_LOG(HLE, "%i = sceIoWaitAsync(%i, %08x) (HACK)", (u32) res, id,
				uknwn);
		RETURN(0); //completed
	} else {
		ERROR_LOG(HLE, "ERROR - sceIoWaitAsync waiting for invalid id %i", id);
		RETURN(-1);
	}
}

void sceIoWaitAsyncCB(int id, u32 address) {
	// Should process callbacks here
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		u64 res = f->asyncResult;
		if (defAction) {
			res = defAction(id, defParam);
			defAction = 0;
		}
		Memory::Write_U64(res, address);
		DEBUG_LOG(HLE, "%i = sceIoWaitAsyncCB(%i, %08x) (HACK)", (u32) res, id,
				address);
		RETURN(0); //completed
	} else {
		ERROR_LOG(HLE, "ERROR - sceIoWaitAsyncCB waiting for invalid id %i",
				id);
	}
}

u32 sceIoPollAsync(int id, u32 address) {
	u32 error;
	FileNode *f = kernelObjects.Get < FileNode > (id, error);
	if (f) {
		u64 res = f->asyncResult;
		if (defAction) {
			res = defAction(id, defParam);
			defAction = 0;
		}
		Memory::Write_U64(res, address);
		DEBUG_LOG(HLE, "%i = sceIoPollAsync(%i, %08x) (HACK)", (u32) res, id,
				address);
		return 0; //completed
	} else {
		ERROR_LOG(HLE, "ERROR - sceIoPollAsync waiting for invalid id %i", id);
		return -1;  // TODO: correct error code
	}
}

class DirListing : public KernelObject {
public:
	const char *GetName() {return name.c_str();}
	const char *GetTypeName() {return "DirListing";}
	static u32 GetMissingErrorCode() { return SCE_KERNEL_ERROR_BADF; }
	int GetIDType() const { return 0; }

	std::string name;
	std::vector<PSPFileInfo> listing;
	int index;
};

u32 sceIoDopen(const char *path) {
	DEBUG_LOG(HLE, "sceIoDopen(\"%s\")", path);

	DirListing *dir = new DirListing();
	SceUID id = kernelObjects.Create(dir);

	// TODO: ERROR_ERRNO_FILE_NOT_FOUND

	dir->listing = pspFileSystem.GetDirListing(path);
	dir->index = 0;
	dir->name = std::string(path);

	return id;
}

u32 sceIoDread(int id, u32 dirent_addr) {
	u32 error;
	DirListing *dir = kernelObjects.Get<DirListing>(id, error);
	if (dir) {
		if (dir->index == (int) dir->listing.size()) {
			DEBUG_LOG(HLE, "sceIoDread( %d %08x ) - end of the line", id,
					dirent_addr);
			return 0;
		}

		PSPFileInfo &info = dir->listing[dir->index];

		SceIoDirEnt *entry = (SceIoDirEnt*) Memory::GetPointer(dirent_addr);

		__IoGetStat(&entry->d_stat, info);

		strncpy(entry->d_name, info.name.c_str(), 256);
		entry->d_private = 0xC0DEBABE;
		DEBUG_LOG(HLE, "sceIoDread( %d %08x ) = %s", id, dirent_addr,
				entry->d_name);

		dir->index++;
		return (u32)(dir->listing.size() - dir->index + 1);
	} else {
		DEBUG_LOG(HLE, "sceIoDread - invalid listing %i, error %08x", id,
				error);
		return -1;  // TODO
	}
}

u32 sceIoDclose(int id) {
	DEBUG_LOG(HLE, "sceIoDclose(%d)", id);
	return kernelObjects.Destroy<DirListing>(id);
}

u32 sceIoIoctl(u32 id, u32 cmd, u32 indataPtr, u32 inlen, u32 outdataPtr, u32 outlen) 
{
	ERROR_LOG(HLE, "UNIMPL 0=sceIoIoctrl id: %08x, cmd %08x, indataPtr %08x, inlen %08x, outdataPtr %08x, outLen %08x", id,cmd,indataPtr,inlen,outdataPtr,outlen);
    return 0;
}

const HLEFunction IoFileMgrForUser[] = {
	{ 0xb29ddf9c, &WrapU_C<sceIoDopen>, "sceIoDopen" },
	{ 0xe3eb004c, &WrapU_IU<sceIoDread>, "sceIoDread" },
	{ 0xeb092469, &WrapU_I<sceIoDclose>, "sceIoDclose" },
	{ 0xe95a012b, 0, "sceIoIoctlAsync" },
	{ 0x63632449, &WrapU_UUUUUU<sceIoIoctl>, "sceIoIoctl" },
	{ 0xace946e8, &WrapU_CU<sceIoGetstat>, "sceIoGetstat" },
	{ 0xb8a740f4, 0, "sceIoChstat" },
	{ 0x55f4717d, &WrapU_C<sceIoChdir>, "sceIoChdir" },
	{ 0x08bd7374, 0, "sceIoGetDevType" },
	{ 0xB2A628C1, &WrapU_CCCU<sceIoAssign>, "sceIoAssign" },
	{ 0xe8bc6571, 0, "sceIoCancel" },
	{ 0xb293727f, sceIoChangeAsyncPriority, "sceIoChangeAsyncPriority" },
	{ 0x810C4BC3, &WrapU_I<sceIoClose>, "sceIoClose" }, //(int fd);
	{ 0xff5940b6, sceIoCloseAsync, "sceIoCloseAsync" },
	{ 0x54F5FB11, &WrapU_CIUIUI<sceIoDevctl>, "sceIoDevctl" }, //(const char *name int cmd, void *arg, size_t arglen, void *buf, size_t *buflen);
	{ 0xcb05f8d6, &WrapU_IUU<sceIoGetAsyncStat>, "sceIoGetAsyncStat" },
	{ 0x27EB27B8, &WrapI64_II64I<sceIoLseek>, "sceIoLseek" }, //(int fd, int offset, int whence);
	{ 0x68963324, &WrapU_III<sceIoLseek32>, "sceIoLseek32" },
	{ 0x1b385d8f, &WrapU_III<sceIoLseek32Async>, "sceIoLseek32Async" },
	{ 0x71b19e77, &WrapU_II64I<sceIoLseekAsync>, "sceIoLseekAsync" },
	{ 0x109F50BC, &WrapU_CI<sceIoOpen>, "sceIoOpen" }, //(const char* file, int mode);
	{ 0x89AA9906, &WrapV_CI<sceIoOpenAsync>, "sceIoOpenAsync" },
	{ 0x06A70004, &WrapU_CI<sceIoMkdir>, "sceIoMkdir" }, //(const char *dir, int mode);
	{ 0x3251ea56, &WrapU_IU<sceIoPollAsync>, "sceIoPollAsync" },
	{ 0x6A638D83, &WrapU_IUI<sceIoRead>, "sceIoRead" }, //(int fd, void *data, int size);
	{ 0xa0b5a7c2, &WrapU_IUI<sceIoReadAsync>, "sceIoReadAsync" },
	{ 0xF27A9C51, &WrapU_C<sceIoRemove>, "sceIoRemove" }, //(const char *file);
	{ 0x779103A0, &WrapU_CC<sceIoRename>, "sceIoRename" }, //(const char *oldname, const char *newname);
	{ 0x1117C65F, &WrapU_C<sceIoRmdir>, "sceIoRmdir" }, //(const char *dir);
	{ 0xA12A0514, &WrapU_IUU<sceIoSetAsyncCallback>, "sceIoSetAsyncCallback" },
	{ 0xab96437f, sceIoSync, "sceIoSync" },
	{ 0x6d08a871, 0, "sceIoUnassign" },
	{ 0x42EC03AC, &WrapU_IVI<sceIoWrite>, "sceIoWrite" }, //(int fd, void *data, int size);
	{ 0x0facab19, 0, "sceIoWriteAsync" },
	{ 0x35dbd746, &WrapV_IU<sceIoWaitAsyncCB>, "sceIoWaitAsyncCB" },
	{ 0xe23eec33, &WrapV_IUU<sceIoWaitAsync>, "sceIoWaitAsync" }, 
};

void Register_IoFileMgrForUser() {
	RegisterModule("IoFileMgrForUser", ARRAY_SIZE(IoFileMgrForUser), IoFileMgrForUser);
}


const HLEFunction StdioForUser[] = {
	{ 0x172D316E, &WrapU_V<sceKernelStdin>, "sceKernelStdin" },
	{ 0xA6BAB2E9, &WrapU_V<sceKernelStdout>, "sceKernelStdout" },
	{ 0xF78BA90A, &WrapU_V<sceKernelStderr>, "sceKernelStderr" }, 
};

void Register_StdioForUser() {
	RegisterModule("StdioForUser", ARRAY_SIZE(StdioForUser), StdioForUser);
}
