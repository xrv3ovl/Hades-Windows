#include <iostream>
#include <Windows.h>
#include <map>
#include <vector>
#include <queue>
#include <string>
#include <mutex>

#include "sysinfo.h"
#include "msgassist.h"
#include "umsginterface.h"

#include "uautostart.h"
#include "unet.h"
#include "usysuser.h"
#include "uprocesstree.h"
#include "uservicesoftware.h"
#include "ufile.h"
#include "uetw.h"


//rapidjson
#include <rapidjson/rapidjson.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

//nlohmannjson
#include <json.hpp>
using json_t = nlohmann::json;

static UAutoStart               g_grpc_uautostrobj;
static UNet                     g_grpc_unetobj;
static NSysUser                 g_grpc_usysuser;
static UProcess                 g_grpc_uprocesstree;
static UServerSoftware          g_grpc_userversoftware;
static UFile                    g_grpc_ufile;
static UEtw                     g_grpc_etw;

static std::queue<UEtwBuffer*>  g_etwdata_queue;
static std::mutex               g_etwdata_cs;
static HANDLE                   g_jobAvailableEvent;
static bool                     g_exit = false;


inline void uMsgInterface::uMsg_SetQueuePtr() { g_grpc_etw.uf_setqueuetaskptr(g_etwdata_queue); }
inline void uMsgInterface::uMsg_SetQueueLockPtr() { g_grpc_etw.uf_setqueuelockptr(g_etwdata_cs); }
inline void uMsgInterface::uMsg_SetEventPtr() { g_grpc_etw.uf_setqueueeventptr(g_jobAvailableEvent); }

void uMsgInterface::uMsg_Init() {
    // 初始化设置数据
    g_jobAvailableEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    this->uMsg_SetQueuePtr();
    this->uMsg_SetQueueLockPtr();
    this->uMsg_SetEventPtr();
    // 最后调用
    this->uMsg_taskPopInit();
};
void uMsgInterface::uMsg_Free()
{
    g_exit = true;
    for (size_t idx = 0; idx < m_thread.size(); ++idx)
    {
        SetEvent(g_jobAvailableEvent);
        WaitForSingleObject(m_thread[idx], INFINITE);
        CloseHandle(m_thread[idx]);
    }

    if (g_jobAvailableEvent != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(g_jobAvailableEvent);
        g_jobAvailableEvent = INVALID_HANDLE_VALUE;
    }
    m_thread.clear();
}

