#include "stdafx.h"
#include "HeartBeat.h"
#include <winsock2.h>
#include <sstream>
#include <iostream>
#include <chrono>
#include <ctime>
#include "Socket.h"
using namespace std;
using namespace std::chrono;


#define		LOG_BUF_SIZE	2048
#define		MAX_BUFF_LEN	8192
char		g_szSendBuf[MAX_BUFF_LEN] = { 0 };
bool SendRaw(SOCKET sock, void*  pData, int32_t nLen);
bool RecvData(SOCKET sock, void*  pData, int32_t nLen) {
	int32_t nLeft = nLen;
	char* pTemp = (char*)pData;
	while (nLeft > 0) {
		int nRev = recv(sock, pTemp, nLeft, 0);
		if ((nRev == SOCKET_ERROR) || (nRev == 0))
		{
			Log(LEVEL_ERROR ,"数据接收失败.");
			return false;
		}
		pTemp = pTemp + nRev;
		nLeft = nLeft - nRev;
	}
	return true;
}

bool SendData(SOCKET sock, uint32_t messgeType, const void* pData, int32_t nLen)
{
	msg_t msg = { 0 };
	msg.type = messgeType;
	int32_t nSendLen = sizeof(msg);
	// 暂时就这么处理，如果数据要大于msg.data长度，再修改
	memmove(msg.data, pData, nLen > sizeof(msg.data) ? sizeof(msg.data) : nLen);
	// 发送固定长度
	if (SendRaw(sock, &msg, nSendLen) != true)
	{
		return false;
	}
	return true;
}

bool SendRaw(SOCKET sock, void*  pData, int32_t nLen) {
	int32_t nLeft = nLen;
	char* pTemp = (char*)pData;
	while (nLeft > 0) {
		int nSend = send(sock, pTemp, nLeft, 0);
		if (nSend == SOCKET_ERROR)
		{
			Log(LEVEL_ERROR, "数据发送失败.");
			return false;
		}
		pTemp = pTemp + nSend;
		nLeft = nLeft - nSend;
	}
	return true;
}


static string getSysTime();
static string perro(char* pszTitle, long nError);
void log(int level, const char* file, const char* func, int lineNo, const char* cFormat, ...)
{
	int nError = WSAGetLastError();
	char levelstr[20];
	if (level)
	{
		switch (level)
		{
		case LEVEL_DEBUG:
			strcpy(levelstr, "[DEBUG] ");
			break;
		case LEVEL_ERROR:
			strcpy(levelstr, "[ERROR] ");
			break;
		case LEVEL_INFOR:
		default:
			strcpy(levelstr, "[INFOR] ");
			break;
		}
	}
	va_list args = NULL;
	ostringstream oss;
	oss << levelstr << file << " [" << lineNo << "][" << func << "] ";
	char logbuf[LOG_BUF_SIZE];
	memset(logbuf, 0, sizeof(logbuf));
	string&& sysTime = getSysTime();
	sprintf(logbuf, "%s ", sysTime.c_str());
	va_start(args, cFormat);
	int lens = strlen(logbuf);
	vsnprintf(logbuf + lens, LOG_BUF_SIZE - 1 - lens, cFormat, args);// last bit'\0'
	va_end(args);
	oss << logbuf;
	if (LEVEL_ERROR == level)
		oss << perro("", nError);
	cout << oss.str() << endl;


}

static string getSysTime()
{
	time_t tt = system_clock::to_time_t(system_clock::now());
	char timeBuf[LOG_BUF_SIZE];
	memset(timeBuf, 0, sizeof(timeBuf));
	struct tm* pTime = localtime(&tt);
	sprintf(timeBuf, "%4d/%02d/%02d %02d:%02d:%02d", pTime->tm_year + 1900, pTime->tm_mon + 1,
		pTime->tm_mday, pTime->tm_hour, pTime->tm_min, pTime->tm_sec);
	return string(timeBuf);
}

static string perro(char* pszTitle, long nError) {
	LPVOID pvErrMsg = NULL;
	if (nError < 0)
		nError = GetLastError();
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, nError,
		MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
		(char*)&pvErrMsg,
		0, NULL);
	char szText[256];
	sprintf(szText, "%s(errno=%d),%s\n", pszTitle, nError, (char*)pvErrMsg);
	LocalFree(pvErrMsg);
	return szText;
}
