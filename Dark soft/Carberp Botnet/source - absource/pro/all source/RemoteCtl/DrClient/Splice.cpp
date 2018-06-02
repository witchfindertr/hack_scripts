//#include <windows.h>
#include "stdafx.h"

//#include "Memory.h"
#include "GetApi.h"
#include "splice.h"

#ifdef _AMD64_
#include "hde64/hde64.cpp"
#else
#include "hde32/hde32.cpp"
#endif

LPVOID GetRelativeBranchDestination(LPVOID lpInst,hdes *hs)
{
#ifdef _AMD64_
    return (LPVOID)MAKEDWORDLONG((LODWORD(lpInst)+hs->len+hs->imm.imm32),HIDWORD(lpInst));
#else
    return (LPVOID)((DWORD)lpInst+hs->len+hs->imm.imm32);
#endif
}

inline bool IsInternalJump(LPVOID lpTarget,ULONG_PTR dest)
{
    ULONG_PTR pt=(ULONG_PTR)lpTarget;
    return ((pt <= dest) && (dest <= pt+sizeof(JMP_REL)));
}

void AppendTempAddress(LPVOID lpAddress,SIZE_T dwPos,CALL_ABS *lpInst,TEMP_ADDR *lpAddr,LPVOID *lpAddrTable)
{
#ifdef _AMD64_
    *lpAddrTable=lpAddress;
#else
    lpAddr->lpAddress=lpAddress;
#endif
    lpAddr->dwPosition=dwPos+((ULONG_PTR)&lpInst->operand-(ULONG_PTR)lpInst);
    lpAddr->dwPc=dwPos+sizeof(*lpInst);
}

void AppendTempAddress(LPVOID lpAddress,SIZE_T dwPos,JCC_ABS *lpInst,TEMP_ADDR *lpAddr,LPVOID *lpAddrTable)
{
#ifdef _AMD64_
    *lpAddrTable=lpAddress;
#else
    lpAddr->lpAddress=lpAddress;
#endif
    lpAddr->dwPosition=dwPos+((ULONG_PTR)&lpInst->operand-(ULONG_PTR)lpInst);
    lpAddr->dwPc=dwPos+sizeof(*lpInst);
}

void AppendTempAddress(LPVOID lpAddress,SIZE_T dwPos,CALL_REL *lpInst,TEMP_ADDR *lpAddr,LPVOID *lpAddrTable)
{
#ifdef _AMD64_
    *lpAddrTable=lpAddress;
#else
    lpAddr->lpAddress=lpAddress;
#endif
    lpAddr->dwPosition=dwPos+((ULONG_PTR)&lpInst->operand-(ULONG_PTR)lpInst);
    lpAddr->dwPc=dwPos+sizeof(*lpInst);
}

#ifdef _AMD64_
#pragma warning(disable:4244)
void AppendRipRelativeAddress(LPVOID lpInst,SIZE_T dwPos,hdes *hs,TEMP_ADDR *lpAddr)
{
    lpAddr->lpAddress=(LPVOID)((ULONG_PTR)lpInst+hs->len+hs->disp.disp32);
    lpAddr->dwPosition=dwPos+hs->len-((hs->flags & 0x3C) >> 2)-4;
    lpAddr->dwPc=dwPos+hs->len;
}
#endif

inline void SetJccOpcode(hdes *hs,JCC_REL *lpInst)
{
    UINT n=(((hs->opcode != 0x0F) ? hs->opcode:hs->opcode2) & 0x0F);
    lpInst->opcode=0x800F|(n<<8);
}

inline void SetJccOpcode(hdes *hs,JCC_ABS *lpInst)
{
    UINT n=(((hs->opcode != 0x0F) ? hs->opcode:hs->opcode2) & 0x0F);
    lpInst->opcode=0x70|n;
}

