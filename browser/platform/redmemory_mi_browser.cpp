/* Portable RedMi heap backend for WebAssembly and the native ILP32 oracle.
 *
 * OpenSWBF2 replaced the original 2005 fixed-arena allocator in RedMemory.cpp with a 16-slot
 * heap backend (RedMemoryMi.h) backed by mimalloc plus Win32 HeapWalk/VirtualQuery diagnostics.
 * Both wasm32 and our native i386 oracle compile against the same RedMemoryMi.h contract without
 * Windows virtual-memory queries by maintaining:
 *
 *   1. An intrusive doubly-linked list per heap slot (0..15) so RedMiDestroy(heap) frees every
 *      block belonging to that heap cleanly on Mission <-> Shell transitions and RedMiVisit()
 *      walks live blocks in O(n).
 *   2. An open-addressing pointer hash table of live RedMi allocations so RedMiOwner(p) and
 *      RedMiClassify(p) answer in O(1) without ever dereferencing arbitrary CRT/stack pointers.
 *      When a pointer is not in the live table, RedMiOwner(p) returns -1, which tells
 *      RedMemory.cpp::RedFreeToHeap to forward the block to PblCrtFreeUncounted(p).
 *
 * Every allocation is zero-initialised and aligned to max(16, gRedHeapSlots[heap].align), matching
 * mi_heap_zalloc_aligned in RedMemoryMi.cpp.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "RedEngineFL/Memory/RedMemory.h"
#include "RedEngineFL/Memory/RedMemoryMi.h"

namespace
{
struct BlockNode
{
    void* pUser;
    void* pRaw;
    size_t uSize;
    size_t uCharge;
    int iHeap;
    unsigned int uGen;
    BlockNode* pPrev;
    BlockNode* pNext;
};

struct HeapState
{
    bool bLive;
    unsigned int uGen;
    BlockNode* pHead;
};

HeapState g_heaps[kRedHeapSlots];
bool g_bFailHeapCreate = false;
bool g_bExhaustionText = false;

/* Open-addressing hash table mapping pUser -> BlockNode*.
 * Tombstone sentinel allows deletion without breaking probe chains. */
BlockNode* const kTombstone = (BlockNode*)(uintptr_t)1;

BlockNode** g_table = 0;
size_t g_tableCap = 0;   /* always power of two */
size_t g_tableCount = 0; /* live entries */
size_t g_tableUsed = 0;  /* live + tombstones */

static inline size_t HashPtr(const void* p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (size_t)x;
}

static bool TableRehash(size_t uNewCap)
{
    BlockNode** pNew = (BlockNode**)calloc(uNewCap, sizeof(BlockNode*));
    if (!pNew)
    {
        return false;
    }
    const size_t uMask = uNewCap - 1;
    if (g_table)
    {
        for (size_t i = 0; i < g_tableCap; ++i)
        {
            BlockNode* pNode = g_table[i];
            if (pNode && pNode != kTombstone)
            {
                size_t idx = HashPtr(pNode->pUser) & uMask;
                while (pNew[idx])
                {
                    idx = (idx + 1) & uMask;
                }
                pNew[idx] = pNode;
            }
        }
        free(g_table);
    }
    g_table = pNew;
    g_tableCap = uNewCap;
    g_tableUsed = g_tableCount;
    return true;
}

static bool TableInsert(BlockNode* pNode)
{
    if (g_tableCap == 0)
    {
        if (!TableRehash(4096))
        {
            return false;
        }
    }
    else if ((g_tableUsed + 1) * 10 >= g_tableCap * 6)
    {
        const size_t uNextCap = (g_tableCount * 10 >= g_tableCap * 5) ? (g_tableCap * 2) : g_tableCap;
        if (!TableRehash(uNextCap))
        {
            return false;
        }
    }
    const size_t uMask = g_tableCap - 1;
    size_t idx = HashPtr(pNode->pUser) & uMask;
    size_t firstTomb = (size_t)-1;
    for (;;)
    {
        BlockNode* pCur = g_table[idx];
        if (!pCur)
        {
            if (firstTomb != (size_t)-1)
            {
                g_table[firstTomb] = pNode;
            }
            else
            {
                g_table[idx] = pNode;
                ++g_tableUsed;
            }
            ++g_tableCount;
            return true;
        }
        if (pCur == kTombstone && firstTomb == (size_t)-1)
        {
            firstTomb = idx;
        }
        idx = (idx + 1) & uMask;
    }
}

