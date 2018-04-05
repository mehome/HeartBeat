#ifndef __SOCKET_H__
#define __SOCKET_H__
#include "stdint.h"
#include <string>
#define LEVEL_ERROR		1
#define LEVEL_DEBUG		2
#define LEVEL_INFOR		3
void log(int level, const char* file, const char* func, int lineNo, const char* cFormat, ...);
#define Log(level,cFormat,...) log(level, __FILE__, __FUNCTION__, __LINE__, cFormat, ##__VA_ARGS__);

bool RecvData(SOCKET sock, void*  pData, int32_t nLen);

bool SendData(SOCKET sock, uint32_t dwCmdID, const void* pData, int32_t nLen);


#endif // __SOCKET_H__