LPVOID CreateBridge(LPVOID lpFunc,LPVOID lpBackup,SIZE_T *dwBackupCodeSize,ULONG_PTR *lpTable)
{
    LPVOID lpBridge=malloc(JUMP_SIZE*6);
    if (lpBridge)
    {
        DWORD dwOldProtect=0;
        VirtualProtect(lpBridge,JUMP_SIZE*6,PAGE_EXECUTE_READWRITE,&dwOldProtect);
#ifdef _AMD64_
        CALL_ABS call={0x15FF,0x00000000};
        JMP_ABS jmp={0x25FF,0x00000000};
        JCC_ABS jcc={0x70,0x02,0xEB,0x06,0x25FF,0x00000000};
        int dwTmpAddrsCount=0;
#else
        CALL_REL call={0xE8,0x00000000};
        JMP_REL jmp={0xE9,0x00000000};
        JCC_REL jcc={0x800F,0x00000000};
#endif
        TEMP_ADDR TmpAddr[MAX_JUMPS]={0};
        int dwTmpAddrCount=0;

		SIZE_T dwOldPos=0,dwNewPos=0;
		ULONG_PTR dwJmpDest=0;
		bool bDone=false;
        while (!bDone)
		{
		    hdes hs;
			LPVOID lpInst=(LPVOID)((ULONG_PTR)lpFunc+dwOldPos);
		    SIZE_T dwCopySize=SizeOfCode(lpInst);
            if ((hs.flags & F_ERROR) == F_ERROR)
                break;

            LPVOID lpCopySrc=lpInst;
            if ((ULONG_PTR)lpInst-(ULONG_PTR)lpFunc >= sizeof(JMP_REL))
            {
                LPVOID lpTmpAddr=0;
#ifdef _AMD64_
                lpTmpAddr=&lpTable[dwTmpAddrsCount++];
                if (dwTmpAddrsCount > MAX_JUMPS)
                    break;
#endif
                AppendTempAddress(lpInst,dwNewPos,&jmp,&TmpAddr[dwTmpAddrCount++],(void **)lpTmpAddr);
                if (dwTmpAddrCount > MAX_JUMPS)
                    break;

                lpCopySrc=&jmp;
                dwCopySize=sizeof(jmp);

                bDone=true;
            }
#ifdef _AMD64_
			else if ((hs.modrm & 0xC7) == 0x05) // RIP-based
			{
				AppendRipRelativeAddress(lpInst,dwNewPos,&hs,&TmpAddr[dwTmpAddrCount++]);
                if (dwTmpAddrCount > MAX_JUMPS)
                    break;

				if ((hs.opcode == 0xFF) && (hs.modrm_reg == 4)) // jmp
					bDone=true;
			}
#endif
			else if (hs.opcode == 0xE8) // call
			{
                LPVOID lpTmpAddr=0;
#ifdef _AMD64_
                lpTmpAddr=&lpTable[dwTmpAddrsCount++];
                if (dwTmpAddrsCount > MAX_JUMPS)
                    break;
#endif
				AppendTempAddress(GetRelativeBranchDestination(lpInst,&hs),dwNewPos,&call,&TmpAddr[dwTmpAddrCount++],(void **)lpTmpAddr);
                if (dwTmpAddrCount > MAX_JUMPS)
                    break;

				lpCopySrc=&call;
				dwCopySize=sizeof(call);
			}
			else if ((hs.opcode & 0xFD) == 0xE9) // jmp
			{
				LPVOID lpDest=GetRelativeBranchDestination(lpInst,&hs);

				if (IsInternalJump(lpFunc,(ULONG_PTR)lpDest))
					dwJmpDest=max(dwJmpDest,(ULONG_PTR)lpDest);
				else
				{
                    LPVOID lpTmpAddr=0;
#ifdef _AMD64_
                    lpTmpAddr=&lpTable[dwTmpAddrsCount++];
                    if (dwTmpAddrsCount > MAX_JUMPS)
                        break;
#endif
					AppendTempAddress(lpDest,dwNewPos,&jmp,&TmpAddr[dwTmpAddrCount++],(void **)lpTmpAddr);
                    if (dwTmpAddrCount > MAX_JUMPS)
                        break;

					lpCopySrc = &jmp;
					dwCopySize = sizeof(jmp);

					bDone=((ULONG_PTR)lpInst >= dwJmpDest);
				}
			}
			else if (((hs.opcode & 0xF0) == 0x70) || (hs.opcode == 0xE3) || ((hs.opcode2 & 0xF0) == 0x80)) // jcc
			{
			    LPVOID lpDest=GetRelativeBranchDestination(lpInst,&hs);

				if (IsInternalJump(lpFunc,(ULONG_PTR)lpDest))
					dwJmpDest=max(dwJmpDest,(ULONG_PTR)lpDest);
				else if (hs.opcode == 0xE3) // jcxz, jecxz
                {
                    bDone=false;
                    break;
                }
				else
				{
                    LPVOID lpTmpAddr=0;
#ifdef _AMD64_
                    lpTmpAddr=&lpTable[dwTmpAddrsCount++];
                    if (dwTmpAddrsCount > MAX_JUMPS)
                        break;
#endif
					AppendTempAddress(lpDest,dwNewPos,&jcc,&TmpAddr[dwTmpAddrCount++],(void **)lpTmpAddr);
                    if (dwTmpAddrCount > MAX_JUMPS)
                        break;

					SetJccOpcode(&hs,&jcc);
					lpCopySrc=&jcc;
					dwCopySize=sizeof(jcc);
				}
			}
			else if (((hs.opcode & 0xFE) == 0xC2) || // ret
                     ((hs.opcode & 0xFD) == 0xE9) || // jmp rel
                     (((hs.modrm & 0xC7) == 0x05) && ((hs.opcode == 0xFF) && (hs.modrm_reg == 4))) || // jmp rip
                     ((hs.opcode == 0xFF) && (hs.opcode2 == 0x25))) // jmp abs
				bDone=((ULONG_PTR)lpInst >= dwJmpDest);

			if (((ULONG_PTR)lpInst < dwJmpDest) && (dwCopySize != hs.len))
			{
			    bDone=false;
				break;
			}

			memcpy((byte*)lpBridge+dwNewPos,lpCopySrc,dwCopySize);

			dwOldPos+=hs.len;
			dwNewPos+=dwCopySize;
		}

		if (bDone)
        {
            memcpy(lpBackup,lpFunc,dwOldPos);
            *dwBackupCodeSize=dwOldPos;
#ifdef _AMD64_
            int dwAddrTblPos=0;
#endif
            for (int i=0; i < dwTmpAddrCount; i++)
            {
                LPVOID lpAddr;
#ifdef _AMD64_
                if ((ULONG_PTR)TmpAddr[i].lpAddress < 0x10000)
                    lpAddr=&lpTable[dwAddrTblPos++];
                else
#endif
                    lpAddr=TmpAddr[i].lpAddress;
                byte *lpTrampoline=(byte*)lpBridge;
                *(DWORD*)(lpTrampoline+TmpAddr[i].dwPosition)=(DWORD)lpAddr-((DWORD)lpBridge+TmpAddr[i].dwPc);
            }
            return lpBridge;
        }
        else
            free(lpBridge);
    }
    return NULL;
}

