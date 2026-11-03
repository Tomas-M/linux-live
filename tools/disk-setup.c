#define _UNICODE
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const GUID linux_type={0x0fc63daf,0x8483,0x4772,{0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}};
static const GUID efi_type={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}};
static const GUID data_type={0xebd0a0a2,0xb9e5,0x4433,{0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}};
static const GUID empty_type={0};
static WCHAR error_text[768];

typedef struct
{
   BYTE mbr[512],header[512],*table;
   DWORD count,table_bytes,table_sectors;
   uint64_t disk_bytes,start,size;
} Plan;

// Retain a plain explanation for the final Windows dialog.
static BOOL fail(const WCHAR *message)
{
   wcsncpy(error_text,message,767);
   error_text[767]=0;
   return FALSE;
}

// Include the system's error text when a Windows API fails.
static BOOL windows_error(const WCHAR *message)
{
   DWORD code=GetLastError();
   WCHAR detail[512]=L"";
   FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,code,0,detail,512,NULL);
   swprintf(error_text,768,L"%ls\n\n%ls (Windows error %lu)",message,detail,(unsigned long)code);
   return FALSE;
}

// Decode little-endian on-disk fields without unaligned pointer casts.
static DWORD get32(const BYTE *data)
{
   DWORD value;
   memcpy(&value,data,4);
   return value;
}

// GPT addresses are 64-bit even for smaller FAT32 volumes.
static uint64_t get64(const BYTE *data)
{
   uint64_t value;
   memcpy(&value,data,8);
   return value;
}

// Encode FAT and GPT fields in the native little-endian Windows representation.
static void put32(BYTE *data,DWORD value)
{
   memcpy(data,&value,4);
}

// Keep 64-bit disk addresses intact in both GPT headers.
static void put64(BYTE *data,uint64_t value)
{
   memcpy(data,&value,8);
}

// Both GPT headers and partition arrays use standard reflected CRC32.
static DWORD crc32(const BYTE *data,DWORD size)
{
   DWORD crc=0xffffffff;
   for (DWORD i=0;i<size;i++)
   {
      crc^=data[i];
      for (int bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1)?0xedb88320:0);
   }
   return ~crc;
}

// Read only the requested sectors, including a disk's very last sector.
static BOOL read_at(HANDLE disk,uint64_t offset,void *data,DWORD size)
{
   LARGE_INTEGER position;
   DWORD done;
   position.QuadPart=offset;
   if (!SetFilePointerEx(disk,position,NULL,FILE_BEGIN) || !ReadFile(disk,data,size,&done,NULL))
      return windows_error(L"Could not read the USB disk.");
   if (done!=size) return fail(L"The USB disk returned incomplete data.");
   return TRUE;
}

// Write only validated metadata or the new data partition's filesystem structures.
static BOOL write_at(HANDLE disk,uint64_t offset,const void *data,DWORD size)
{
   LARGE_INTEGER position;
   DWORD done;
   position.QuadPart=offset;
   if (!SetFilePointerEx(disk,position,NULL,FILE_BEGIN) || !WriteFile(disk,data,size,&done,NULL))
      return windows_error(L"Could not write to the USB disk.");
   if (done!=size) return fail(L"The USB disk did not accept all data.");
   return TRUE;
}

// Validate a header before trusting any disk address or allocation size in it.
static BOOL read_header(HANDLE disk,uint64_t lba,uint64_t sectors,BYTE *header)
{
   if (lba<1 || lba>=sectors) return fail(L"A partition table header lies outside the disk.");
   if (!read_at(disk,lba*512,header,512)) return FALSE;
   DWORD size=get32(header+12),expected=get32(header+16);
   if (memcmp(header,"EFI PART",8) || size<92 || size>512) return fail(L"The disk has an invalid GPT header.");
   put32(header+16,0);
   BOOL valid=crc32(header,size)==expected && get64(header+24)==lba;
   put32(header+16,expected);
   if (!valid) return fail(L"The partition table header failed its integrity check.");
   return TRUE;
}

