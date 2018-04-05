#pragma once
#include <winsock2.h>
#include <MSWSock.h>
#include <vector>
#include <string>
#include <cassert>
#include <map>

// 缓冲区长度 (1024*8)
// 之所以为什么设置8K，也是一个江湖上的经验值
// 如果确实客户端发来的每组数据都比较少，那么就设置得小一些，省内存
#define MAX_BUFFER_LEN        8192  
// 默认的context 个数
#define DEFAULT_CLIENT_CONTEXT_LEN	32
// 软件信息长度
#define PROGRAM_INFO_LEN	128


//////////////////////////////////////////////////////////////////
// 在完成端口上投递的I/O操作的类型
typedef enum _OPERATION_TYPE
{
	ACCEPT_POSTED,                     // 标志投递的Accept操作
	SEND_POSTED,                       // 标志投递的是发送操作
	RECV_POSTED,                       // 标志投递的是接收操作
	NULL_POSTED                        // 用于初始化，无意义
}OPERATION_TYPE;

//====================================================================================
//
//				单IO数据结构体定义(用于每一个重叠操作的参数)
//
//====================================================================================

typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED		m_Overlapped;                               // 每一个重叠网络操作的重叠结构(针对每一个Socket的每一个操作，都要有一个)              
	SOCKET			m_sockAccept;                               // 这个网络操作所使用的Socket
	WSABUF			m_wsaBuf;                                   // WSA类型的缓冲区，用于给重叠操作传参数的
	char			m_szBuffer[MAX_BUFFER_LEN];                 // 这个是WSABUF里具体存字符的缓冲区
	OPERATION_TYPE	m_OpType;                                   // 标识网络操作的类型(对应上面的枚举)
	// 初始化
	_PER_IO_CONTEXT()
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_sockAccept = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_OpType = NULL_POSTED;
	}
	// 释放掉Socket
	~_PER_IO_CONTEXT()
	{
		if (m_sockAccept != INVALID_SOCKET)
		{
			closesocket(m_sockAccept);
			m_sockAccept = INVALID_SOCKET;
		}
	}
	// 重置缓冲区内容
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_wsaBuf.len = MAX_BUFFER_LEN;
	}

} PER_IO_CONTEXT, *PPER_IO_CONTEXT;


//====================================================================================
//
//				单句柄数据结构体定义(用于每一个完成端口，也就是每一个Socket的参数)
//
//====================================================================================

typedef struct _PER_SOCKET_CONTEXT
{
	SOCKET      m_Socket;									// 每一个客户端连接的Socket
	SOCKADDR_IN m_ClientAddr;								// 客户端的地址
	std::vector<_PER_IO_CONTEXT*> m_vectorIoContext;		// 客户端网络操作的上下文数据，
															// 也就是说对于每一个客户端Socket，是可以在上面同时投递多个IO请求的
	int missed_heartbeats;									// 记录心跳包丢失次数
	_PER_SOCKET_CONTEXT()
	{
		m_vectorIoContext.reserve(DEFAULT_CLIENT_CONTEXT_LEN);
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr));
		missed_heartbeats = 0;
	}

	// 释放资源
	~_PER_SOCKET_CONTEXT()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}
		// 释放掉所有的IO上下文数据
		for (int i = 0; i < m_vectorIoContext.size(); i++)
		{
			delete m_vectorIoContext.at(i);
		}
		m_vectorIoContext.clear();
	}

	// 获取一个新的IoContext
	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* p = new _PER_IO_CONTEXT;

		m_vectorIoContext.push_back(p);

		return p;
	}

	// 从数组中移除一个指定的IoContext
	void RemoveContext(_PER_IO_CONTEXT* pContext)
	{
		assert(pContext != NULL);

		for (int i = 0; i < m_vectorIoContext.size(); i++)
		{
			if (pContext == m_vectorIoContext.at(i))
			{
				delete pContext;
				pContext = NULL;
				std::vector<_PER_IO_CONTEXT*>::iterator it = m_vectorIoContext.begin() + i;
				m_vectorIoContext.erase(it);
				break;
			}
		}
	}

} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;