void WriteRelativeJump(LPVOID lpFrom,LPVOID lpTo)
{
    JMP_REL jmp;

    DWORD dwOldProtect;
    VirtualProtect(lpFrom,sizeof(jmp),PAGE_EXECUTE_READWRITE,&dwOldProtect);
    if (!IsBadWritePtr(lpFrom,sizeof(jmp)))
    {
        jmp.opcode=0xE9;
        jmp.operand=(DWORD)lpTo-((DWORD)lpFrom+sizeof(jmp));

        memcpy(lpFrom,&jmp,sizeof(jmp));

        VirtualProtect(lpFrom,sizeof(jmp),dwOldProtect,&dwOldProtect);
    }
    return;
}

#ifdef _AMD64_
void WriteAbsoluteJump(LPVOID lpFrom,LPVOID lpTo,LPVOID lpTable)
{
    JMP_ABS jmp;

    DWORD dwOldProtect;
    VirtualProtect(lpFrom,sizeof(jmp),PAGE_EXECUTE_READWRITE,&dwOldProtect);
    if (!IsBadWritePtr(lpTo,sizeof(jmp)))
    {
        jmp.opcode=0x25FF;
        jmp.operand=(DWORD)lpTable-((DWORD)lpFrom+sizeof(jmp));

        memcpy(lpFrom,&jmp,sizeof(jmp));
        memcpy(lpTable,&lpTo,sizeof(lpTo));

        VirtualProtect(lpFrom,sizeof(jmp),dwOldProtect,&dwOldProtect);
    }
    return;
}
#endif

#ifdef _AMD64_
#define BLOCK_SIZE 0x10000
LPVOID malloc_SomewhereAroundHere(LPVOID lpFunc,SIZE_T dwSize)
{
    LPVOID lpAlloc=0;
    byte *lpMin=(byte *)lpFunc-0x20000000,
         *lpMax=(byte *)lpFunc+0x20000000;

    MEMORY_BASIC_INFORMATION mi={0};
    for (byte *lpQuery=lpMin; (INT_PTR)lpQuery < (INT_PTR)lpMax; lpQuery+=BLOCK_SIZE)
    {
        if (VirtualQuery(lpQuery,&mi,sizeof(mi)))
        {
            if (((mi.State == MEM_FREE) || (mi.State == MEM_RESERVE)) && (mi.RegionSize >= dwSize))
            {
                lpAlloc=VirtualAlloc(lpQuery,dwSize,MEM_COMMIT,PAGE_EXECUTE);
                if (lpAlloc)
                    break;
                else
                    lpQuery+=mi.RegionSize-BLOCK_SIZE;
            }
        }
    }
    return lpAlloc;
}
#endif

PVOID HookApi( DWORD Dll, DWORD FuncHash, DWORD ReplacementFunc )
{
    FARPROC pFunc;

    pFunc = (FARPROC)GetProcAddressEx( NULL, Dll, FuncHash );

    void *lpBackup=malloc(JUMP_SIZE*4);
    ULONG_PTR *lpTable=0;
#ifdef _AMD64_
    lpTable=(ULONG_PTR *)malloc(sizeof(ULONG_PTR)*MAX_JUMPS);
    LPVOID lpRelay=malloc_SomewhereAroundHere(pFunc,sizeof(JMP_REL));
    if (!lpRelay)
    {
        free(lpBackup);
        return;
    }
#endif
    SIZE_T dwBackupCodeSize=0;
    void *lpBridge=CreateBridge(pFunc,lpBackup,&dwBackupCodeSize,&lpTable[1]);
    if (lpBridge)
    {
#ifdef _AMD64_
        WriteAbsoluteJump(lpRelay,ReplacementFunc,lpTable);
        WriteRelativeJump(pFunc,lpRelay);
#else
        WriteRelativeJump(pFunc,(LPVOID)ReplacementFunc);
#endif
    }
    return lpBridge;
}