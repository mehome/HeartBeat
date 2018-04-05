#include "stdafx.h"
#include "ClientSocket.h"
#include "Header.h"
#include <iostream>
#include "fstream"
#include "Log.h"
#include <cassert>
#include <cstring>
#include "shlwapi.h"
using namespace std;
#pragma comment(lib,"ws2_32.lib")
// 默认端口
#define DEFAULT_PORT          "12345"
// 默认IP地址
#define DEFAULT_IP            _T("127.0.0.1")
#pragma comment(lib, "Shlwapi.lib")
ClientSocket::ClientSocket()
	:m_bLibLoaded(false),
	m_hClientSock(INVALID_SOCKET)
{
	Log::getInstance().setLogLevel(LEVEL_INFOR);
	ZeroMemory(m_szSendBuf, sizeof(m_szSendBuf));
}

ClientSocket::~ClientSocket()
{
	stop();
	UnloadSockLib();
	Log::getInstance().removeInstance();
}

bool ClientSocket::LoadSockLib()
{
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (NO_ERROR != nResult)
	{
		Loger(LEVEL_ERROR, perro("WSAStartup", WSAGetLastError()).c_str());
		return  m_bLibLoaded = false;
	}
	return m_bLibLoaded = true;
}

void ClientSocket::UnloadSockLib()
{
	if (m_bLibLoaded)
	{
		m_bLibLoaded = false;
		WSACleanup();
	}
}

//#define _WINSOCK_DEPRECATED_NO_WARNINGS
//Project properties->Configuration Properties->C / C++->General->SDL checks->No
//创建socket，连接服务器
bool ClientSocket::start()
{
	// 配置文件不存在
	if (!ReadConfig())
		return false;

	m_hClientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_hClientSock == INVALID_SOCKET)
	{
		Loger(LEVEL_ERROR, perro("创建socket失败.", WSAGetLastError()).c_str());
		return false;
	}

	sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	//已注册端口号：1024~49151
	addr.sin_port = htons(m_nPort);
	addr.sin_addr.S_un.S_addr = inet_addr(m_strIP.c_str());
	if (0 != connect(m_hClientSock, (sockaddr *)&addr, sizeof(addr)))
	{
		Loger(LEVEL_ERROR, perro("连接服务器失败.", WSAGetLastError()).c_str());
		return false;
	}

	FD_ZERO(&m_fdAll);
	FD_SET(m_hClientSock, &m_fdAll);
	DWORD nThreadID = 0;
	HANDLE hThread = ::CreateThread(0, 0, UserDataThread, (void *)this, 0, &nThreadID);
	m_tv.tv_sec = T1;
	m_tv.tv_usec = 0;
	m_nHeartbeats = 0;
	int rc;
	fd_set readfd;
	for (;; )
	{
		
		readfd = m_fdAll;
		rc = select(m_hClientSock + 1, &readfd, NULL, NULL, &m_tv);
		if (rc < 0)
			Loger(LEVEL_ERROR, perro("select failure.", WSAGetLastError()).c_str());
		if (rc == 0)		/* timed out */
		{
			if (++m_nHeartbeats > 3)
			{
				Loger(LEVEL_ERROR, perro("connection dead.", WSAGetLastError()).c_str());
				// 可以关闭socke
				closesocket(m_hClientSock);
				break;
			}
			Loger(LEVEL_INFOR, "sending heartbeat #%d\n", m_nHeartbeats);
			if (!SendData(NET_CMD_HEART_BEAT, NULL, 0))
			{
				Loger(LEVEL_ERROR, perro("send failure.", WSAGetLastError()).c_str());
			}
			m_tv.tv_sec = T2;
			continue;
		}
		if (!FD_ISSET(m_hClientSock, &readfd))
		{
			Loger(LEVEL_ERROR, "select returned invalid socket.");
			continue;
		}
		HEADER header;
		bool res = RecvData(&header, sizeof(header));
		m_tv.tv_sec = T1;
		m_nHeartbeats = 0;
		if (!res)
		{
			closesocket(m_hClientSock);
			break;
		}
		switch (header.dwCmd)
		{
		case NET_CMD_HEART_BEAT:
			Loger(LEVEL_INFOR, "heart beat reply packet");
			break;
		case NET_CMD_DATA:
		{
			char szBuf[MAX_BUFFER_LEN] = { 0 };
			RecvData(szBuf, header.dwLen);
			Loger(LEVEL_INFOR, szBuf);
		}	
			break;
		}
	}
	DWORD nExitCode = 0;
	WaitForSingleObject(hThread,INFINITE);
	GetExitCodeThread(hThread, &nExitCode);
	CloseHandle(hThread);
	return true;
}