// Derive the free tail from the live ISO, accepting GPT already relocated by Windows.
static BOOL read_plan(HANDLE disk,uint64_t bytes,uint64_t source_offset,Plan *plan)
{
   memset(plan,0,sizeof(*plan));
   plan->disk_bytes=bytes;
   if (bytes%512) return fail(L"This tool requires a disk with 512-byte logical sectors.");
   uint64_t sectors=bytes/512;
   BYTE backup_header[512];
   if (!read_at(disk,0,plan->mbr,512) || !read_header(disk,1,sectors,plan->header)) return FALSE;
   BYTE *header=plan->header;
   uint64_t backup=get64(header+32);
   plan->count=get32(header+80);
   BOOL signature=plan->mbr[510]==0x55 && plan->mbr[511]==0xaa;
   BOOL entries=plan->count>=4 && plan->count<=1024 && get32(header+84)==128;
   if (!signature || !entries) return fail(L"The USB does not have the expected live-image partition table.");
   plan->table_bytes=plan->count*128;
   plan->table_sectors=(plan->table_bytes+511)/512;
   if (get64(header+72)!=2 || get64(header+40)<2+plan->table_sectors)
      return fail(L"The primary partition table has an unexpected layout.");
   plan->table=malloc(plan->table_bytes);
   if (!plan->table) return fail(L"Not enough memory to read the partition table.");
   if (!read_at(disk,1024,plan->table,plan->table_bytes)) return FALSE;
   DWORD table_crc=crc32(plan->table,plan->table_bytes);
   if (table_crc!=get32(header+88)) return fail(L"The partition table failed its integrity check.");
   if (!read_header(disk,backup,sectors,backup_header)) return FALSE;
   BOOL backup_address=backup>plan->table_sectors && get64(backup_header+72)==backup-plan->table_sectors;
   BOOL backup_layout=get32(backup_header+80)==plan->count && get32(backup_header+84)==128;
   BOOL matching=get64(backup_header+32)==1 && get32(backup_header+88)==table_crc;
   if (!backup_address || !backup_layout || !matching || memcmp(header+40,backup_header+40,32))
      return fail(L"The primary and backup partition tables do not agree.");
   BYTE *backup_table=malloc(plan->table_bytes);
   if (!backup_table) return fail(L"Not enough memory to verify the partition table.");
   BOOL read=read_at(disk,(backup-plan->table_sectors)*512,backup_table,plan->table_bytes);
   BOOL equal=read && !memcmp(plan->table,backup_table,plan->table_bytes);
   free(backup_table);
   if (!read) return FALSE;
   if (!equal) return fail(L"The primary and backup partition arrays differ.");

   uint64_t previous=get64(header+40)-1,image_bytes=0;
   DWORD used=0;
   for (DWORD i=0;i<plan->count;i++)
   {
      BYTE *entry=plan->table+i*128,sector[512];
      if (!memcmp(entry,&empty_type,16)) continue;
      uint64_t first=get64(entry+32),last=get64(entry+40);
      BOOL ordered=i==used && first>previous;
      BOOL range=last>=first && last<sectors && last<=get64(header+48);
      if (!ordered || !range) return fail(L"The disk has unexpected or overlapping partitions.");
      const GUID *type=&data_type;
      if (used==0) type=&linux_type;
      if (used==1) type=&efi_type;
      if (used>3 || memcmp(entry,type,16)) return fail(L"The disk contains an unexpected partition type.");
      if (used<3)
      {
         if (!read_at(disk,first*512,sector,512)) return FALSE;
         if (used==0)
         {
            if (memcmp(sector+1,"CD001",5)) return fail(L"The first partition is not a live ISO filesystem.");
            image_bytes=(uint64_t)get32(sector+80)*2048;
         }
         else if (memcmp(sector+54,"FAT16   ",8)) return fail(L"The boot or setup partition is not the expected FAT16 image.");
      }
      if (used==2 && first*512!=source_offset) return fail(L"Run this program from the disk's DISK-SETUP partition.");
      if (used==3)
      {
         WCHAR name[37]={0};
         memcpy(name,entry+56,72);
         if (wcscmp(name,L"Live data")) return fail(L"A different data partition already exists. It will not be reformatted.");
         plan->start=first*512;
      }
      previous=last;
      used++;
   }
   if (used==4) return TRUE;
   BOOL original=used==3 && previous<backup-plan->table_sectors;
   BOOL usable=get64(header+48)<backup-plan->table_sectors;
   if (!original || !usable) return fail(L"The disk is not a supported live image.");
   if (image_bytes<=(previous+1)*512 || image_bytes>bytes) return fail(L"The ISO image size does not contain the original partitions.");
   plan->start=(image_bytes+1048575)/1048576*1048576;
   uint64_t end=(sectors-1-plan->table_sectors)*512;
   if (end<=plan->start) return fail(L"There is no unused space available after the live image.");
   plan->size=end-plan->start;
   BOOL size_ok=plan->size>=64ULL*1024*1024 && plan->size/512<=UINT32_MAX;
   if (!size_ok || plan->start/512>UINT32_MAX) return fail(L"The unused space must be between 64 MiB and 2 TiB for FAT32.");
   return TRUE;
}

