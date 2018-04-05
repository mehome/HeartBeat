#pragma once
#include <winsock2.h>
#include <stdint.h>
#include <string>

#define SEND_BUFF_LEN	8192
#define INFO_LEN		128


class ClientSocket
{
public:
	ClientSocket();
	virtual ~ClientSocket();

	// 加载socket库
	bool LoadSockLib();

	// 卸载socket库
	void UnloadSockLib();

	// 读取配置信息
	bool ReadConfig();

	//客户端初始化，连接服务器
	bool start();

	// 停止客户端，关闭socket
	void stop();
	
private:
	static DWORD WINAPI UserDataThread(LPVOID lpParam);

	//原始的数据传输函数
	bool SendData(uint32_t dwCmdID, const void* pData, uint32_t nLen);
	bool RecvData(void*  pData, uint32_t nLen);
	bool SendRaw(void*  pData, uint32_t nLen);
private:
	std::string						m_strIP;						// 服务器端的IP地址
	unsigned short					m_nPort;						// 服务器端的监听端口
	bool							m_bLibLoaded;					// socket库是否被加载
	SOCKET							m_hClientSock;					// socket套接字
	char							m_szSendBuf[SEND_BUFF_LEN];		// 用于拼接字符串
	fd_set							m_fdAll;
	struct timeval					m_tv;
	int								m_nHeartbeats;
};