DWORD WINAPI ClientSocket::UserDataThread(LPVOID lpParam)
{
	ClientSocket* pClient = (ClientSocket*)lpParam;
	while (true)
	{
		string str;
		cin >> str;
		if (!pClient->SendData(NET_CMD_DATA, str.c_str(), str.size()))
			break;

	}

	return 0;
}

void ClientSocket::stop()
{
	if (INVALID_SOCKET != m_hClientSock)
	{
		closesocket(m_hClientSock);
		m_hClientSock = INVALID_SOCKET;
	}
}

bool ClientSocket::RecvData(void*  pData, uint32_t nLen){
	INT nLeft = nLen;
	LPSTR pTemp = (LPSTR)pData;
	while (nLeft > 0) {
		int nRev = recv(m_hClientSock, pTemp, nLeft, 0);
		if ((nRev == SOCKET_ERROR) || (nRev == 0))
		{
			Loger(LEVEL_ERROR, perro("数据接收失败.", WSAGetLastError()).c_str());
			return false;
		}
		pTemp = pTemp + nRev;
		nLeft = nLeft - nRev;
	}
	return true;
}



bool ClientSocket::SendData(uint32_t dwCmdID, const void* pData, uint32_t nLen)
{
	HEADER header = { 0 };
	header.dwCmd = dwCmdID;
	header.dwLen = nLen;
	uint32_t nSendLen = sizeof(header);
	// 数据头
	memmove(m_szSendBuf, &header, nSendLen);
	// 数据
	memmove(m_szSendBuf + nSendLen, pData, nLen);
	nSendLen += nLen;
	//发送数据包的包头 和 数据，服务器为完成端口，需要一次性发送数据
	if (SendRaw(m_szSendBuf, nSendLen) != true)
	{
		return false;
	}
	return true;
}
bool ClientSocket::SendRaw(void*  pData, uint32_t nLen){
	INT nLeft = nLen;
	LPSTR pTemp = (LPSTR)pData;
	while (nLeft > 0){
		int nSend = send(m_hClientSock, pTemp, nLeft, 0);
		if (nSend == SOCKET_ERROR)
		{
			Loger(LEVEL_ERROR, perro("数据发送失败.", WSAGetLastError()).c_str());
			return false;
		}
		pTemp = pTemp + nSend;
		nLeft = nLeft - nSend;
	}
	return true;
}

// 读取配置信息
bool ClientSocket::ReadConfig()
{
	const uint32_t nLen = 128;
	char szBuf[nLen] = { 0 };
	char szConfigPath[nLen] = { 0 };
	// 检查配置文件是否存在
	int _res = SearchPath(".\\", "UpdateConfigClient.ini", NULL, nLen, szConfigPath, NULL);
	if (_res == 0)
	{
		Loger(LEVEL_ERROR, perro("没有找到配置文件：UpdateConfigClient.ini", WSAGetLastError()).c_str());
		return false;
	}
	// 读取ip地址和端口号
	GetPrivateProfileString("TCP\\IP", "SERVER_IP", DEFAULT_IP, szBuf, nLen, szConfigPath);
	m_strIP = szBuf;
	GetPrivateProfileString("TCP\\IP", "SERVER_PORT", DEFAULT_PORT, szBuf, nLen, szConfigPath);
	m_nPort = atoi(szBuf);
	return true;
}