// Quick-format the unused tail as FAT32 without the Windows formatter's size limit.
static BOOL format_data(HANDLE disk,const Plan *plan)
{
   DWORD sectors=plan->size/512,cluster=1;
   if (plan->size>=512ULL*1024*1024) cluster=8;
   if (plan->size>=8ULL*1024*1024*1024) cluster=32;
   if (plan->size>=32ULL*1024*1024*1024) cluster=64;
   DWORD fat=((uint64_t)(sectors-32)/cluster+2+127)/128;
   DWORD clusters=(sectors-32-2*fat)/cluster;
   if (clusters<65525 || clusters>0x0ffffff5) return fail(L"This space cannot be formatted with the supported FAT32 cluster sizes.");
   BYTE boot[512]={0},info[512]={0},entries[512]={0},root[512]={0};
   boot[0]=0xeb; boot[1]=0x58; boot[2]=0x90;
   memcpy(boot+3,"MSWIN4.1",8);
   boot[12]=2; boot[13]=cluster; boot[14]=32; boot[16]=2; boot[21]=0xf8;
   boot[24]=63; boot[26]=255;
   put32(boot+28,plan->start/512); put32(boot+32,sectors); put32(boot+36,fat);
   put32(boot+44,2); boot[48]=1; boot[50]=6; boot[64]=0x80; boot[66]=0x29;
   put32(boot+67,GetTickCount());
   memcpy(boot+71,"USB-DATA   FAT32   ",19);
   boot[90]=0xeb; boot[91]=0xfe; boot[510]=0x55; boot[511]=0xaa;
   put32(info,0x41615252); put32(info+484,0x61417272);
   put32(info+488,clusters-1); put32(info+492,3); put32(info+508,0xaa550000);
   BYTE *zero=calloc(1,1024*1024);
   if (!zero) return fail(L"Not enough memory to prepare the filesystem.");
   uint64_t bytes=(32ULL+2ULL*fat+cluster)*512,offset=plan->start;
   while (bytes)
   {
      DWORD count=1024*1024;
      if (bytes<count) count=bytes;
      if (!write_at(disk,offset,zero,count)) { free(zero); return FALSE; }
      bytes-=count;
      offset+=count;
   }
   free(zero);
   if (!write_at(disk,plan->start,boot,512) || !write_at(disk,plan->start+512,info,512)) return FALSE;
   if (!write_at(disk,plan->start+6*512,boot,512) || !write_at(disk,plan->start+7*512,info,512)) return FALSE;
   put32(entries,0x0ffffff8); put32(entries+4,0x0fffffff); put32(entries+8,0x0fffffff);
   if (!write_at(disk,plan->start+32*512,entries,512) || !write_at(disk,plan->start+(32ULL+fat)*512,entries,512)) return FALSE;
   memcpy(root,"USB-DATA   ",11);
   root[11]=8;
   if (!write_at(disk,plan->start+(32ULL+2ULL*fat)*512,root,512)) return FALSE;
   if (!FlushFileBuffers(disk)) return windows_error(L"Could not finish writing the new filesystem.");
   return TRUE;
}