static BlockNode* TableFind(const void* pUser)
{
    if (!pUser || g_tableCap == 0)
    {
        return 0;
    }
    const size_t uMask = g_tableCap - 1;
    size_t idx = HashPtr(pUser) & uMask;
    for (;;)
    {
        BlockNode* pCur = g_table[idx];
        if (!pCur)
        {
            return 0;
        }
        if (pCur != kTombstone && pCur->pUser == pUser)
        {
            return pCur;
        }
        idx = (idx + 1) & uMask;
    }
}

static BlockNode* TableRemove(const void* pUser)
{
    if (!pUser || g_tableCap == 0)
    {
        return 0;
    }
    const size_t uMask = g_tableCap - 1;
    size_t idx = HashPtr(pUser) & uMask;
    for (;;)
    {
        BlockNode* pCur = g_table[idx];
        if (!pCur)
        {
            return 0;
        }
        if (pCur != kTombstone && pCur->pUser == pUser)
        {
            g_table[idx] = kTombstone;
            --g_tableCount;
            return pCur;
        }
        idx = (idx + 1) & uMask;
    }
}

static unsigned int ComputeCharge(int iHeap, size_t uSize)
{
    unsigned int uAlign = gRedHeapSlots[iHeap].align;
    if (uAlign < 4)
    {
        uAlign = 4;
    }
    const unsigned int uHdr = (uAlign + kRedTagBytes - 1) & ~(uAlign - 1);
    return ((unsigned int)uSize + uAlign - 1 + uHdr) & ~(uAlign - 1);
}

static void UnlinkNode(BlockNode* pNode)
{
    HeapState& rHeap = g_heaps[pNode->iHeap];
    if (pNode->pPrev)
    {
        pNode->pPrev->pNext = pNode->pNext;
    }
    else if (rHeap.pHead == pNode)
    {
        rHeap.pHead = pNode->pNext;
    }
    if (pNode->pNext)
    {
        pNode->pNext->pPrev = pNode->pPrev;
    }
    pNode->pPrev = 0;
    pNode->pNext = 0;
}
}  // namespace

void* RedMiInit(void* start, int maxheaps)
{
    const uintptr_t uHeaps = ((uintptr_t)start + 3) & ~(uintptr_t)3;
    return (void*)((uHeaps + (size_t)maxheaps * kRedHeapObjBytes + 3) & ~(uintptr_t)3);
}

void RedMiDestroy(int i)
{
    if (i < 0 || i >= kRedHeapSlots || !g_heaps[i].bLive)
    {
        return;
    }
    BlockNode* pCur = g_heaps[i].pHead;
    g_heaps[i].pHead = 0;
    while (pCur)
    {
        BlockNode* pNext = pCur->pNext;
        TableRemove(pCur->pUser);
        free(pCur->pRaw);
        free(pCur);
        pCur = pNext;
    }
    g_heaps[i].bLive = false;
    g_heaps[i].uGen = (g_heaps[i].uGen + 1) & 0xFFFFFFu;
}

size_t RedMiCreate(int heap, void* start, void* end, unsigned int align)
{
    if (heap < 0 || heap >= kRedHeapSlots)
    {
        return 0;
    }
    if (align == 0 || (align & (align - 1)) != 0)
    {
        align = 16;
    }
    const unsigned int uMask = ~(align - 1);
    const unsigned int uStart = ((unsigned int)(uintptr_t)start + align - 1) & uMask;
    const unsigned int uEnd = (unsigned int)(uintptr_t)end & uMask;
    RedMiDestroy(heap);
    g_heaps[heap].uGen = (g_heaps[heap].uGen + 1) & 0xFFFFFFu;
    if (g_bFailHeapCreate)
    {
        return 0;
    }
    g_heaps[heap].bLive = true;
    g_heaps[heap].pHead = 0;
    return (uEnd >= uStart) ? (size_t)(uEnd - uStart) : 0;
}

void RedMiFailHeapCreate(bool bFail)
{
    g_bFailHeapCreate = bFail;
}