// 反序列化数据
void uMsgInterface::uMsgEtwDataHandlerEx()
{
    //for (;;)
    {
        g_etwdata_cs.lock();
        if (!g_etwdata_queue.size())
            return;
        UEtwBuffer* etw_taskdata = g_etwdata_queue.front();
        if (!etw_taskdata)
            return;
        g_etwdata_queue.pop();
        g_etwdata_cs.unlock();

        wchar_t output[MAX_PATH * 2] = { 0, };
        switch (etw_taskdata->taskid)
        {
        case UF_ETW_NETWORK:
        {
            UEtwNetWork* etwNet = (UEtwNetWork*)&(etw_taskdata->data[0]);
            if (!etwNet)
                break;
            swprintf(output, L"[etw] protocol:%d pid:%d localport: %x:%d  remoteport: %x:%d", \
                etwNet->protocol,
                etwNet->processId,
                etwNet->ipv4LocalAddr, etwNet->toLocalPort, \
                etwNet->RemoteAddr, etwNet->toRemotePort);
            OutputDebugString(output);
        }
        break;
        case UF_ETW_PROCESSINFO:
        {
            UEtwProcessInfo* etwProcess = (UEtwProcessInfo*)&(etw_taskdata->data[0]);
            if (!etwProcess)
                break;
            swprintf(output, L"[etw] pid: %d  Path: ", etwProcess->processId);
            lstrcatW(output, etwProcess->processPath);
            OutputDebugString(output);
        }
        break;
        default:
            break;
        }
    }
}
void uMsgInterface::uMsg_taskPopEtwLoop()
{
    try
    {
        if (!g_jobAvailableEvent)
            return;
        do
        {
            WaitForSingleObject(
                g_jobAvailableEvent,
                INFINITE
            );

            if (g_exit)
                break;

            uMsgEtwDataHandlerEx();

        } while (true);
    }
    catch (const std::exception&)
    {

    }
}
static unsigned WINAPI uMsg_taskPopThread(void* pData)
{
    (reinterpret_cast<uMsgInterface*>(pData))->uMsg_taskPopEtwLoop();
    return 0;
}
void uMsgInterface::uMsg_taskPopInit()
{
    int i = 0;
    HANDLE hThread;
    unsigned threadId;

    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    DWORD threadCount = sysinfo.dwNumberOfProcessors;
    if (threadCount == 0)
    {
        threadCount = 4;
    }
    for (i = 0; i < threadCount; i++)
    {
        hThread =(HANDLE)_beginthreadex(0, 0, uMsg_taskPopThread, (LPVOID)this, 0, &threadId);
        if (hThread != 0 && hThread != (HANDLE)(-1L))
        {
            m_thread.push_back(hThread);
        }
    }
}
void uMsgInterface::uMsg_taskPush(const int taskcode, std::vector<std::string>& vec_task_string)
{
    std::string tmpstr; wstring catstr;
    int i = 0, index = 0;
    DWORD dwAllocateMemSize = 0;
    char* ptr_Getbuffer;
    bool nstatus = Choose_mem(ptr_Getbuffer, dwAllocateMemSize, taskcode);
    if (false == nstatus || nullptr == ptr_Getbuffer || dwAllocateMemSize == 0)
        return;
    json_t j;
    try
    {
        // ptr_Getbuffer
        do
        {
            switch (taskcode)
            {
            case UF_PROCESS_ENUM:
            {
                if (false == g_grpc_uprocesstree.uf_EnumProcess(ptr_Getbuffer))
                    break;
                PUProcessNode procesNode = (PUProcessNode)ptr_Getbuffer;
                if (!procesNode)
                    break;

                std::vector<std::string> test_vec;
                for (i = 0; i < procesNode->processcount; ++i)
                {   
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, procesNode->sysprocess[i].fullprocesspath);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_process_Path"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, procesNode->sysprocess[i].szExeFile);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_process_szExeFile"] = tmpstr.c_str();
                    j["win_user_process_pid"] = to_string(procesNode->sysprocess[i].pid).c_str();
                    tmpstr.clear();
                    tmpstr = String_ToUtf8(procesNode->sysprocess[i].priclassbase);
                    j["win_user_process_pribase"] = tmpstr.c_str();
                    j["win_user_process_parenid"] = to_string(procesNode->sysprocess[i].th32ParentProcessID).c_str();
                    j["win_user_process_thrcout"] = to_string(procesNode->sysprocess[i].threadcout).c_str();
                    vec_task_string.push_back(j.dump());
                }
                std::cout << "[User] Process Enum Success" << std::endl;
            }
            break;
            case UF_PROCESS_PID_TREE:
            {
                // Command - pid
                if (false == g_grpc_uprocesstree.uf_GetProcessInfo(4, ptr_Getbuffer))
                    break;
            }
            break;
            case UF_SYSAUTO_START:
            {
                if (false == g_grpc_uautostrobj.uf_EnumAutoStartask(ptr_Getbuffer, dwAllocateMemSize))
                    break;

                PUAutoStartNode autorunnode = (PUAutoStartNode)ptr_Getbuffer;
                if (!autorunnode)
                    break;

                
                j["win_user_autorun_flag"] = "1";
                for (i = 0; i < autorunnode->regnumber; ++i)
                {
                    tmpstr.clear();
                    tmpstr = String_ToUtf8(autorunnode->regrun[i].szValueName);
                    j["win_user_autorun_regName"] = tmpstr.c_str();
                    tmpstr.clear();
                    tmpstr = String_ToUtf8(autorunnode->regrun[i].szValueKey);
                    j["win_user_autorun_regKey"] = tmpstr.c_str();
                    vec_task_string.push_back(j.dump());
                }

                j.clear();
                j["win_user_autorun_flag"] = "2";
                for (i = 0; i < autorunnode->taskrunnumber; ++i)
                {
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, autorunnode->taskschrun[i].szValueName);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_autorun_tschname"] = tmpstr.c_str();
                    j["win_user_autorun_tscState"] = to_string(autorunnode->taskschrun[i].State).c_str();
                    j["win_user_autorun_tscLastTime"] = to_string(autorunnode->taskschrun[i].LastTime).c_str();
                    j["win_user_autorun_tscNextTime"] = to_string(autorunnode->taskschrun[i].NextTime).c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, autorunnode->taskschrun[i].TaskCommand);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_autorun_tscCommand"] = tmpstr.c_str();
                    vec_task_string.push_back(j.dump());
                }

                std::cout << "[User] SystemAutoStartRun Enum Success" << std::endl;
            }
            break;
            case UF_SYSNET_INFO:
            {
                if (false == g_grpc_unetobj.uf_EnumNetwork(ptr_Getbuffer))
                    break;

                PUNetNode netnode = (PUNetNode)ptr_Getbuffer;
                if (!netnode)
                    break;
                j["win_user_net_flag"] = "1";
                for (i = 0; i < netnode->tcpnumber; i++)
                {
                    j["win_user_net_src"] = netnode->tcpnode[i].szlip;
                    j["win_user_net_dst"] = netnode->tcpnode[i].szrip;
                    j["win_user_net_status"] = netnode->tcpnode[i].TcpState;
                    j["win_user_net_pid"] = netnode->tcpnode[i].PidString;
                    vec_task_string.push_back(j.dump());
                }

                j.clear();
                j["win_user_net_flag"] = "2";
                for (i = 0; i < netnode->udpnumber; i++)
                {

                    j["win_user_net_src"] = netnode->tcpnode[i].szlip;
                    j["win_user_net_pid"] = netnode->tcpnode[i].PidString;
                    vec_task_string.push_back(j.dump());
                }
                std::cout << "[User] EnumNetwork Enum Success" << std::endl;
            }
            break;
            case UF_SYSSESSION_INFO: // v2.0
            {
            }
            break;
            case UF_SYSINFO_ID:     // v1.0 --> 是否上线时候主动发送?非被动采集
            {
            }
            break;
            case UF_SYSLOG_ID:      // 待定 --> etw完全可以取代
            {
            }
            break;
            case UF_SYSUSER_ID:
            {
                if (false == g_grpc_usysuser.uf_EnumSysUser(ptr_Getbuffer))
                    break;

                PUUserNode pusernode = (PUUserNode)ptr_Getbuffer;
                if (!pusernode)
                    break;

                for (i = 0; i < pusernode->usernumber; ++i)
                {
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pusernode->usernode[i].serveruser);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_sysuser_user"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pusernode->usernode[i].servername);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_sysuser_name"] = tmpstr.c_str();
                    j["win_user_sysuser_sid"] = to_string((ULONGLONG)pusernode->usernode[i].serverusid).c_str();
                    j["win_user_sysuser_flag"] = to_string(pusernode->usernode[i].serveruflag).c_str();
                    vec_task_string.push_back(j.dump());
                }
                std::cout << "[User] SysUser Enum Success" << std::endl;
            }
            break;
            case UF_SYSSERVICE_SOFTWARE_ID:
            {
                if (false == g_grpc_userversoftware.EnumAll(ptr_Getbuffer))
                    break;

                PUAllServerSoftware pNode = (PUAllServerSoftware)ptr_Getbuffer;
                if (!pNode)
                    break;

                j["win_user_softwareserver_flag"] = "1";
                for (i = 0; i < pNode->servicenumber; ++i)
                {
                    j.clear();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uSericeinfo[i].lpServiceName);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_server_lpsName"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uSericeinfo[i].lpDisplayName);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_server_lpdName"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uSericeinfo[i].lpBinaryPathName);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_server_lpPath"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uSericeinfo[i].lpDescription);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_server_lpDescr"] = tmpstr.c_str();
                    j["win_user_server_status"] = pNode->uSericeinfo[i].dwCurrentState;
                    vec_task_string.push_back(j.dump());
                }

                j.clear();
                j["win_user_softwareserver_flag"] = "2";
                for (i = 0; i < pNode->softwarenumber; ++i)
                {
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].szSoftName);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_lpsName"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].szSoftSize);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_Size"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].szSoftVer);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_Ver"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].strSoftInsPath);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_installpath"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].strSoftUniPath);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_uninstallpath"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].szSoftDate);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_data"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, pNode->uUsoinfo[i].strSoftVenRel);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_software_venrel"] = tmpstr.c_str();
                    vec_task_string.push_back(j.dump());
                }
                std::cout << "[User] Software_Server Enum Success" << std::endl;
            }
            break;
            case UF_SYSFILE_ID:
            {
                // Command 获取 目录路径
                if (false == g_grpc_ufile.uf_GetDirectoryFile((char*)"D:\\bin", ptr_Getbuffer))
                    break;

                PUDriectInfo directinfo = (PUDriectInfo)ptr_Getbuffer;
                if (!directinfo)
                    break;

                // 先回发送一次cout和总目录大小
                j["win_user_driectinfo_flag"] = "1";
                j["win_user_driectinfo_filecout"] = to_string(directinfo->FileNumber).c_str();
                j["win_user_driectinfo_size"] = to_string(directinfo->DriectAllSize).c_str();
                vec_task_string.push_back(j.dump());
               
                
                // 枚举的文件发送
                j.clear();
                j["win_user_driectinfo_flag"] = "2";
                for (i = 0; i < directinfo->FileNumber; ++i)
                {
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, directinfo->fileEntry[i].filename);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_driectinfo_filename"] = tmpstr.c_str();
                    tmpstr.clear();
                    Wchar_tToString(tmpstr, directinfo->fileEntry[i].filepath);
                    tmpstr = String_ToUtf8(tmpstr);
                    j["win_user_driectinfo_filePath"] = tmpstr.c_str();
                    j["win_user_driectinfo_fileSize"] = to_string(directinfo->fileEntry[i].filesize).c_str();
                    vec_task_string.push_back(j.dump());
                }
                std::cout << "[User] GetDirectoryFile Enum Success" << std::endl;
            }
            break;
            case UF_FILE_INFO:
            {
                // Command 获取 文件绝对路径
                if (false == g_grpc_ufile.uf_GetFileInfo((char*)"d:\\bin\\1.txt", ptr_Getbuffer))
                    break;

                PUFileInfo fileinfo = (PUFileInfo)ptr_Getbuffer;
                if (!fileinfo)
                    break;

                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->cFileName);
                tmpstr = String_ToUtf8(tmpstr);
                j["win_user_fileinfo_filename"] = tmpstr.c_str();
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->dwFileAttributes);
                tmpstr = String_ToUtf8(tmpstr);
                j["win_user_fileinfo_dwFileAttributes"] = tmpstr.c_str();
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->dwFileAttributesHide);
                tmpstr = String_ToUtf8(tmpstr);
                j["win_user_fileinfo_dwFileAttributesHide"] = tmpstr.c_str();
                j["win_user_fileinfo_md5"] = fileinfo->md5;
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->m_seFileSizeof);
                j["win_user_fileinfo_m_seFileSizeof"] = tmpstr.c_str();
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->seFileAccess);
                tmpstr = String_ToUtf8(tmpstr);
                j["win_user_fileinfo_seFileAccess"] = tmpstr.c_str();
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->seFileCreate);
                j["win_user_fileinfo_seFileCreate"] = tmpstr.c_str();
                tmpstr.clear();
                Wchar_tToString(tmpstr, fileinfo->seFileModify);
                j["win_user_fileinfo_seFileModify"] = tmpstr.c_str();
                vec_task_string.push_back(j.dump());
                std::cout << "[User] GetFIleInfo Success" << std::endl;
            }
            break;
            case UF_ROOTKIT_ID:     // v2.0
            {
            }
            break;
            default:
                break;
            }
        } while (false);
    }
    catch (const std::exception&)
    {

    }

    if (ptr_Getbuffer)
    {
        delete[] ptr_Getbuffer;
        ptr_Getbuffer = nullptr;
    }

}