// Publish the new partition only after formatting, writing the backup GPT first.
static BOOL prepare_data(HANDLE disk,Plan *plan)
{
   GUID unique;
   if (FAILED(CoCreateGuid(&unique))) return fail(L"Windows could not create a unique partition identifier.");
   if (!format_data(disk,plan)) return FALSE;
   BYTE *entry=plan->table+3*128;
   memset(entry,0,128);
   memcpy(entry,&data_type,16); memcpy(entry+16,&unique,16);
   put64(entry+32,plan->start/512); put64(entry+40,(plan->start+plan->size)/512-1);
   memcpy(entry+56,L"Live data",18);
   uint64_t last=plan->disk_bytes/512-1;
   BYTE *header=plan->header,backup[512];
   put64(header+32,last); put64(header+48,last-plan->table_sectors-1);
   put32(header+88,crc32(plan->table,plan->table_bytes));
   memcpy(backup,header,512);
   put64(backup+24,last); put64(backup+32,1); put64(backup+72,last-plan->table_sectors);
   put32(backup+16,0); put32(backup+16,crc32(backup,get32(backup+12)));
   put32(header+16,0); put32(header+16,crc32(header,get32(header+12)));
   if (!write_at(disk,(last-plan->table_sectors)*512,plan->table,plan->table_bytes) || !write_at(disk,last*512,backup,512)) return FALSE;
   if (!FlushFileBuffers(disk)) return windows_error(L"Could not finish writing the backup partition table.");
   if (!write_at(disk,1024,plan->table,plan->table_bytes) || !write_at(disk,512,header,512)) return FALSE;
   for (int i=446;i<510;i+=16)
      if (plan->mbr[i+4]==0xee) put32(plan->mbr+i+12,last>UINT32_MAX?UINT32_MAX:last);
   if (!write_at(disk,0,plan->mbr,512)) return FALSE;
   if (!FlushFileBuffers(disk)) return windows_error(L"Could not finish updating the disk.");
   return TRUE;
}

#ifdef DISK_SETUP_TEST
// Test builds exercise the same disk code on explicit disposable images or VHDs.
int wmain(int argc,WCHAR **argv)
{
   if (argc!=5) return 2;
   uint64_t bytes=_wcstoui64(argv[2],NULL,10);
   // Large tests must use a mounted disposable VHD, avoiding NTFS hole zero filling.
   if (bytes>2ULL*1024*1024*1024 && _wcsnicmp(argv[1],L"\\\\.\\PhysicalDrive",17)) return 2;
   HANDLE disk=CreateFileW(argv[1],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,NULL);
   if (disk==INVALID_HANDLE_VALUE) return 3;
   Plan plan={0};
   BOOL ok=read_plan(disk,bytes,_wcstoui64(argv[3],NULL,10),&plan);
   if (ok) wprintf(L"PLAN %llu %llu\n",(unsigned long long)plan.start,(unsigned long long)plan.size);
   if (ok && plan.size && !wcscmp(argv[4],L"write")) ok=prepare_data(disk,&plan);
   if (!ok) fwprintf(stderr,L"%ls\n",error_text);
   free(plan.table);
   CloseHandle(disk);
   return ok?0:1;
}
#else