void* RedMiAlloc(int i, size_t size, size_t* pReserved)
{
    if (i < 0 || i >= kRedHeapSlots || !g_heaps[i].bLive)
    {
        return 0;
    }
    unsigned int uAlign = gRedHeapSlots[i].align;
    if (uAlign < 16)
    {
        uAlign = 16;
    }
    const unsigned int uCharge = ComputeCharge(i, size);
    const size_t uAllocSize = (size > 0 ? size : 1) + uAlign;
    void* pRaw = calloc(1, uAllocSize);
    if (!pRaw)
    {
        RedMemory::failHeap = i;
        RedMemory::failSize = uCharge;
        return 0;
    }
    const uintptr_t uAligned = ((uintptr_t)pRaw + (uAlign - 1)) & ~(uintptr_t)(uAlign - 1);
    void* pUser = (void*)uAligned;

    BlockNode* pNode = (BlockNode*)malloc(sizeof(BlockNode));
    if (!pNode)
    {
        free(pRaw);
        RedMemory::failHeap = i;
        RedMemory::failSize = uCharge;
        return 0;
    }
    pNode->pUser = pUser;
    pNode->pRaw = pRaw;
    pNode->uSize = size;
    pNode->uCharge = uCharge;
    pNode->iHeap = i;
    pNode->uGen = g_heaps[i].uGen;
    pNode->pPrev = 0;
    pNode->pNext = g_heaps[i].pHead;
    if (g_heaps[i].pHead)
    {
        g_heaps[i].pHead->pPrev = pNode;
    }
    g_heaps[i].pHead = pNode;

    if (!TableInsert(pNode))
    {
        UnlinkNode(pNode);
        free(pNode);
        free(pRaw);
        RedMemory::failHeap = i;
        RedMemory::failSize = uCharge;
        return 0;
    }

    if (pReserved)
    {
        *pReserved = uCharge;
    }
    return pUser;
}

RedMiBlockKind RedMiClassify(const void* pUser)
{
    BlockNode* pNode = TableFind(pUser);
    if (!pNode)
    {
        return kRedMiBadPrefix;
    }
    if (pNode->iHeap < 0 || pNode->iHeap >= kRedHeapSlots || !g_heaps[pNode->iHeap].bLive)
    {
        return kRedMiBadPrefix;
    }
    if (pNode->uGen != g_heaps[pNode->iHeap].uGen)
    {
        return kRedMiBadPrefix;
    }
    return kRedMiValid;
}

size_t RedMiFree(void* block)
{
    BlockNode* pNode = TableRemove(block);
    if (!pNode)
    {
        return 0;
    }
    const size_t uCharge = pNode->uCharge;
    UnlinkNode(pNode);
    free(pNode->pRaw);
    free(pNode);
    return uCharge;
}

void* RedMiRealloc(int i, void* block, size_t oldSize, size_t size, size_t* pReserved,
                   size_t* pOldCharge)
{
    BlockNode* pOld = TableFind(block);
    if (!pOld)
    {
        return 0;
    }
    void* pNew = RedMiAlloc(i, size, pReserved);
    if (!pNew)
    {
        return 0;
    }
    const size_t uCopy = oldSize < size ? oldSize : size;
    if (uCopy > 0)
    {
        memcpy(pNew, block, uCopy);
    }
    const size_t uOld = RedMiFree(block);
    if (pOldCharge)
    {
        *pOldCharge = uOld;
    }
    return pNew;
}

int RedMiOwner(const void* p)
{
    BlockNode* pNode = TableFind(p);
    if (!pNode)
    {
        return -1;
    }
    if (pNode->iHeap >= 0 && pNode->iHeap < kRedHeapSlots && g_heaps[pNode->iHeap].bLive &&
        pNode->uGen == g_heaps[pNode->iHeap].uGen)
    {
        return pNode->iHeap;
    }
    return kRedOwnerStale;
}