// 工作者线程的线程参数
class IOCPHeartBeatServer;
typedef struct _tagThreadParams
{
	IOCPHeartBeatServer*	pIOCPServer;										// 类指针，用于调用类中的函数
	int         nThreadNo;										// 线程编号
} THREADPARAMS, *PTHREADPARAMS;

class IOCPHeartBeatServer
{
public:
	IOCPHeartBeatServer();
	virtual ~IOCPHeartBeatServer();

	// 加载socket库
	bool LoadSockLib();

	// 卸载socket库
	void UnloadSockLib();

	// 启动服务器
	bool Start();

	//	停止服务器
	void Stop();

	// 获得本机的IP地址
	std::string GetLocalIP();
private:
	// 线程函数，为IOCP请求服务的工作者线程
	static DWORD WINAPI _WorkerThread(LPVOID lpParam);

	// 初始化IOCP
	bool _InitializeIOCP();

	// 初始化Socket
	bool _InitializeListenSocket();

	// 最后释放资源
	void _DeInitialize();

	// 在有客户端连入的时候，进行处理(新建一socket),并且取出第一组数据
	bool _DoAccpet(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, PER_SOCKET_CONTEXT* pNewClientSocketContext, PER_IO_CONTEXT* pNewClientIoContext);

	// 投递Accept请求
	bool _PostAccept(PER_IO_CONTEXT* pAcceptIoContext);

	// 将句柄绑定到完成端口中(例如：将accept后的socket绑定到完成端口)
	bool _AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext);

	// 处理接收到的数据
	bool _DealRecvData(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// 投递接受数据的请求
	bool _DoPostRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// 投递接收数据请求
	bool _PostRecv(PER_IO_CONTEXT* pIoContext);

	// 投递数据发送请求
	bool _DoPostSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen);

	// 投递数据发送请求
	bool _PostSend(PER_IO_CONTEXT* pIoContext,uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen);

	// 将客户端的相关信息存储到数组中
	void _AddToContextList(PER_SOCKET_CONTEXT *pSocketContext);

	// 将客户端的信息从数组中移除
	void _RemoveContext(PER_SOCKET_CONTEXT *pSocketContext);

	// 清空客户端信息
	void _ClearContextList();

	// 处理完成端口上的错误
	bool HandleError(PER_SOCKET_CONTEXT *pSockContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr);

		// 判断客户端Socket是否已经断开
	bool _IsSocketAlive(SOCKET s);

	// 获得本机的处理器数量
	int _GetNoOfProcessors();

public:
	// 读取配置信息
	bool ReadConfig();

private:
	int								m_nThreads;						// 生成的线程数量
	HANDLE*							m_phWorkerThreads;				// 工作者线程的句柄指针
	HANDLE							m_hIOCompletionPort;			// 完成端口的句柄
	HANDLE							m_hShutdownEvent;				// 用来通知线程系统退出的事件，为了能够更好的退出线程
	bool							m_bLibLoaded;					// socket库是否被加载
	CRITICAL_SECTION				m_csClientSockContext;          // 用于Worker线程同步的互斥量

	std::string						m_strIP;						// 服务器端的IP地址
	unsigned short					m_nPort;                       // 服务器端的监听端口

	std::vector<PER_SOCKET_CONTEXT*> m_vectorClientSockContext;          // 客户端Socket的Context信息        

	PER_SOCKET_CONTEXT*				m_pListenContext;              // 用于监听的Socket的Context信息

	// 函数指针在构造函数中初始为NULL，Run-time stack #2错误
	LPFN_ACCEPTEX					m_lpfnAcceptEx;					// AcceptEx的函数指针，用于调用这个扩展函数
	LPFN_GETACCEPTEXSOCKADDRS		m_lpfnGetAcceptExSockAddrs;		// GetAcceptExSockaddrs 的函数指针
	//
	LPFN_TRANSMITFILE				m_lpfnTransmitFile;				// TransmitFile 的函数指针。
};