// Resolve a mounted volume to exactly one physical disk and partition offset.
static BOOL volume_extent(const WCHAR *name,DISK_EXTENT *extent)
{
   WCHAR path[128];
   wcsncpy(path,name,127); path[127]=0;
   size_t length=wcslen(path);
   if (length && path[length-1]=='\\') path[length-1]=0;
   HANDLE volume=CreateFileW(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
   if (volume==INVALID_HANDLE_VALUE) return FALSE;
   VOLUME_DISK_EXTENTS extents;
   DWORD returned;
   BOOL ok=DeviceIoControl(volume,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,NULL,0,&extents,sizeof(extents),&returned,NULL);
   CloseHandle(volume);
   if (!ok || extents.NumberOfDiskExtents!=1) return FALSE;
   *extent=extents.Extents[0];
   return TRUE;
}

// Use Windows' storage provider to reject disks used by its system or boot files.
static BOOL safe_system_disk(DWORD number)
{
   IWbemLocator *locator=NULL;
   IWbemServices *services=NULL;
   IEnumWbemClassObject *objects=NULL;
   IWbemClassObject *object=NULL;
   BSTR space=SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage"),language=SysAllocString(L"WQL");
   WCHAR query[128];
   swprintf(query,128,L"SELECT IsBoot,IsSystem FROM MSFT_Disk WHERE Number=%lu",(unsigned long)number);
   BSTR statement=SysAllocString(query);
   HRESULT result=CoCreateInstance(&CLSID_WbemLocator,NULL,CLSCTX_INPROC_SERVER,&IID_IWbemLocator,(void **)&locator);
   if (SUCCEEDED(result)) result=IWbemLocator_ConnectServer(locator,space,NULL,NULL,NULL,0,NULL,NULL,&services);
   if (SUCCEEDED(result)) result=CoSetProxyBlanket((IUnknown *)services,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,NULL,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,NULL,EOAC_NONE);
   if (SUCCEEDED(result)) result=IWbemServices_ExecQuery(services,language,statement,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,NULL,&objects);
   ULONG count=0;
   if (SUCCEEDED(result)) result=IEnumWbemClassObject_Next(objects,10000,1,&object,&count);
   BOOL safe=FALSE;
   if (SUCCEEDED(result) && count==1)
   {
      VARIANT boot,system;
      VariantInit(&boot); VariantInit(&system);
      HRESULT a=IWbemClassObject_Get(object,L"IsBoot",0,&boot,NULL,NULL);
      HRESULT b=IWbemClassObject_Get(object,L"IsSystem",0,&system,NULL,NULL);
      BOOL valid=SUCCEEDED(a) && SUCCEEDED(b) && V_VT(&boot)==VT_BOOL && V_VT(&system)==VT_BOOL;
      safe=valid && V_BOOL(&boot)==VARIANT_FALSE && V_BOOL(&system)==VARIANT_FALSE;
      VariantClear(&boot); VariantClear(&system);
   }
   if (object) IWbemClassObject_Release(object);
   if (objects) IEnumWbemClassObject_Release(objects);
   if (services) IWbemServices_Release(services);
   if (locator) IWbemLocator_Release(locator);
   SysFreeString(statement); SysFreeString(language); SysFreeString(space);
   if (!safe) return fail(L"Windows could not confirm that this disk is separate from its system and boot files. No changes were made.");
   return TRUE;
}

// Wait for the new FAT32 volume and give it an available Windows drive letter.
static BOOL mount_data(DWORD number,uint64_t offset,WCHAR *drive)
{
   for (int attempt=0;attempt<30;attempt++)
   {
      WCHAR name[128];
      HANDLE search=FindFirstVolumeW(name,128);
      if (search!=INVALID_HANDLE_VALUE)
      {
         do
         {
            DISK_EXTENT extent;
            if (!volume_extent(name,&extent)) continue;
            if (extent.DiskNumber!=number || (uint64_t)extent.StartingOffset.QuadPart!=offset) continue;
            WCHAR filesystem[32],label[32],paths[512];
            DWORD required;
            if (!GetVolumeInformationW(name,label,32,NULL,NULL,NULL,filesystem,32)) continue;
            if (wcscmp(filesystem,L"FAT32")) continue;
            if (GetVolumePathNamesForVolumeNameW(name,paths,512,&required) && paths[0])
            {
               wcsncpy(drive,paths,259); drive[259]=0;
               FindVolumeClose(search);
               return TRUE;
            }
            DWORD used=GetLogicalDrives();
            for (int letter=3;letter<26;letter++)
            {
               if (used&(1UL<<letter)) continue;
               WCHAR path[]={L'A'+letter,L':',L'\\',0};
               if (!SetVolumeMountPointW(path,name)) continue;
               wcscpy(drive,path);
               FindVolumeClose(search);
               return TRUE;
            }
         } while (FindNextVolumeW(search,name,128));
         FindVolumeClose(search);
      }
      Sleep(200);
   }
   return fail(L"The data partition is ready, but Windows has not mounted it. Unplug and reconnect the USB drive to access USB-DATA.");
}

typedef struct
{
   HANDLE disk,thread;
   DWORD number;
   uint64_t source_offset;
   Plan *plan;
   HWND dialog;
   BOOL success;
   WCHAR drive[260];
} Job;

// Recheck the confirmed disk layout, prepare its free tail and refresh Windows.
static DWORD WINAPI prepare_worker(void *data)
{
   Job *job=data;
   Plan fresh={0};
   BOOL ok=read_plan(job->disk,job->plan->disk_bytes,job->source_offset,&fresh);
   if (ok)
   {
      BOOL same_space=fresh.start==job->plan->start && fresh.size==job->plan->size;
      BOOL same_partitions=fresh.table_bytes==job->plan->table_bytes && !memcmp(fresh.table,job->plan->table,fresh.table_bytes);
      if (!same_space || !same_partitions) ok=fail(L"The disk layout changed while this window was open. Close Disk Setup and try again.");
   }
   if (ok) ok=prepare_data(job->disk,&fresh);
   free(fresh.table);
   DWORD returned;
   if (ok && !DeviceIoControl(job->disk,IOCTL_DISK_UPDATE_PROPERTIES,NULL,0,NULL,0,&returned,NULL))
      ok=windows_error(L"The data partition was created, but Windows could not refresh the disk. Reconnect the USB drive.");
   if (ok) ok=mount_data(job->number,job->plan->start,job->drive);
   job->success=ok;
   return 0;
}

// Keep the progress window responsive while disk writes run on a worker thread.
static HRESULT CALLBACK progress_callback(HWND window,UINT event,WPARAM button,LPARAM value,LONG_PTR data)
{
   (void)value;
   Job *job=(Job *)data;
   if (event==TDN_CREATED)
   {
      job->dialog=window;
      SendMessageW(window,TDM_ENABLE_BUTTON,IDOK,FALSE);
      SendMessageW(window,TDM_SET_MARQUEE_PROGRESS_BAR,TRUE,0);
      SendMessageW(window,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,30);
      job->thread=CreateThread(NULL,0,prepare_worker,job,0,NULL);
      if (!job->thread)
      {
         windows_error(L"Could not start disk preparation.");
         SendMessageW(window,TDM_ENABLE_BUTTON,IDOK,TRUE);
         PostMessageW(window,TDM_CLICK_BUTTON,IDOK,0);
      }
   }
   if (event==TDN_TIMER && job->thread && WaitForSingleObject(job->thread,0)==WAIT_OBJECT_0)
   {
      SendMessageW(window,TDM_ENABLE_BUTTON,IDOK,TRUE);
      SendMessageW(window,TDM_CLICK_BUTTON,IDOK,0);
   }
   if (event==TDN_BUTTON_CLICKED && button==IDOK && job->thread)
      if (WaitForSingleObject(job->thread,0)==WAIT_TIMEOUT) return S_FALSE;
   return S_OK;
}

// Display ordinary themed Windows dialogs with a clear heading and explanation.
static int dialog(const WCHAR *heading,const WCHAR *content,PCWSTR icon,TASKDIALOG_COMMON_BUTTON_FLAGS buttons)
{
   int button=IDCANCEL;
   if (FAILED(TaskDialog(NULL,NULL,L"Disk Setup",heading,content,buttons,icon,&button)))
      button=MessageBoxW(NULL,content,L"Disk Setup",MB_OKCANCEL|MB_ICONINFORMATION);
   return button;
}

// Identify only the USB containing this executable; no disk picker or arbitrary target.
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show)
{
   (void)instance; (void)previous; (void)show;
   (void)command;
   int argc;
   WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
   BOOL preview=argv && argc==2 && !wcscmp(argv[1],L"--preview");
   LocalFree(argv);
   CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
   HANDLE disk=INVALID_HANDLE_VALUE,mutex=NULL;
   Plan plan={0};
   WCHAR executable[32768],root[32768],volume[128],device[64],model[256]=L"USB disk";
   DISK_EXTENT extent;
   BOOL ok=FALSE;
   if (!GetModuleFileNameW(NULL,executable,32768) || !GetVolumePathNameW(executable,root,32768) || !GetVolumeNameForVolumeMountPointW(root,volume,128))
   {
      windows_error(L"Could not locate the disk containing Disk Setup.");
      goto done;
   }
   if (!volume_extent(volume,&extent)) { fail(L"Run Disk Setup from the USB drive's DISK-SETUP partition."); goto done; }
   // Serialize preparation on this disk across users and elevated instances.
   WCHAR mutex_name[96];
   swprintf(mutex_name,96,L"Global\\LinuxLiveKit.DiskSetup.%lu",(unsigned long)extent.DiskNumber);
   mutex=CreateMutexW(NULL,FALSE,mutex_name);
   if (!mutex) { windows_error(L"Could not reserve this disk for preparation."); goto done; }
   if (GetLastError()==ERROR_ALREADY_EXISTS) { fail(L"Disk Setup is already running for this disk. Close the other window first."); goto done; }
   swprintf(device,64,L"\\\\.\\PhysicalDrive%lu",(unsigned long)extent.DiskNumber);
   DWORD access=GENERIC_READ;
   if (!preview) access|=GENERIC_WRITE;
   disk=CreateFileW(device,access,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,NULL);
   if (disk==INVALID_HANDLE_VALUE) { windows_error(L"Could not open the USB disk. Administrator access is required."); goto done; }
   BYTE descriptor[4096];
   STORAGE_PROPERTY_QUERY query={StorageDeviceProperty,PropertyStandardQuery,{0}};
   DWORD returned;
   if (!DeviceIoControl(disk,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),descriptor,sizeof(descriptor),&returned,NULL))
   { windows_error(L"Could not identify the disk hardware."); goto done; }
   STORAGE_DEVICE_DESCRIPTOR *storage=(STORAGE_DEVICE_DESCRIPTOR *)descriptor;
   if (returned<sizeof(*storage)) { fail(L"Windows returned incomplete disk hardware information."); goto done; }
   BOOL supported_bus=storage->BusType==BusTypeUsb;