int RedMiMoveEnd(int iHeap, int iDelta, bool* pbAllocFailed)
{
    if (pbAllocFailed)
    {
        *pbAllocFailed = false;
    }
    int iSource = iHeap;
    int iTarget = iHeap + 1;
    unsigned int uAmount = (unsigned int)iDelta;
    if (iDelta < 0)
    {
        iSource = iHeap + 1;
        iTarget = iHeap;
        uAmount = (unsigned int)(-iDelta);
    }
    const RedHeapSlot& rTarget = gRedHeapSlots[iTarget];
    const unsigned int uSourceAlign = gRedHeapSlots[iSource].align;
    const unsigned int uAlign = rTarget.align;
    const unsigned int uHdr = (uAlign + kRedTagBytes - 1) & ~(uAlign - 1);
    const unsigned int uMoveAlign = uSourceAlign > uAlign ? uSourceAlign : uAlign;
    const unsigned int uMove = uAmount & ~(uMoveAlign - 1);
    if (uMove <= uHdr)
    {
        return 0;
    }
    const unsigned int uUsed = (unsigned int)rTarget.used;
    const unsigned int uHeadroom = uUsed < rTarget.defaultCap ? rTarget.defaultCap - uUsed : 0;
    if (uMove > uHeadroom)
    {
        RedMemory::failHeap = iTarget;
        RedMemory::failSize = uMove;
        if (pbAllocFailed)
        {
            *pbAllocFailed = true;
        }
        return 0;
    }
    const unsigned int uRem = uHeadroom - uMove;
    if (uRem != 0 && uRem < uAlign + uHdr)
    {
        return 0;
    }
    return iDelta < 0 ? -(int)uMove : (int)uMove;
}

bool RedMiVisit(int heap, bool (*pfnBlock)(void* pUser, size_t uSize, void* pArg), void* pArg)
{
    if (heap < 0 || heap >= kRedHeapSlots || !g_heaps[heap].bLive || !pfnBlock)
    {
        return false;
    }
    for (BlockNode* pCur = g_heaps[heap].pHead; pCur; pCur = pCur->pNext)
    {
        if (!pfnBlock(pCur->pUser, pCur->uSize, pArg))
        {
            break;
        }
    }
    return true;
}

void RedMiNoteOutput(const char* pcMsg)
{
    static const char* const kapcExhaustion[] = {
        "unable to allocate memory for a thread local heap",
        "unable to dynamically create a thread local for a heap",
        "unable to allocate thread local variables",
        "aligned allocation request is too large",
    };
    bool bFound = false;
    for (int i = 0; i < 4; ++i)
    {
        bFound = bFound || (pcMsg != 0 && strstr(pcMsg, kapcExhaustion[i]) != 0);
    }
    g_bExhaustionText = bFound;
}

bool RedMiTakeErrorFatal(int err)
{
    const bool bExhaustion = g_bExhaustionText;
    g_bExhaustionText = false;
    if (err == ENOMEM || err == EOVERFLOW)
    {
        return false;
    }
    return !(bExhaustion && (err == EFAULT || err == EINVAL));
}

void RedMiConfigure(long lPurgeMs)
{
    (void)lPurgeMs;
}

void RedMiRegisterOutput()
{
}

bool RedMiFormatOptions(char* pcOut, unsigned int uSize)
{
    if (!pcOut || uSize == 0)
    {
        return false;
    }
    const int iLen = snprintf(pcOut, uSize, "OPENSWBF2 BROWSER_HEAP slots=%d live_blocks=%zu",
                              kRedHeapSlots, g_tableCount);
    if (iLen < 0 || (unsigned int)iLen >= uSize)
    {
        pcOut[uSize - 1] = 0;
        return false;
    }
    return true;
}

void RedMiOutputLines(const char* pcMsg,
                      void (*pfnLine)(const char* pcLine, int iLen, void* pArg), void* pArg)
{
    if (!pcMsg || !pfnLine)
    {
        return;
    }
    const char* pcStart = pcMsg;
    for (;;)
    {
        const char* pcEnd = strchr(pcStart, '\n');
        const int iLen = pcEnd ? (int)(pcEnd - pcStart) : (int)strlen(pcStart);
        if (iLen > 0)
        {
            pfnLine(pcStart, iLen, pArg);
        }
        if (!pcEnd)
        {
            return;
        }
        pcStart = pcEnd + 1;
    }
}

bool RedMiFirstArena(uintptr_t* puBase, size_t* puSize)
{
    (void)puBase;
    (void)puSize;
    return false;
}

void RedMiRecordReservation(const void* p)
{
    (void)p;
}

void RedMiTrim()
{
}
