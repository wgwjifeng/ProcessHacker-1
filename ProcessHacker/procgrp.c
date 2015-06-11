/*
 * Process Hacker -
 *   process grouping
 *
 * Copyright (C) 2015 wj32
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <phapp.h>

typedef struct _PHP_PROCESS_DATA
{
    PPH_PROCESS_NODE Process;
    LIST_ENTRY ListEntry;
    BOOLEAN HasWindow;
} PHP_PROCESS_DATA, *PPHP_PROCESS_DATA;

PPH_LIST PhpCreateProcessDataList(
    _In_ PPH_LIST Processes
    )
{
    PPH_LIST processDataList;
    ULONG i;

    processDataList = PhCreateList(Processes->Count);

    for (i = 0; i < Processes->Count; i++)
    {
        PPH_PROCESS_NODE process = Processes->Items[i];
        PPHP_PROCESS_DATA processData;

        processData = PhAllocate(sizeof(PHP_PROCESS_DATA));
        processData->Process = process;
    }

    return processDataList;
}

VOID PhpDestroyProcessDataList(
    _In_ PPH_LIST List
    )
{
    ULONG i;

    for (i = 0; i < List->Count; i++)
    {
        PPHP_PROCESS_DATA processData = List->Items[i];
        PhFree(processData);
    }

    PhDereferenceObject(List);
}

VOID PhpProcessDataListToLinkedList(
    _In_ PPH_LIST List,
    _Out_ PLIST_ENTRY ListHead
    )
{
    ULONG i;

    InitializeListHead(ListHead);

    for (i = 0; i < List->Count; i++)
    {
        PPHP_PROCESS_DATA processData = List->Items[i];
        InsertTailList(ListHead, &processData->ListEntry);
    }
}

VOID PhpProcessDataListToHashtable(
    _In_ PPH_LIST List,
    _Out_ PPH_HASHTABLE *Hashtable
    )
{
    PPH_HASHTABLE hashtable;
    ULONG i;

    hashtable = PhCreateSimpleHashtable(List->Count);

    for (i = 0; i < List->Count; i++)
    {
        PPHP_PROCESS_DATA processData = List->Items[i];
        PhAddItemSimpleHashtable(hashtable, processData->Process->ProcessId, processData);
    }

    *Hashtable = hashtable;
}

typedef struct _QUERY_WINDOWS_CONTEXT
{
    PPH_HASHTABLE ProcessDataHashtable;
} QUERY_WINDOWS_CONTEXT, *PQUERY_WINDOWS_CONTEXT;

BOOL CALLBACK PhpQueryWindowsEnumWindowsProc(
    _In_ HWND hwnd,
    _In_ LPARAM lParam
    )
{
    PQUERY_WINDOWS_CONTEXT context = (PQUERY_WINDOWS_CONTEXT)lParam;
    ULONG processId;
    PPHP_PROCESS_DATA processData;
    HWND parentWindow;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    GetWindowThreadProcessId(hwnd, &processId);
    processData = PhFindItemSimpleHashtable2(context->ProcessDataHashtable, UlongToHandle(processId));

    if (!processData || processData->HasWindow)
        return TRUE;

    if (!((parentWindow = GetParent(hwnd)) && IsWindowVisible(parentWindow)) && // skip windows with a visible parent
        PhGetWindowTextEx(hwnd, PH_GET_WINDOW_TEXT_INTERNAL | PH_GET_WINDOW_TEXT_LENGTH_ONLY, NULL) != 0) // skip windows with no title
    {
        processData->HasWindow = TRUE;
    }

    return TRUE;
}

PPHP_PROCESS_DATA PhpFindGroupRoot(
    _In_ PPHP_PROCESS_DATA ProcessData,
    _In_ PPH_HASHTABLE ProcessDataHashtable
    )
{
    PPH_PROCESS_NODE root;
    PPHP_PROCESS_DATA rootProcessData;
    PPH_PROCESS_NODE parent;
    PPHP_PROCESS_DATA processData;
    PPH_STRING fileName;
    PPH_STRING userName;

    root = ProcessData->Process;
    rootProcessData = ProcessData;
    fileName = ProcessData->Process->ProcessItem->FileName;
    userName = ProcessData->Process->ProcessItem->UserName;

    if (ProcessData->HasWindow)
        return rootProcessData;

    while (parent = root->Parent)
    {
        if ((processData = PhFindItemSimpleHashtable2(ProcessDataHashtable, parent->ProcessId)) &&
            parent->ProcessItem->FileName && PhEqualString(parent->ProcessItem->FileName, fileName, TRUE) &&
            parent->ProcessItem->UserName && PhEqualString(parent->ProcessItem->UserName, userName, TRUE))
        {
            root = parent;
            rootProcessData = processData;

            if (processData->HasWindow)
                break;
        }
        else
        {
            break;
        }
    }

    return rootProcessData;
}

VOID PhpAddGroupMember(
    _In_ PPHP_PROCESS_DATA ProcessData,
    _Inout_ PPH_LIST List
    )
{
    PhAddItemList(List, ProcessData->Process);
    RemoveEntryList(&ProcessData->ListEntry);
}

VOID PhpAddGroupMembersFromRoot(
    _In_ PPHP_PROCESS_DATA ProcessData,
    _Inout_ PPH_LIST List,
    _In_ PPH_HASHTABLE ProcessDataHashtable
    )
{
    PPH_STRING fileName;
    PPH_STRING userName;
    ULONG i;

    PhpAddGroupMember(ProcessData, List);
    fileName = ProcessData->Process->ProcessItem->FileName;
    userName = ProcessData->Process->ProcessItem->UserName;

    for (i = 0; i < ProcessData->Process->Children->Count; i++)
    {
        PPH_PROCESS_NODE node = ProcessData->Process->Children->Items[i];
        PPHP_PROCESS_DATA processData;

        if ((processData = PhFindItemSimpleHashtable2(ProcessDataHashtable, node->ProcessId)) &&
            node->ProcessItem->FileName && PhEqualString(node->ProcessItem->FileName, fileName, TRUE) &&
            node->ProcessItem->UserName && PhEqualString(node->ProcessItem->UserName, userName, TRUE) &&
            !processData->HasWindow)
        {
            PhpAddGroupMembersFromRoot(processData, List, ProcessDataHashtable);
        }
    }
}

PPH_LIST PhCreateProcessGroupList(
    _In_opt_ PC_COMPARE_FUNCTION CompareFunction,
    _In_opt_ PVOID Context,
    _In_ ULONG MaximumGroups,
    _In_ ULONG Flags
    )
{
    PPH_LIST processList;
    PPH_LIST processDataList;
    LIST_ENTRY processDataListHead; // We will be removing things from this list as we group processes together
    PPH_HASHTABLE processDataHashtable; // Process ID to process data hashtable
    QUERY_WINDOWS_CONTEXT queryWindowsContext;
    PPH_LIST processGroupList;
    PLIST_ENTRY listEntry;

    // We group together processes that share a common ancestor and have the same file name, where the ancestor must
    // have a visible window and all other processes in the group do not have a visible window. All processes in the
    // group must have the same user name. All ancestors up to the lowest common ancestor must have the same file name
    // and user name.

    processList = PhDuplicateProcessNodeList();

    if (CompareFunction)
        qsort_s(processList->Items, processList->Count, sizeof(PPH_PROCESS_NODE), CompareFunction, Context);

    processDataList = PhpCreateProcessDataList(processList);
    PhDereferenceObject(processList);
    PhpProcessDataListToLinkedList(processDataList, &processDataListHead);
    PhpProcessDataListToHashtable(processDataList, &processDataHashtable);

    queryWindowsContext.ProcessDataHashtable = processDataHashtable;
    PhEnumChildWindows(NULL, 0x800, PhpQueryWindowsEnumWindowsProc, (LPARAM)&queryWindowsContext);

    processGroupList = PhCreateList(10);
    listEntry = processDataListHead.Flink;

    while (listEntry != &processDataListHead && processGroupList->Count < MaximumGroups)
    {
        PPHP_PROCESS_DATA processData = CONTAINING_RECORD(listEntry, PHP_PROCESS_DATA, ListEntry);
        PPH_PROCESS_GROUP processGroup;
        PPH_STRING fileName;
        PPH_STRING userName;

        listEntry = listEntry->Flink;
        processGroup = PhAllocate(sizeof(PH_PROCESS_GROUP));
        processGroup->Processes = PhCreateList(4);
        fileName = processData->Process->ProcessItem->FileName;
        userName = processData->Process->ProcessItem->UserName;

        if (!fileName || !userName || (Flags & PH_GROUP_PROCESSES_DONT_GROUP))
        {
            processGroup->Representative = processData->Process;
            PhpAddGroupMember(processData, processGroup->Processes);
        }
        else
        {
            processData = PhpFindGroupRoot(processData, processDataHashtable);
            processGroup->Representative = processData->Process;
            PhpAddGroupMembersFromRoot(processData, processGroup->Processes, processDataHashtable);
        }

        PhAddItemList(processGroupList, processGroup);
    }

    PhDereferenceObject(processDataHashtable);
    PhpDestroyProcessDataList(processDataList);

    return processGroupList;
}

VOID PhFreeProcessGroupList(
    _In_ PPH_LIST List
    )
{
    ULONG i;

    for (i = 0; i < List->Count; i++)
    {
        PPH_PROCESS_GROUP processGroup = List->Items[i];

        PhDereferenceObject(processGroup->Processes);
        PhFree(processGroup);
    }

    PhDereferenceObject(List);
}