#ifdef DISK_SETUP_UI_TEST
   supported_bus=storage->BusType==BusTypeFileBackedVirtual || storage->BusType==BusTypeVirtual;
#endif
   if (!supported_bus)
   { fail(L"This disk cannot be prepared. Run Disk Setup from DISK-SETUP on your live USB drive."); goto done; }
   if (storage->ProductIdOffset && storage->ProductIdOffset<returned)
   {
      descriptor[returned-1]=0;
      MultiByteToWideChar(CP_ACP,0,(char *)descriptor+storage->ProductIdOffset,-1,model,256);
   }
   BYTE geometry[1024];
   if (!DeviceIoControl(disk,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,NULL,0,geometry,sizeof(geometry),&returned,NULL))
   { windows_error(L"Could not determine the USB disk capacity."); goto done; }
   DISK_GEOMETRY_EX *physical=(DISK_GEOMETRY_EX *)geometry;
   if (physical->Geometry.BytesPerSector!=512) { fail(L"This tool requires 512-byte logical sectors."); goto done; }
   if (!safe_system_disk(extent.DiskNumber)) goto done;
   if (!preview && !DeviceIoControl(disk,IOCTL_DISK_IS_WRITABLE,NULL,0,NULL,0,&returned,NULL))
   { windows_error(L"The USB disk is write-protected or unavailable."); goto done; }
   if (!read_plan(disk,physical->DiskSize.QuadPart,extent.StartingOffset.QuadPart,&plan)) goto done;
   if (preview)
   {
      wprintf(L"PLAN %llu %llu\n",(unsigned long long)plan.start,(unsigned long long)plan.size);
      ok=TRUE;
      goto done;
   }
   if (!plan.size)
   {
      WCHAR drive[260],message[512];
      if (!mount_data(extent.DiskNumber,plan.start,drive)) goto done;
      swprintf(message,512,L"This USB already has a data drive at %ls",drive);
      dialog(L"USB-DATA is already available",message,TD_INFORMATION_ICON,TDCBF_OK_BUTTON);
      ok=TRUE;
      goto done;
   }
   WCHAR explanation[1024];
   swprintf(explanation,1024,L"Create a %.2f GB drive (FAT32) for your files.\n\nUSB: %ls\n\nThe live system and boot files will be kept.",plan.size/1e9,model);
   if (dialog(L"Use the remaining USB space",explanation,TD_INFORMATION_ICON,TDCBF_OK_BUTTON|TDCBF_CANCEL_BUTTON)!=IDOK)
   { ok=TRUE; goto done; }
   Job job={0};
   job.disk=disk; job.number=extent.DiskNumber; job.source_offset=extent.StartingOffset.QuadPart; job.plan=&plan;
   TASKDIALOGCONFIG progress={0};
   progress.cbSize=sizeof(progress); progress.pszWindowTitle=L"Disk Setup";
   progress.pszMainInstruction=L"Preparing USB-DATA";
   progress.pszContent=L"Keep the USB drive connected.";
   progress.dwFlags=TDF_SHOW_MARQUEE_PROGRESS_BAR|TDF_CALLBACK_TIMER;
   progress.dwCommonButtons=TDCBF_OK_BUTTON; progress.pfCallback=progress_callback; progress.lpCallbackData=(LONG_PTR)&job;
   HRESULT result=TaskDialogIndirect(&progress,NULL,NULL,NULL);
   if (job.thread) { WaitForSingleObject(job.thread,INFINITE); CloseHandle(job.thread); }
   if (FAILED(result)) { fail(L"Windows could not open the disk preparation window."); goto done; }
   if (!job.success) goto done;
   swprintf(explanation,1024,L"Your new drive is available at %ls",job.drive);
   dialog(L"USB-DATA is ready",explanation,TD_INFORMATION_ICON,TDCBF_OK_BUTTON);
   ok=TRUE;
done:
   if (!ok)
   {
      if (preview) fwprintf(stderr,L"%ls\n",error_text);
      else dialog(L"Disk Setup could not continue",error_text,TD_ERROR_ICON,TDCBF_OK_BUTTON);
   }
   free(plan.table);
   if (disk!=INVALID_HANDLE_VALUE) CloseHandle(disk);
   if (mutex) CloseHandle(mutex);
   CoUninitialize();
   return ok?0:1;
}
#endif
